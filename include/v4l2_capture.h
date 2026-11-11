#ifndef V4L2_CAPTURE_H
#define V4L2_CAPTURE_H

#include <stdint.h>
#include <stddef.h>

/*
 * V4L2 摄像头采集模块 —— 模板
 *
 * 用法（典型主循环）:
 *
 *   struct v4l2_cam cam;
 *   v4l2_cam_open(&cam, "/dev/video0", 640, 480);
 *
 *   while (running) {
 *       struct v4l2_frame f;
 *       if (v4l2_cam_dqbuf(&cam, &f) == 0) {
 *           // f.data 指向一帧 NV12 原始数据, f.size 字节
 *           // TODO: RGA 把 NV12 缩放/letterbox → 送 NPU 推理
 *           v4l2_cam_qbuf(&cam, &f);   // 用完必须还回去
 *       }
 *   }
 *
 *   v4l2_cam_close(&cam);
 */

struct v4l2_frame {
    uint8_t *data;   /* mmap 映射出的帧首地址（不要 free，不要越界写） */
    size_t   size;   /* 实际字节长度 */
    int      index;  /* 来自哪个缓冲区，还帧时要用 */
    uint32_t sequence;/* 帧序号 */
};

struct v4l2_cam {
    int      fd;
    uint32_t width;
    uint32_t height;
    uint32_t pixfmt;      /* 四字符码，如 V4L2_PIX_FMT_NV12 */
    uint32_t num_planes;  /* 驱动返回的 plane 数（rkisp 的 NV12 为 1） */
    size_t   frame_size;

    /* 缓冲区池 */
    struct {
        void   *start;
        size_t  length;
    } buffers[4];
    int n_buffers;
};

/* 打开并初始化摄像头：协商格式 -> 申请缓冲 -> mmap -> 开流。
 * 返回 0 成功，-1 失败（stderr 已打印原因）。 */
int  v4l2_cam_open(struct v4l2_cam *cam, const char *dev,
                   uint32_t width, uint32_t height);

/* 取一帧（阻塞直到有帧）。返回 0 成功，-1 出错/已关流。 */
int  v4l2_cam_dqbuf(struct v4l2_cam *cam, struct v4l2_frame *frame);

/* 把帧对应的缓冲区重新入队，让内核继续往里写。 */
int  v4l2_cam_qbuf(struct v4l2_cam *cam, const struct v4l2_frame *frame);

/* 停流并释放全部资源。 */
void v4l2_cam_close(struct v4l2_cam *cam);

#endif /* V4L2_CAPTURE_H */
