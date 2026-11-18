#ifndef JPEG_ENCODE_H
#define JPEG_ENCODE_H

#include <stddef.h>
#include <stdint.h>

/*
 * 把一帧 NV12 图像编码成 JPEG，结果通过 malloc 分配，调用者用 free() 释放。
 *
 * 入参:
 *   nv12    : NV12 数据首地址（Y 平面 w*h 字节 + 交错 UV 平面 w*h/2 字节）
 *   w, h    : 图像宽高（像素）。要求 w、h 均为偶数（V4L2 的 NV12 保证）。
 *             h 不是 16 的倍数也没关系，函数内部会自动补边。
 *   quality : JPEG 质量 1..100，越大越清晰文件越大（预览建议 70~85）
 *
 * 出参:
 *   jpeg_out : 指向 malloc 出的 JPEG 缓冲区（调用者负责 free）
 *   jpeg_len : JPEG 实际字节数
 *
 * 返回: 0 成功，-1 失败（stderr 已打印原因）
 */
int jpeg_encode_nv12(const uint8_t *nv12, int w, int h, int quality,
                     uint8_t **jpeg_out, size_t *jpeg_len);

#endif /* JPEG_ENCODE_H */
