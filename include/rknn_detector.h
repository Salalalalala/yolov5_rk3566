#ifndef RKNN_DETECTOR_H
#define RKNN_DETECTOR_H

#include <stddef.h>
#include <stdint.h>

/* 一帧最多返回多少个检测框（超出部分在 NMS 阶段丢弃） */
#define YOLO_MAX_BOXES 64

/* 一个检测框。
 * 坐标已经换算回"摄像头原始帧"的像素坐标系（左上原点），
 * 不是模型输入 640x640 的坐标系。 */
typedef struct {
    float x1, y1;     /* 左上角 */
    float x2, y2;     /* 右下角 */
    int   class_id;   /* COCO 类别 0~79 */
    float score;      /* obj * cls，0~1 */
} yolo_box;

/* 不透明句柄：内部持有 rknn_context、输入缓冲、输出属性等，
 * 调用方不需要（也不应该）关心内部结构，方便以后换模型/换实现。 */
typedef struct yolo_detector yolo_detector;

/* 加载 .rknn 模型并完成 NPU 初始化。
 *
 * 成功返回句柄，失败返回 NULL（错误原因打印到 stderr）。
 * 模型必须是 export_onnx.py 导出的 raw-head 模型（3 个输出），
 * 原因见 export_onnx.py 文件头说明。 */
yolo_detector *yolo_detector_create(const char *model_path);

/* 释放 NPU 上下文和全部内存。允许传 NULL。 */
void yolo_detector_destroy(yolo_detector *det);

/* 对一帧 NV12 做检测。
 *
 *   nv12/nv12_size : V4L2 取到的整帧数据（Y 平面 + 交错的 UV 平面）
 *   width/height   : 帧宽高（本工程为 640x480）
 *   boxes/max_boxes: 输出数组及容量（建议 YOLO_MAX_BOXES）
 *   out_num        : 实际写入 boxes 的个数
 *
 * 返回 0 成功，-1 失败（RGA/RKNN 出错，错误原因打印到 stderr）。
 * 内部流程：RGA letterbox(NV12->RGB888 640x640) → rknn_run → 3 head 解码 → NMS。 */
int yolo_detector_run(yolo_detector *det, const uint8_t *nv12, size_t nv12_size,
                      int width, int height, yolo_box *boxes, int max_boxes,
                      int *out_num);

/* 把检测框直接画到 NV12 帧的 Y 平面上（原地修改，用于编码前叠加显示）。
 *
 * 只写 Y（亮度）通道，UV 不动：白框在水面/白墙等亮背景上可能不明显，
 * 但不需要额外的 RGB 转换，开销极小（就是几行 memset）。
 * thickness 为线宽像素。 */
void yolo_draw_nv12(uint8_t *nv12, size_t nv12_size, int width, int height,
                    const yolo_box *boxes, int num, int thickness);

#endif /* RKNN_DETECTOR_H */
