/* main.c —— demo1: V4L2 取帧最小示例
 * 用法: ./demo1 [/dev/video0]
 */
#include <stdio.h>
#include <signal.h>
#include "v4l2_capture.h"

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

int main(int argc, char *argv[])
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/video0";

    signal(SIGINT, on_sigint);

    struct v4l2_cam cam;
    if (v4l2_cam_open(&cam, dev, 640, 480) != 0) {
        fprintf(stderr, "打开 %s 失败\n", dev);
        return 1;
    }

    printf("按 Ctrl+C 退出\n");

    int n = 0;
    while (running) {
        struct v4l2_frame f;
        if (v4l2_cam_dqbuf(&cam, &f) != 0)
            break;

        /* 这里就是你后续流水线的插入点：
         * NV12 帧 (f.data, f.size)
         *   → RGA 缩放 + letterbox 到 640x640（NV12 是 RGA 原生输入）
         *   → RKNN (NPU) 推理
         *   → 画框/输出结果
         */
        if (n % 30 == 0)   /* 每 30 帧打一条，别刷屏 */
            printf("frame %4u: %zu bytes, seq=%u\n", n, f.size, f.sequence);

        v4l2_cam_qbuf(&cam, &f);   /* 用完必须还，否则流会饿死 */
        n++;
    }

    printf("共取 %d 帧\n", n);
    v4l2_cam_close(&cam);
    return 0;
}
