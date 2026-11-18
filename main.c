/* main.c —— demo1: V4L2 取帧最小示例
 * 用法: ./demo1 [/dev/video0] [ip port]
 *   ip port 可选：给了就把每帧经 TCP 发到该地址端口
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <arpa/inet.h>
#include "v4l2_capture.h"
#include "frame_process.h"
#include "tcp_sender.h"

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

/* 传给回调的上下文：fd < 0 表示不发送 */
struct send_ctx { int fd; };

/* 帧处理回调：每取到一帧由采集循环调用。
 * 后续流水线（RGA letterbox → RKNN 推理 → 画框）都加在这里，
 * 采集循环本身不用改。 */
static int on_frame(const void *data, size_t size, uint32_t seq, void *user)
{
    struct send_ctx *ctx = user;

    if (ctx && ctx->fd >= 0) {
        /* 先发 4 字节帧长（网络字节序），再发帧数据。
         * TCP 是字节流、没有边界；接收端靠"长度+数据"切出每一帧。 */
        uint32_t len = htonl((uint32_t)size);
        if (tcp_send_all(ctx->fd, &len, sizeof(len)) < 0 ||
            tcp_send_all(ctx->fd, data, size) < 0)
            return 1;   /* 发送失败 → 停止采集 */
    }

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

    /* 可选：给了 ip port 就建立 TCP 连接，把帧发过去 */
    struct send_ctx ctx = { .fd = -1 };
    if (1) {
        ctx.fd = tcp_connect("192.168.137.1", (uint16_t)atoi("5000"));
        if (ctx.fd < 0)
            fprintf(stderr, "TCP 连接失败，继续仅本地运行\n");
    }

    frame_set_handler(&on_frame, &ctx);   /* 注册帧处理回调（类似 signal 注册） */

    printf("按 Ctrl+C 退出\n");

    int n = 0;
    while (running) {
        struct v4l2_frame f;
        if (v4l2_cam_dqbuf(&cam, &f) != 0)
            break;

        /* 取到帧后交给回调处理，采集循环不关心具体逻辑 */
        if (frame_process(f.data, f.size, f.sequence) != 0)
            break;                          /* 回调返回非 0 → 停止 */

        v4l2_cam_qbuf(&cam, &f);   /* 用完必须还，否则流会饿死 */
        n++;
    }

    printf("共取 %d 帧\n", n);
    tcp_close(ctx.fd);
    v4l2_cam_close(&cam);
    return 0;
}
