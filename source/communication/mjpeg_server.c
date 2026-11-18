/* mjpeg_server.c —— MJPEG over HTTP 服务器（给 VLC 拉流用）
 *
 * 协议回顾（IP 摄像头通用做法）:
 *   1. accept 后立刻发一段响应头:
 *        HTTP/1.0 200 OK\r\n
 *        Content-Type: multipart/x-mixed-replace; boundary=frame\r\n
 *        \r\n
 *   2. 之后每帧发一个分块:
 *        --frame\r\n
 *        Content-Type: image/jpeg\r\n
 *        Content-Length: <字节数>\r\n
 *        \r\n
 *        <JPEG 数据>
 *        \r\n
 * VLC 认识这个格式，会一帧一帧连续播放。
 */
#include "mjpeg_server.h"
#include "tcp_sender.h"   /* 复用 tcp_send_all 的"保证发完"逻辑 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BOUNDARY "frame"

int mjpeg_server_listen(const char *ip, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[mjpeg] socket: %s\n", strerror(errno));
        return -1;
    }

    /* SO_REUSEADDR：程序退出后端口能立刻复用，避免 TIME_WAIT 占用 */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "[mjpeg] 非法 IP: %s\n", ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[mjpeg] bind %s:%u: %s\n", ip, port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {              /* 单客户端预览够用 */
        fprintf(stderr, "[mjpeg] listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    printf("[mjpeg] 监听 %s:%u\n", ip, port);
    return fd;
}

int mjpeg_server_accept(int listen_fd)
{
    struct sockaddr_in cli;
    socklen_t len = sizeof(cli);
    int cfd = accept(listen_fd, (struct sockaddr *)&cli, &len);
    if (cfd < 0) {
        fprintf(stderr, "[mjpeg] accept: %s\n", strerror(errno));
        return -1;
    }

    /* 发送 multipart 响应头，之后才开始传帧 */
    char header[256];
    int hn = snprintf(header, sizeof(header),
                      "HTTP/1.0 200 OK\r\n"
                      "Content-Type: multipart/x-mixed-replace; boundary=%s\r\n"
                      "\r\n",
                      BOUNDARY);
    if (tcp_send_all(cfd, header, (size_t)hn) < 0) {
        fprintf(stderr, "[mjpeg] 发送响应头失败\n");
        close(cfd);
        return -1;
    }

    printf("[mjpeg] 客户端接入 %s:%u\n",
           inet_ntoa(cli.sin_addr), ntohs(cli.sin_port));
    return cfd;
}

int mjpeg_server_send_frame(int client_fd, const uint8_t *jpeg, size_t len)
{
    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
                      "--%s\r\n"
                      "Content-Type: image/jpeg\r\n"
                      "Content-Length: %zu\r\n"
                      "\r\n",
                      BOUNDARY, len);

    /* 分块头 + JPEG 数据 + 结尾空行，三部分都"保证发完" */
    if (tcp_send_all(client_fd, hdr, (size_t)hn) < 0) return -1;
    if (tcp_send_all(client_fd, jpeg, len) < 0)        return -1;
    if (tcp_send_all(client_fd, "\r\n", 2) < 0)        return -1;
    return 0;
}

void mjpeg_server_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
