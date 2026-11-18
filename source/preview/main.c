/* mjpeg_preview —— 摄像头 → JPEG → HTTP multipart → VLC 播放
 *
 * 用法: ./mjpeg_preview [/dev/video0] [port]
 *   默认设备 /dev/video0，默认端口 8080
 *
 * 跑起来后，在宿主机上用 VLC 打开:
 *   vlc http://192.168.137.200:8080
 * （192.168.137.200 是板子 IP，按实际改）
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "v4l2_capture.h"
#include "jpeg_encoder.h"
#include "mjpeg_server.h"

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

int main(int argc, char *argv[])
{
    const char *dev     = (argc > 1) ? argv[1] : "/dev/video0";
    uint16_t    port    = (argc > 2) ? (uint16_t)atoi(argv[2]) : 8080;
    int         quality = 80;   /* 预览质量，够看且省带宽 */

    signal(SIGINT, on_sigint);

    /* 1. 打开摄像头 */
    struct v4l2_cam cam;
    if (v4l2_cam_open(&cam, dev, 640, 480) != 0) {
        fprintf(stderr, "打开 %s 失败\n", dev);
        return 1;
    }

    /* 1.5 创建硬件 JPEG 编码器（用摄像头实际回读的分辨率） */
    jpeg_encoder *enc = jpeg_encoder_create(cam.width, cam.height, quality);
    if (!enc) {
        v4l2_cam_close(&cam);
        return 1;
    }

    /* 2. 启动 MJPEG 服务器（监听） */
    int lfd = mjpeg_server_listen("0.0.0.0", port);
    if (lfd < 0) {
        v4l2_cam_close(&cam);
        return 1;
    }
    printf("用 VLC 打开: http://<板子IP>:%u\n", port);

    /* 3. 主循环：等客户端 → 取帧 → 编码 → 发送；客户端断开就重新等下一个 */
    int frame_n = 0;
    while (running) {
        int cfd = mjpeg_server_accept(lfd);   /* 阻塞，直到 VLC 连进来 */
        if (cfd < 0)
            break;

        while (running) {
            struct v4l2_frame f;
            if (v4l2_cam_dqbuf(&cam, &f) != 0)
                break;

            uint8_t *jpeg = NULL;
            size_t   jlen = 0;
            if (jpeg_encoder_encode(enc, f.data, &jpeg, &jlen) == 0) {
                if (mjpeg_server_send_frame(cfd, jpeg, jlen) < 0) {
                    free(jpeg);
                    break;                 /* 客户端断开 → 重新 accept */
                }
                free(jpeg);
                if (frame_n % 30 == 0)
                    printf("已发送 %d 帧 (每帧 %zu 字节)\n", frame_n, jlen);
                frame_n++;
            }

            v4l2_cam_qbuf(&cam, &f);       /* 用完必须还 */
        }

        mjpeg_server_close(cfd);
    }

    printf("共发送 %d 帧\n", frame_n);
    mjpeg_server_close(lfd);
    jpeg_encoder_destroy(enc);
    v4l2_cam_close(&cam);
    return 0;
}
