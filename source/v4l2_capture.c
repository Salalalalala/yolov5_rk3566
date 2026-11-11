/* v4l2_capture.c —— V4L2 采集模板（NV12, mmap, 阻塞式 DQBUF）
 *
 * 设计要点:
 *   - 不依赖任何第三方库，只用 <linux/videodev2.h>（内核头）
 *   - 针对 rkisp（Rockchip ISP）：它是 Multiplanar 采集设备（device_caps
 *     含 V4L2_CAP_VIDEO_CAPTURE_MPLANE），所以本文件全程用
 *     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE + pix_mp / v4l2_plane 结构。
 *     若以后接 USB 摄像头（uvcvideo 是单平面 V4L2_CAP_VIDEO_CAPTURE），
 *     需换成单平面结构（fmt.pix / buf.m.offset），ioctl 流程完全一样。
 *   - rkisp 输出的 NV12 是"单 plane 半平面"布局：Y 平面 + 交错 UV，
 *     一整帧在同一个连续缓冲区里，一个 mmap 就能拿到整帧。
 *   - 4 个缓冲区轮转（乒乓缓冲），采集与处理天然并行
 *   - 所有错误走 perror + 返回值，不搞花活
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "v4l2_capture.h"

#define CAM_ERR(...) do { fprintf(stderr, "[cam] " __VA_ARGS__); } while (0)

static int xioctl(int fd, unsigned long req, void *arg)
{
    /* ioctl 在收到信号时会中断返回 EINTR，重试即可（类似 write 的语义） */
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

/* 打开设备并把格式协商、缓冲区、流全部设置好 */
int v4l2_cam_open(struct v4l2_cam *cam, const char *dev,
                  uint32_t width, uint32_t height)
{
    memset(cam, 0, sizeof(*cam));

    cam->fd = open(dev, O_RDWR | O_NONBLOCK, 0);
    if (cam->fd < 0) {
        CAM_ERR("open %s: %s\n", dev, strerror(errno));
        return -1;
    }

    /* 1. 确认这是 multiplanar 视频采集设备 */
    struct v4l2_capability cap;
    if (xioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        CAM_ERR("QUERYCAP: %s\n", strerror(errno));
        goto fail;
    }
    uint32_t caps = cap.device_caps;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        CAM_ERR("%s 不是 multiplanar 采集设备 (caps=%08x)\n", dev, caps);
        CAM_ERR("（若是单平面设备如 USB 摄像头，需改用单平面 V4L2 结构）\n");
        goto fail;
    }

    /* 2. 设置格式：分辨率 + 像素格式（multiplanar 结构）。
     * 驱动可能不接受你请求的值，会返回它实际用的，必须回读检查。 */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = width;
    fmt.fmt.pix_mp.height      = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;   /* rkisp 原生，RGA/RKNN 直喂 */
    fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;

    if (xioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        CAM_ERR("S_FMT: %s\n", strerror(errno));
        goto fail;
    }
    cam->width      = fmt.fmt.pix_mp.width;       /* 回读实际生效值 */
    cam->height     = fmt.fmt.pix_mp.height;
    cam->pixfmt     = fmt.fmt.pix_mp.pixelformat;
    cam->num_planes = fmt.fmt.pix_mp.num_planes;
    cam->frame_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

    if (cam->pixfmt != V4L2_PIX_FMT_NV12) {
        char fourcc[5] = {0};
        memcpy(fourcc, &cam->pixfmt, 4);
        CAM_ERR("警告: 驱动给的格式是 %s，不是 NV12\n", fourcc);
    }
    if (cam->num_planes != 1) {
        CAM_ERR("警告: 期望 1 plane，实际 %u plane；本模板只 mmap 第 0 plane\n",
                cam->num_planes);
    }

    /* 3. 申请内核缓冲区并 mmap 映射到用户空间（零拷贝） */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = 4;                        /* 乒乓池大小 */
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
        CAM_ERR("REQBUFS: %s\n", strerror(errno));
        goto fail;
    }
    cam->n_buffers = req.count;
    if (cam->n_buffers < 2) {
        CAM_ERR("缓冲区太少: %d\n", cam->n_buffers);
        goto fail;
    }

    for (int i = 0; i < cam->n_buffers; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.length   = VIDEO_MAX_PLANES;   /* planes[] 数组能容纳的 plane 数 */
        buf.m.planes = planes;

        if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            CAM_ERR("QUERYBUF: %s\n", strerror(errno));
            goto fail;
        }
        /* NV12 单 plane：整个连续缓冲区在 plane 0 里 */
        cam->buffers[i].length = planes[0].length;
        cam->buffers[i].start  = mmap(NULL, planes[0].length,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED, cam->fd,
                                      planes[0].m.mem_offset);
        if (cam->buffers[i].start == MAP_FAILED) {
            CAM_ERR("mmap: %s\n", strerror(errno));
            goto fail;
        }
    }

    /* 4. 全部缓冲入队，然后开流 */
    for (int i = 0; i < cam->n_buffers; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.length   = VIDEO_MAX_PLANES;
        buf.m.planes = planes;
        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            CAM_ERR("QBUF: %s\n", strerror(errno));
            goto fail;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        CAM_ERR("STREAMON: %s\n", strerror(errno));
        goto fail;
    }

    printf("[cam] %s: %ux%u NV12, %d buffers, frame=%zu bytes\n",
           dev, cam->width, cam->height, cam->n_buffers, cam->frame_size);
    return 0;

fail:
    v4l2_cam_close(cam);
    return -1;
}

/* 取一帧（阻塞等待）。注意：open 时用了 O_NONBLOCK，这里用
 * select() 人为制造阻塞语义，避免 DQBUF 立刻返回 EAGAIN。 */
int v4l2_cam_dqbuf(struct v4l2_cam *cam, struct v4l2_frame *frame)
{
    fd_set fds;
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

    FD_ZERO(&fds);
    FD_SET(cam->fd, &fds);
    int r = select(cam->fd + 1, &fds, NULL, NULL, &tv);
    if (r == -1) {
        if (errno == EINTR) return -1;
        CAM_ERR("select: %s\n", strerror(errno));
        return -1;
    }
    if (r == 0) {
        CAM_ERR("取帧超时(2s)，检查摄像头是否在出流\n");
        return -1;
    }

    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.length   = VIDEO_MAX_PLANES;
    buf.m.planes = planes;

    if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
        CAM_ERR("DQBUF: %s\n", strerror(errno));
        return -1;
    }

    frame->data     = cam->buffers[buf.index].start;
    frame->size     = planes[0].bytesused;  /* 单 plane，整帧字节数在此 */
    frame->index    = buf.index;
    frame->sequence = buf.sequence;
    return 0;
}

int v4l2_cam_qbuf(struct v4l2_cam *cam, const struct v4l2_frame *frame)
{
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.index    = frame->index;
    buf.length   = VIDEO_MAX_PLANES;
    buf.m.planes = planes;

    if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
        CAM_ERR("QBUF(requeue): %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

void v4l2_cam_close(struct v4l2_cam *cam)
{
    if (cam->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(cam->fd, VIDIOC_STREAMOFF, &type);  /* 忽略错误，尽力关 */
        for (int i = 0; i < cam->n_buffers; i++) {
            if (cam->buffers[i].start && cam->buffers[i].start != MAP_FAILED)
                munmap(cam->buffers[i].start, cam->buffers[i].length);
        }
        close(cam->fd);
    }
    memset(cam, 0, sizeof(*cam));
}
