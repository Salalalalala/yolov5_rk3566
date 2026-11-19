/* rknn_detector.c —— YOLOv5s @ RK3566 NPU 检测模块
 *
 * 整条流水线：
 *
 *   V4L2 帧(NV12 640x480)
 *        │
 *        ├─ ① RGA letterbox：NV12 → RGB888 640x640（等比缩放 + 灰边，硬件完成）
 *        │
 *        ├─ ② RKNN 推理：输入 UINT8 NHWC，模型内部已固化 /255 归一化
 *        │      输出 3 个 head 的原始 logits：
 *        │        [1,255,80,80] [1,255,40,40] [1,255,20,20]
 *        │
 *        ├─ ③ CPU 解码：sigmoid + 网格/anchor 还原 → 置信度过滤
 *        │
 *        ├─ ④ NMS：按类别做贪心抑制
 *        │
 *        └─ ⑤ 坐标反映射：640x640 → 原帧 640x480，并画框
 *
 * 为什么模型要输出 raw head（3 个输出）而不是训练框架默认的 (1,25200,85)：
 *   RKNN 1.5.0 的量化参数是 per-tensor 的（rknn_tensor_attr.scale 是标量），
 *   官方输出把 box 坐标(0~640)和置信度(0~1)拼在同一个张量里，坐标会把量化
 *   步长撑到 ~2.5，置信度全部被量化为 0（板端实测 obj 恒为 0）。
 *   改成 3 个 head 的原始 logits 后，每个输出张量取值范围一致，int8 正常。
 */
#include "rknn_detector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rga/im2d.h>     /* RGA 2.x im2d API：wrapbuffer_virtualaddr / improcess */

#include "rknn_api.h"     /* include/rknn_api.h，1.5.0，与板端 librknnrt 版本一致 */

/* ---- 后处理参数（如无特殊需求不要改） ---- */
#define OBJ_THRESH      0.25f   /* 置信度阈值：score = obj * cls */
#define NMS_IOU_THRESH  0.45f   /* IoU 超过该值视为同一目标 */
#define MAX_CANDIDATES  256     /* 阈值过滤后的候选框上限，防止数组溢出 */
#define YOLO_NCH        85      /* 每个 anchor 的输出通道：4(box) + 1(obj) + 80(cls) */
#define YOLO_NCLS       80      /* COCO 类别数 */

/* YOLOv5s 三个检测层（P3/P4/P5）的 anchor，单位是像素。
 * 注意：这个 checkpoint（v6/v7）里 Detect.anchors 存的是"grid 单位"（1.25, 1.625...），
 * 模型内部 anchor_grid = anchors * stride 后才是这里的像素值。因此解码时
 * wh = (2*sigmoid)^2 * anchor，不要再乘 stride（乘了会大 8/16/32 倍）。 */
static const float k_anchors[3][6] = {
    { 10, 13,  16, 30,  33, 23 },    /* stride 8  */
    { 30, 61,  62, 45,  59, 119 },   /* stride 16 */
    {116, 90, 156, 198, 373, 326 },  /* stride 32 */
};

/* 一个候选框（内部结构，坐标仍在 640x640 模型输入坐标系） */
typedef struct {
    float x1, y1, x2, y2;
    float score;
    int   cls;
} candidate_t;

/* 检测器句柄：把 NPU 上下文和所有复用的缓冲区集中在一起，
 * 避免每帧 malloc/free（30fps 下这点很重要）。 */
struct yolo_detector {
    rknn_context ctx;          /* NPU 上下文 */
    uint8_t     *model_data;   /* .rknn 文件内容（rknn_init 后仍保留，destroy 时释放） */
    size_t       model_size;

    int in_w, in_h;            /* 模型输入宽高（本模型 640x640） */
    uint8_t *rgb;              /* letterbox 后的 RGB888 输入缓冲，in_w*in_h*3 */

    int n_out;                 /* 输出个数，raw-head 模型固定为 3 */
    rknn_tensor_attr out_attr[3];

    /* 解码用的临时数组（按最大 head 的网格分配，每帧复用，避免栈上开大数组）。
     * 之所以按"通道优先"的方式解码，见 decode_head 的注释。 */
    int      scratch_plane;    /* 最大 head 的像素数（80x80=6400） */
    float   *scratch_obj;      /* 每个像素的 obj 概率 */
    float   *scratch_best;     /* 每个像素的类别最大 logit */
    uint8_t *scratch_cls;      /* 每个像素的类别下标 */
};

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

static inline float sigmoid_f(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

/* int8 反量化：q 为模型输出的原始定点值，按该输出张量的 scale/zp 还原成 float。
 * 1.5.0 是 per-tensor 量化，scale/zp 都是标量（见 rknn_api.h 的 rknn_tensor_attr）。 */
static inline float dequant_i8(const rknn_tensor_attr *a, int8_t q)
{
    return ((float)q - (float)a->zp) * a->scale;
}

static inline float box_iou(const candidate_t *a, const candidate_t *b)
{
    float ix1 = fmaxf(a->x1, b->x1);
    float iy1 = fmaxf(a->y1, b->y1);
    float ix2 = fminf(a->x2, b->x2);
    float iy2 = fminf(a->y2, b->y2);
    float iw  = ix2 - ix1;
    float ih  = iy2 - iy1;
    if (iw <= 0 || ih <= 0)
        return 0.0f;
    float inter = iw * ih;
    float area_a = (a->x2 - a->x1) * (a->y2 - a->y1);
    float area_b = (b->x2 - b->x1) * (b->y2 - b->y1);
    return inter / (area_a + area_b - inter + 1e-6f);
}

/* qsort 用：按 score 从大到小 */
static int cand_cmp_desc(const void *pa, const void *pb)
{
    const candidate_t *a = pa, *b = pb;
    return (a->score < b->score) - (a->score > b->score);
}

/* NV12 的 Y 行宽（stride）。rkisp 输出的 buffer 可能按对齐要求留 padding，
 * 用 "总字节数 = stride * height * 3/2" 反推，避免直接假设 stride == width。 */
static int nv12_stride(size_t nv12_size, int width, int height)
{
    size_t expected = (size_t)width * height * 3 / 2;
    if (height > 0 && nv12_size >= expected) {
        int s = (int)(nv12_size * 2 / (3 * (size_t)height));
        if (s >= width)
            return s;
    }
    return width;
}

/* ------------------------------------------------------------------ */
/* 创建 / 销毁                                                        */
/* ------------------------------------------------------------------ */

yolo_detector *yolo_detector_create(const char *model_path)
{
    if (!model_path)
        return NULL;

    /* 1. 把 .rknn 整个读进内存：rknn_init(1.5.0) 只接受内存指针，没有路径接口。 */
    FILE *fp = fopen(model_path, "rb");
    if (!fp) {
        perror(model_path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fprintf(stderr, "模型文件 %s 大小异常\n", model_path);
        fclose(fp);
        return NULL;
    }
    uint8_t *model = malloc((size_t)sz);
    if (!model || fread(model, 1, (size_t)sz, fp) != (size_t)sz) {
        fprintf(stderr, "读取模型 %s 失败\n", model_path);
        free(model);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    yolo_detector *d = calloc(1, sizeof(*d));
    if (!d) {
        free(model);
        return NULL;
    }
    d->model_data = model;
    d->model_size = (size_t)sz;

    /* 2. 初始化 NPU。返回 0 (RKNN_SUCC) 表示成功。 */
    int ret = rknn_init(&d->ctx, model, (uint32_t)sz, 0, NULL);
    if (ret != 0) {
        fprintf(stderr, "rknn_init 失败: %d（检查模型是否 1.5.0 转换、板端 runtime 版本）\n", ret);
        goto fail;
    }

    /* 3. 查询输入/输出个数，校验是不是 raw-head 模型。 */
    rknn_input_output_num io;
    memset(&io, 0, sizeof(io));
    if (rknn_query(d->ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) != 0) {
        fprintf(stderr, "rknn_query(IN_OUT_NUM) 失败\n");
        goto fail;
    }
    if (io.n_output != 3) {
        fprintf(stderr, "模型有 %u 个输出，期望 3 个（raw-head）。"
                        "请用 export_onnx.py 重新导出。\n", io.n_output);
        goto fail;
    }
    d->n_out = (int)io.n_output;

    /* 4. 取模型输入尺寸。模型是 NCHW（1x3x640x640），但 fmt 字段决定 dims 的含义，
     *    这里兼容 NHWC/NCHW 两种描述，避免写死 640。 */
    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    if (rknn_query(d->ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr)) != 0) {
        fprintf(stderr, "rknn_query(INPUT_ATTR) 失败\n");
        goto fail;
    }
    if (in_attr.fmt == RKNN_TENSOR_NHWC) {
        d->in_h = (int)in_attr.dims[1];
        d->in_w = (int)in_attr.dims[2];
    } else {
        d->in_h = (int)in_attr.dims[2];
        d->in_w = (int)in_attr.dims[3];
    }
    if (d->in_w <= 0 || d->in_h <= 0) {
        fprintf(stderr, "模型输入尺寸异常: %dx%d\n", d->in_w, d->in_h);
        goto fail;
    }

    /* 5. 预分配 RGB 输入缓冲，避免每帧 malloc。 */
    d->rgb = malloc((size_t)d->in_w * d->in_h * 3);
    if (!d->rgb) {
        fprintf(stderr, "分配输入缓冲失败\n");
        goto fail;
    }

    /* 6. 记录每个输出的属性（后面解码要用 dims 推 head 的网格大小）。 */
    for (int i = 0; i < d->n_out; i++) {
        memset(&d->out_attr[i], 0, sizeof(d->out_attr[i]));
        d->out_attr[i].index = i;
        if (rknn_query(d->ctx, RKNN_QUERY_OUTPUT_ATTR,
                       &d->out_attr[i], sizeof(d->out_attr[i])) != 0) {
            fprintf(stderr, "rknn_query(OUTPUT_ATTR, %d) 失败\n", i);
            goto fail;
        }
    }

    /* 7. 校验输出是 int8 + 仿射量化。后面直接读 int8 原始数据手工反量化，
     *    比 want_float=1 省一次每帧 8.5MB 的 int8→float 转换和内存分配。 */
    int max_plane = 0;
    for (int i = 0; i < d->n_out; i++) {
        rknn_tensor_attr *a = &d->out_attr[i];
        if (a->type != RKNN_TENSOR_INT8 ||
            a->qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
            fprintf(stderr, "输出 %d 不是 int8/affine 量化 (type=%d qnt=%d)，"
                            "需要 int8 量化模型\n", i, a->type, a->qnt_type);
            goto fail;
        }
        int plane = (int)a->dims[2] * (int)a->dims[3];
        if (plane > max_plane)
            max_plane = plane;
    }

    /* 8. 按最大 head 分配解码临时数组（80x80=6400 个元素，每帧复用）。 */
    d->scratch_plane = max_plane;
    d->scratch_obj  = malloc(sizeof(float) * (size_t)max_plane);
    d->scratch_best = malloc(sizeof(float) * (size_t)max_plane);
    d->scratch_cls  = malloc(sizeof(uint8_t) * (size_t)max_plane);
    if (!d->scratch_obj || !d->scratch_best || !d->scratch_cls) {
        fprintf(stderr, "分配解码临时数组失败\n");
        goto fail;
    }

    printf("RKNN 模型已加载: %s，输入 %dx%d，输出 %d 个 head\n",
           model_path, d->in_w, d->in_h, d->n_out);
    return d;

fail:
    yolo_detector_destroy(d);
    return NULL;
}

void yolo_detector_destroy(yolo_detector *d)
{
    if (!d)
        return;
    if (d->ctx)
        rknn_destroy(d->ctx);
    free(d->scratch_obj);
    free(d->scratch_best);
    free(d->scratch_cls);
    free(d->rgb);
    free(d->model_data);
    free(d);
}

/* ------------------------------------------------------------------ */
/* 单个 head 的解码                                                   */
/* ------------------------------------------------------------------ */

/* 输入 p 指向一个 head 的 int8 输出，布局 [1, 3*85, ny, nx]（NCHW）：
 *   p[(a*85 + c) * ny*nx + y*nx + x]
 * 其中 a=anchor 序号(0~2)，c=通道(0~84)，需要按 out_attr 的 scale/zp 反量化。
 *
 * YOLOv5 v7 的解码公式（在 Detect.forward 里）：
 *   xy = (sigmoid(xy_logit) * 2 - 0.5 + 网格坐标) * stride
 *   wh = (sigmoid(wh_logit) * 2)^2 * anchor（anchor 为像素值，见 k_anchors 注释）
 * 注意模型输出的是 logits，sigmoid 要在这里做（raw-head 导出时去掉了）。
 *
 * 性能要点：若按"每个位置扫 80 个类别"写，每次读类别通道都会跨 6.4KB（80x80），
 * 25200 个位置 × 80 次全是 cache miss，实测单帧会飙到 160ms+。改成"通道优先"：
 *   ① 整幅算 obj 概率（连续读）；
 *   ② 逐类别通道刷最大值（每次连续读 6400 字节，刚好在 L1/L2）；
 *   ③ 只对过阈值的位置做少量反量化，还原框。
 */
static void decode_head(const yolo_detector *d, int out_index, const int8_t *p,
                        int img_w, int img_h, float scale, int pad_x, int pad_y,
                        candidate_t *cands, int *ncand)
{
    const rknn_tensor_attr *attr = &d->out_attr[out_index];
    int ny = (int)attr->dims[2];
    int nx = (int)attr->dims[3];
    if (ny <= 0 || nx <= 0)
        return;

    int plane  = ny * nx;            /* 单个通道的像素数 */
    if (plane > d->scratch_plane)
        return;
    int stride = d->in_w / nx;       /* 该 head 的步长：80->8, 40->16, 20->32 */
    int ai     = (stride == 8) ? 0 : (stride == 16) ? 1 : 2;
    const float *anc = k_anchors[ai];

    float   *obj  = d->scratch_obj;
    float   *best = d->scratch_best;
    uint8_t *bcls = d->scratch_cls;

    for (int a = 0; a < 3; a++) {
        const int8_t *pa = p + (size_t)(a * YOLO_NCH) * plane;

        /* ① obj 通道（第 4 通道）整幅反量化 + sigmoid */
        const int8_t *ch_obj = pa + (size_t)4 * plane;
        for (int i = 0; i < plane; i++)
            obj[i] = sigmoid_f(dequant_i8(attr, ch_obj[i]));

        /* ② 80 个类别通道逐通道比较，记录最大 logit 和类别号 */
        const int8_t *ch_cls = pa + (size_t)5 * plane;
        for (int i = 0; i < plane; i++) {
            best[i] = dequant_i8(attr, ch_cls[i]);
            bcls[i] = 0;
        }
        for (int c = 1; c < YOLO_NCLS; c++) {
            const int8_t *ch = ch_cls + (size_t)c * plane;
            for (int i = 0; i < plane; i++) {
                float v = dequant_i8(attr, ch[i]);
                if (v > best[i]) {
                    best[i] = v;
                    bcls[i] = (uint8_t)c;
                }
            }
        }

        /* ③ 置信度过滤 + 网格/anchor 解码 */
        for (int y = 0; y < ny; y++) {
            for (int x = 0; x < nx; x++) {
                const int q = y * nx + x;

                if (obj[q] < 0.05f)   /* 粗筛：obj 太小时 obj*cls 不可能超过阈值 */
                    continue;
                float score = obj[q] * sigmoid_f(best[q]);
                if (score < OBJ_THRESH)
                    continue;

                /* 框中心/宽高（模型输入 640x640 坐标系） */
                float bx = (sigmoid_f(dequant_i8(attr, pa[q])) * 2.0f - 0.5f + x) * stride;
                float by = (sigmoid_f(dequant_i8(attr, pa[plane + q])) * 2.0f - 0.5f + y) * stride;
                float bw = powf(sigmoid_f(dequant_i8(attr, pa[2 * plane + q])) * 2.0f, 2.0f)
                           * anc[a * 2];
                float bh = powf(sigmoid_f(dequant_i8(attr, pa[3 * plane + q])) * 2.0f, 2.0f)
                           * anc[a * 2 + 1];

                /* 反映射：去掉 letterbox 灰边再除以缩放比 → 原帧坐标 */
                float x1 = (bx - bw * 0.5f - pad_x) / scale;
                float y1 = (by - bh * 0.5f - pad_y) / scale;
                float x2 = (bx + bw * 0.5f - pad_x) / scale;
                float y2 = (by + bh * 0.5f - pad_y) / scale;

                /* 完全落在灰边里的框丢弃 */
                if (x2 <= 0 || y2 <= 0 || x1 >= img_w || y1 >= img_h)
                    continue;

                /* 裁剪到画面内，避免画框越界 */
                x1 = fmaxf(x1, 0.0f);
                y1 = fmaxf(y1, 0.0f);
                x2 = fminf(x2, (float)(img_w - 1));
                y2 = fminf(y2, (float)(img_h - 1));

                cands[*ncand].x1    = x1;
                cands[*ncand].y1    = y1;
                cands[*ncand].x2    = x2;
                cands[*ncand].y2    = y2;
                cands[*ncand].score = score;
                cands[*ncand].cls   = bcls[q];
                (*ncand)++;

                if (*ncand >= MAX_CANDIDATES)
                    return;           /* 候选框满了，剩余位置丢弃 */
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 推理入口                                                           */
/* ------------------------------------------------------------------ */

int yolo_detector_run(yolo_detector *d, const uint8_t *nv12, size_t nv12_size,
                      int width, int height, yolo_box *boxes, int max_boxes,
                      int *out_num)
{
    if (!d || !nv12 || !boxes || !out_num || width <= 0 || height <= 0)
        return -1;
    *out_num = 0;

    /* 1. letterbox 参数：等比缩放，居中留灰边。
     *    640x480 -> 640x640 时 scale=1，上下各 80 像素灰边。 */
    float scale = fminf((float)d->in_w / width, (float)d->in_h / height);
    int new_w = (int)lroundf(width * scale);
    int new_h = (int)lroundf(height * scale);
    if (new_w > d->in_w) new_w = d->in_w;
    if (new_h > d->in_h) new_h = d->in_h;
    int pad_x = (d->in_w - new_w) / 2;
    int pad_y = (d->in_h - new_h) / 2;

    /* 2. 背景先填 (114,114,114)：RGA 只往 drect 里写缩放后的图像，
     *    矩形之外保持这里的颜色。YOLO 训练时的 letterbox 也是填 114。 */
    memset(d->rgb, 114, (size_t)d->in_w * d->in_h * 3);

    /* 3. RGA 硬件转换 + 缩放。improcess 支持指定 src/dst 矩形，
     *    这是实现 letterbox 的关键：dst 只接收缩放后的区域，其余留灰。 */
    rga_buffer_t src = wrapbuffer_virtualaddr((void *)nv12, width, height,
                                              RK_FORMAT_YCbCr_420_SP,
                                              nv12_stride(nv12_size, width, height),
                                              height);
    rga_buffer_t dst = wrapbuffer_virtualaddr(d->rgb, d->in_w, d->in_h,
                                              RK_FORMAT_RGB_888);
    rga_buffer_t pat = {0};
    im_rect srect = { 0, 0, width, height };
    im_rect drect = { pad_x, pad_y, new_w, new_h };
    im_rect prect = { 0, 0, 0, 0 };
    IM_STATUS st = improcess(src, dst, pat, srect, drect, prect, IM_SYNC);
    if (st != IM_STATUS_SUCCESS) {
        fprintf(stderr, "RGA letterbox 失败: %d\n", (int)st);
        return -1;
    }

    /* 4. 送 NPU。
     *    - type=UINT8：传 0~255 原始像素；
     *    - fmt=NHWC：RGA 输出就是 NHWC 的 RGB888；
     *    - pass_through=0：让 runtime 按量化参数做 uint8 -> int8 转换
     *      （/255 归一化已经固化进模型的量化参数里了）。 */
    rknn_input in;
    memset(&in, 0, sizeof(in));
    in.index        = 0;
    in.buf          = d->rgb;
    in.size         = (uint32_t)(d->in_w * d->in_h * 3);
    in.type         = RKNN_TENSOR_UINT8;
    in.fmt          = RKNN_TENSOR_NHWC;
    in.pass_through = 0;
    if (rknn_inputs_set(d->ctx, 1, &in) != 0) {
        fprintf(stderr, "rknn_inputs_set 失败\n");
        return -1;
    }
    if (rknn_run(d->ctx, NULL) != 0) {
        fprintf(stderr, "rknn_run 失败\n");
        return -1;
    }

    /* 5. 取输出。want_float=0 直接拿 int8 原始数据，在解码时用 scale/zp 反量化：
     *    这样每帧只搬 ~2.1MB，而不是 8.5MB 的 float32，
     *    也省掉 runtime 内部的一次大块 malloc + 转换。 */
    rknn_output outs[3];
    memset(outs, 0, sizeof(outs));
    for (int i = 0; i < d->n_out; i++) {
        outs[i].index      = i;
        outs[i].want_float = 0;
    }
    if (rknn_outputs_get(d->ctx, d->n_out, outs, NULL) != 0) {
        fprintf(stderr, "rknn_outputs_get 失败\n");
        return -1;
    }

    /* 6. 解码三个 head */
    candidate_t cands[MAX_CANDIDATES];
    int ncand = 0;
    for (int i = 0; i < d->n_out && ncand < MAX_CANDIDATES; i++)
        decode_head(d, i, (const int8_t *)outs[i].buf, width, height,
                    scale, pad_x, pad_y, cands, &ncand);

    rknn_outputs_release(d->ctx, d->n_out, outs);

    /* 7. NMS：先按分数降序，再逐类贪心抑制。
     *    score 置 -1 表示该候选已被抑制。 */
    if (ncand > 0)
        qsort(cands, (size_t)ncand, sizeof(candidate_t), cand_cmp_desc);

    int kept = 0;
    for (int i = 0; i < ncand && kept < max_boxes; i++) {
        if (cands[i].score < 0)
            continue;
        boxes[kept].x1       = cands[i].x1;
        boxes[kept].y1       = cands[i].y1;
        boxes[kept].x2       = cands[i].x2;
        boxes[kept].y2       = cands[i].y2;
        boxes[kept].class_id = cands[i].cls;
        boxes[kept].score    = cands[i].score;
        kept++;

        for (int j = i + 1; j < ncand; j++) {
            if (cands[j].score < 0 || cands[j].cls != cands[i].cls)
                continue;
            if (box_iou(&cands[i], &cands[j]) > NMS_IOU_THRESH)
                cands[j].score = -1;
        }
    }

    *out_num = kept;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 画框（直接写 NV12 的 Y 平面）                                      */
/* ------------------------------------------------------------------ */

/* 在 Y 平面上画一个空心矩形（只改亮度，UV 保持不变）。 */
static void draw_rect_y(uint8_t *y_plane, int stride, int w, int h,
                        int x1, int y1, int x2, int y2, int thickness)
{
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= w) x2 = w - 1;
    if (y2 >= h) y2 = h - 1;
    if (x1 > x2 || y1 > y2 || thickness <= 0)
        return;

    for (int k = 0; k < thickness; k++) {
        int top = y1 + k, bot = y2 - k;
        int lft = x1 + k, rgt = x2 - k;
        if (top > bot || lft > rgt)
            break;
        /* 上下两条横线 */
        memset(y_plane + (size_t)top * stride + lft, 255, (size_t)(rgt - lft + 1));
        memset(y_plane + (size_t)bot * stride + lft, 255, (size_t)(rgt - lft + 1));
        /* 左右两条竖线 */
        for (int r = top; r <= bot; r++) {
            y_plane[(size_t)r * stride + lft] = 255;
            y_plane[(size_t)r * stride + rgt] = 255;
        }
    }
}

void yolo_draw_nv12(uint8_t *nv12, size_t nv12_size, int width, int height,
                    const yolo_box *boxes, int num, int thickness)
{
    if (!nv12 || !boxes || num <= 0)
        return;

    int stride   = nv12_stride(nv12_size, width, height);
    uint8_t *yp  = nv12;   /* NV12 的 Y 平面在最前面，尺寸 width*height */

    for (int i = 0; i < num; i++) {
        draw_rect_y(yp, stride, width, height,
                    (int)lroundf(boxes[i].x1), (int)lroundf(boxes[i].y1),
                    (int)lroundf(boxes[i].x2), (int)lroundf(boxes[i].y2),
                    thickness);
    }
}
