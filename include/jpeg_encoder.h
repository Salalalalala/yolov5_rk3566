#ifndef JPEG_ENCODER_H
#define JPEG_ENCODER_H

#include <stddef.h>
#include <stdint.h>

/*
 * 硬件 JPEG 编码器（基于 Rockchip MPP，RK3566 VPU 硬件加速）。
 *
 * 与软件版不同，MPP 的初始化很贵（创建上下文、配置、申请 DMA 缓冲），
 * 所以接口是"创建一次、编码多帧"的有状态形式：
 *   create → encode(循环) → destroy
 */

typedef struct jpeg_encoder jpeg_encoder;   /* 不透明句柄 */

/* 创建编码器并初始化 MPP 硬件 JPEG 编码。
 * 入参:
 *   w, h    : 图像宽高（像素），输入为 NV12
 *   quality : JPEG 质量 1..99，越大越清晰、文件越大（预览建议 80）
 * 返回: 句柄；失败返回 NULL（stderr 已打印原因）。 */
jpeg_encoder *jpeg_encoder_create(int w, int h, int quality);

/* 编码一帧 NV12 为 JPEG。结果 malloc 分配，调用者 free()。
 * 入参:
 *   enc  : 句柄
 *   nv12 : 输入帧首地址（w*h*3/2 字节，Y 平面 + 交错 UV）
 * 出参:
 *   out     : 指向 malloc 出的 JPEG 缓冲区
 *   out_len : JPEG 字节数
 * 返回: 0 成功，-1 失败。 */
int jpeg_encoder_encode(jpeg_encoder *enc, const uint8_t *nv12,
                        uint8_t **out, size_t *out_len);

/* 销毁编码器，释放 MPP 硬件资源。入参可为 NULL（忽略）。 */
void jpeg_encoder_destroy(jpeg_encoder *enc);

#endif /* JPEG_ENCODER_H */
