/* main.c —— demo1: V4L2 取帧最小示例
 * 用法: ./demo1 [/dev/video0]
 */
#include <stdio.h>
#include <signal.h>
#include "v4l2_capture.h"
#include "frame_process.h"

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

/* 帧处理回调：每取到一帧由采集循环调用。
 * 后续流水线（RGA letterbox → RKNN 推理 → 画框）都加在这里，
 * 采集循环本身不用改。 */
static int on_frame(const void *data, size_t size, uint32_t seq, void *user)
{
    (void)data;   /* 现在还没用，流水线接上后会用到 */
    (void)user;

    static int n = 0;      /* 回调自己的计数器，演示回调可带状态 */
    if (n % 30 == 0)       /* 每 30 帧打一条，别刷屏 */
        printf("frame %4u: %zu bytes, seq=%u\n", n, size, seq);
    n++;
    return 0;              /* 返回非 0 可让采集循环退出 */
}

int main(int argc, char *argv[])
{
    /*默认设备*/
    const char *dev = (argc > 1) ? argv[1] : "/dev/video0";
    /*信号量SIGINT*/
    signal(SIGINT, on_sigint);
    /**/
    struct v4l2_cam cam;
    if (v4l2_cam_open(&cam, dev, 640, 480) != 0) {
        fprintf(stderr, "打开 %s 失败\n", dev);
        return 1;
    }

    frame_set_handler(&on_frame, NULL);   /* 注册帧处理回调（类似 signal 注册） */

    printf("按 Ctrl+C 退出\n");

    int n = 0;
    while (running) {
        struct v4l2_frame f;
        if (v4l2_cam_dqbuf(&cam, &f) != 0)
            break;

        /* 取到帧后交给回调处理，采集循环不关心具体逻辑 */
        frame_process(f.data, f.size, f.sequence);

        v4l2_cam_qbuf(&cam, &f);   /* 用完必须还，否则流会饿死 */
        n++;
    }

    printf("共取 %d 帧\n", n);
    v4l2_cam_close(&cam);
    return 0;
}
