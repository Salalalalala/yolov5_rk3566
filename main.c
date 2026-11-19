/* mjpeg_preview —— 摄像头 → (可选 NPU 检测画框) → JPEG → HTTP multipart → VLC
 *
 * 用法: ./mjpeg_preview [/dev/video0] [port] [model.rknn]
 *   默认设备 /dev/video0，默认端口 8080
 *   第三个参数可选：给了 .rknn 模型就在预览画面上叠加检测框，
 *   不给则退化成纯预览（相当于之前的功能）。
 *
 * 跑起来后，在宿主机上用 VLC 打开:
 *   vlc http://192.168.137.200:8080
 * （192.168.137.200 是板子 IP，按实际改）
 *
 * 数据流：
 *   V4L2(NV12) → [RGA letterbox → RKNN → 解码/NMS → 在原帧上画框] → MPP 编码 JPEG
 *   检测和画框都在编码之前完成，所以 VLC 里看到的就是带框的画面。
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include "v4l2_capture.h"
#include "jpeg_encoder.h"
#include "mjpeg_server.h"
#include "rknn_detector.h"

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

/* 返回两个时间点之间的毫秒数，用于统计推理耗时 */
static double ms_between(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000.0 +
           (b->tv_nsec - a->tv_nsec) / 1e6;
}

int main(int argc, char *argv[])
{
    const char *dev     = (argc > 1) ? argv[1] : "/dev/video0";
    uint16_t    port    = (argc > 2) ? (uint16_t)atoi(argv[2]) : 8080;
    const char *model   = (argc > 3) ? argv[3] : NULL;  /* NULL = 不检测 */
    int         quality = 80;   /* 预览质量，够看且省带宽 */

    signal(SIGINT, on_sigint);

    /* 1. 打开摄像头 */
    struct v4l2_cam cam;
    if (v4l2_cam_open(&cam, dev, 640, 480) != 0) {
        fprintf(stderr, "打开 %s 失败\n", dev);
        return 1;
    }

    /* 1.5 加载 NPU 检测模型（可选）。
     *     只加载一次，后面每帧复用同一个 rknn_context。
     *     模型文件来自 convert_rknn.py（int8）。 */
    yolo_detector *det = NULL;
    if (model) {
        det = yolo_detector_create(model);
        if (!det) {
            v4l2_cam_close(&cam);
            return 1;   /* 用户明确要求检测，加载失败就退出，避免"静默不检测" */
        }
    }

    /* 1.6 创建硬件 JPEG 编码器（用摄像头实际回读的分辨率） */
    jpeg_encoder *enc = jpeg_encoder_create(cam.width, cam.height, quality);
    if (!enc) {
        yolo_detector_destroy(det);
        v4l2_cam_close(&cam);
        return 1;
    }

    /* 2. 启动 MJPEG 服务器（监听） */
    int lfd = mjpeg_server_listen("0.0.0.0", port);
    if (lfd < 0) {
        jpeg_encoder_destroy(enc);
        yolo_detector_destroy(det);
        v4l2_cam_close(&cam);
        return 1;
    }
    printf("用 VLC 打开: http://<板子IP>:%u\n", port);
    if (det)
        printf("NPU 检测已开启\n");

    /* 3. 主循环：等客户端 → 取帧 → 检测/画框 → 编码 → 发送；
     *    客户端断开就重新等下一个。 */
    int frame_n = 0;
    while (running) {
        int cfd = mjpeg_server_accept(lfd);   /* 阻塞，直到 VLC 连进来 */
        if (cfd < 0)
            break;

        while (running) {
            struct v4l2_frame f;
            if (v4l2_cam_dqbuf(&cam, &f) != 0)
                break;

            /* 3.1 NPU 检测 + 画框（在编码前，直接改 NV12 的 Y 平面）。
             *     检测失败不影响预览，继续走后面的编码。 */
            if (det) {
                yolo_box boxes[YOLO_MAX_BOXES];
                int nbox = 0;
                struct timespec t0, t1;

                clock_gettime(CLOCK_MONOTONIC, &t0);
                int ret = yolo_detector_run(det, f.data, f.size,
                                            (int)cam.width, (int)cam.height,
                                            boxes, YOLO_MAX_BOXES, &nbox);
                clock_gettime(CLOCK_MONOTONIC, &t1);

                if (ret == 0) {
                    yolo_draw_nv12(f.data, f.size,
                                   (int)cam.width, (int)cam.height,
                                   boxes, nbox, 2);

                    /* 每 30 帧打一条统计，别刷屏；需要细节时逐框打印 */
                    if (f.sequence % 30 == 0) {
                        printf("seq=%u: %d 个目标, NPU+RGA %.1f ms\n",
                               f.sequence, nbox, ms_between(&t0, &t1));
                        for (int i = 0; i < nbox; i++)
                            printf("  cls=%2d score=%.2f box=(%.0f,%.0f)-(%.0f,%.0f)\n",
                                   boxes[i].class_id, boxes[i].score,
                                   boxes[i].x1, boxes[i].y1,
                                   boxes[i].x2, boxes[i].y2);
                    }
                }
            }

            /* 3.2 硬件 JPEG 编码（输入是 NV12，此时可能已经叠加了检测框） */
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
    yolo_detector_destroy(det);
    v4l2_cam_close(&cam);
    return 0;
}
