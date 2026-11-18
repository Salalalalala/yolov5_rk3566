/* jpeg_encoder.c —— 硬件 JPEG 编码器（Rockchip MPP）
 *
 * 用 RK3566 的 VPU 硬件做 NV12 → JPEG 编码，不占 CPU。
 *
 * 关键点:
 *   - MPP 1.5.0 用新的"字符串键"配置 API（mpp_enc_cfg_set_s32），
 *     旧的结构体配置已废弃。
 *   - 输入缓冲用 DRM 内存（本板内核 4.19 有 /dev/dri，无 ION），
 *     拷贝一次 NV12 后 DMA 直接交给 VPU。
 *   - 编码走同步接口 mpi->encode()：提交帧 + 取回包一步完成。
 */
#define MODULE_TAG "jpeg_encoder"   /* MPP 日志用的模块名，必须在使用前定义 */

#include "jpeg_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

struct jpeg_encoder {
    MppCtx          ctx;          /* MPP 上下文 */
    MppApi         *mpi;          /* MPP 接口函数表 */
    MppBufferGroup  grp;          /* 输入缓冲区组 */
    MppBuffer       buf;          /* 输入缓冲（放 NV12 数据） */
    MppFrame        frame;        /* 输入帧描述 */
    int w, h;                     /* 图像宽高 */
    int hor_stride, ver_stride;   /* 对齐后的步长 */
    int quality;                  /* 质量 1..99 */
};

#define ALIGN16(x) (((x) + 15) & ~15)

jpeg_encoder *jpeg_encoder_create(int w, int h, int quality)
{
    if (w <= 0 || h <= 0)
        return NULL;
    if (quality < 1)  quality = 1;
    if (quality > 99) quality = 99;

    jpeg_encoder *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->w = w;
    e->h = h;
    e->quality = quality;
    e->hor_stride = ALIGN16(w);
    e->ver_stride = ALIGN16(h);

    MPP_RET ret = mpp_create(&e->ctx, &e->mpi);
    if (ret) { fprintf(stderr, "[mpp] mpp_create 失败 ret=%d\n", ret); goto fail; }

    ret = mpp_init(e->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG);
    if (ret) { fprintf(stderr, "[mpp] mpp_init 失败 ret=%d\n", ret); goto fail; }

    /* 配置编码参数（字符串键） */
    MppEncCfg cfg = NULL;
    mpp_enc_cfg_init(&cfg);
    mpp_enc_cfg_set_s32(cfg, "prep:width",      w);
    mpp_enc_cfg_set_s32(cfg, "prep:height",     h);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", e->hor_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", e->ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format",     MPP_FMT_YUV420SP);   /* NV12 */
    mpp_enc_cfg_set_s32(cfg, "codec:type",      MPP_VIDEO_CodingMJPEG);
    mpp_enc_cfg_set_s32(cfg, "rc:mode",         MPP_ENC_RC_MODE_FIXQP);
    mpp_enc_cfg_set_s32(cfg, "jpeg:q_factor",   quality);
    mpp_enc_cfg_set_s32(cfg, "jpeg:qf_max",     99);
    mpp_enc_cfg_set_s32(cfg, "jpeg:qf_min",     1);
    ret = e->mpi->control(e->ctx, MPP_ENC_SET_CFG, cfg);
    mpp_enc_cfg_deinit(cfg);
    if (ret) { fprintf(stderr, "[mpp] 设置编码配置失败 ret=%d\n", ret); goto fail; }

    /* 输入缓冲区（DRM 内存，DMA 直接给 VPU） */
    ret = mpp_buffer_group_get_internal(&e->grp, MPP_BUFFER_TYPE_DRM);
    if (ret) { fprintf(stderr, "[mpp] 创建缓冲区组失败 ret=%d\n", ret); goto fail; }
    size_t buf_size = (size_t)e->hor_stride * e->ver_stride * 3 / 2;
    ret = mpp_buffer_get(e->grp, &e->buf, buf_size);
    if (ret) { fprintf(stderr, "[mpp] 申请输入缓冲失败 ret=%d\n", ret); goto fail; }

    /* 输入帧描述：宽高/步长/格式/缓冲区 */
    mpp_frame_init(&e->frame);
    mpp_frame_set_width(e->frame,       w);
    mpp_frame_set_height(e->frame,      h);
    mpp_frame_set_hor_stride(e->frame,  e->hor_stride);
    mpp_frame_set_ver_stride(e->frame,  e->ver_stride);
    mpp_frame_set_fmt(e->frame,         MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(e->frame,      e->buf);

    printf("[mpp] 硬件 JPEG 编码器就绪 %dx%d stride=%dx%d q=%d\n",
           w, h, e->hor_stride, e->ver_stride, quality);
    return e;

fail:
    jpeg_encoder_destroy(e);
    return NULL;
}

int jpeg_encoder_encode(jpeg_encoder *e, const uint8_t *nv12,
                        uint8_t **out, size_t *out_len)
{
    if (!e || !nv12 || !out || !out_len)
        return -1;

    /* 1. 把 NV12 拷进 DMA 输入缓冲（Y、UV 都要按步长摆放） */
    uint8_t *dst    = mpp_buffer_get_ptr(e->buf);
    uint8_t *y_dst  = dst;
    uint8_t *uv_dst = dst + (size_t)e->hor_stride * e->ver_stride;
    const uint8_t *y_src  = nv12;
    const uint8_t *uv_src = nv12 + (size_t)e->w * e->h;

    for (int r = 0; r < e->h; r++)
        memcpy(y_dst + (size_t)r * e->hor_stride, y_src + (size_t)r * e->w, (size_t)e->w);
    for (int r = 0; r < e->h / 2; r++)
        memcpy(uv_dst + (size_t)r * e->hor_stride, uv_src + (size_t)r * e->w, (size_t)e->w);

    /* 2. 提交帧 + 取回结果包。
     * 注意：同步接口 mpi->encode() 对 JPEG 会返回 NULL 包（本板 MPP 1.5.0），
     * 必须用异步的 put_frame + get_packet 两步。 */
    MppPacket packet = NULL;
    MPP_RET ret = e->mpi->encode_put_frame(e->ctx, e->frame);
    if (ret) {
        fprintf(stderr, "[mpp] encode_put_frame 失败 ret=%d\n", ret);
        return -1;
    }
    ret = e->mpi->encode_get_packet(e->ctx, &packet);
    if (ret || !packet) {
        fprintf(stderr, "[mpp] encode_get_packet 失败 ret=%d packet=%p\n",
                ret, (void *)packet);
        return -1;
    }

    /* 3. 拷出 JPEG 数据（必须在 deinit 前拷贝） */
    size_t len  = mpp_packet_get_length(packet);
    void  *data = mpp_packet_get_data(packet);
    uint8_t *jpeg = malloc(len ? len : 1);
    if (!jpeg) {
        mpp_packet_deinit(&packet);
        return -1;
    }
    if (len && data)
        memcpy(jpeg, data, len);
    mpp_packet_deinit(&packet);

    *out = jpeg;
    *out_len = len;
    return 0;
}

void jpeg_encoder_destroy(jpeg_encoder *e)
{
    if (!e)
        return;
    if (e->frame) mpp_frame_deinit(&e->frame);
    if (e->buf)   mpp_buffer_put(e->buf);
    if (e->grp)   mpp_buffer_group_put(e->grp);
    if (e->ctx)   mpp_destroy(e->ctx);
    free(e);
}
