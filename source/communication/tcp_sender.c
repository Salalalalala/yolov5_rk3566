/* tcp_sender.c —— 极简 TCP 客户端（把数据发到指定 ip:port）
 *
 * 只用 POSIX socket，不依赖第三方库。三个函数：
 *   tcp_connect   —— 建立连接
 *   tcp_send_all  —— 保证发完（处理 send 的部分发送）
 *   tcp_close     —— 关连接
 */
#include "tcp_sender.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int tcp_connect(const char *ip, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[tcp] socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);                 /* 主机序 → 网络序（大端） */
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "[tcp] 非法 IP: %s\n", ip);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[tcp] connect %s:%u: %s\n", ip, port, strerror(errno));
        close(fd);
        return -1;
    }

    printf("[tcp] 已连接 %s:%u\n", ip, port);
    return fd;
}

ssize_t tcp_send_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t left  = n;

    while (left > 0) {
        ssize_t sent = send(fd, p, left, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;    /* 被信号打断，重试 */
            fprintf(stderr, "[tcp] send: %s\n", strerror(errno));
            return -1;
        }
        if (sent == 0) {                     /* 对端已关闭连接 */
            fprintf(stderr, "[tcp] 对端关闭了连接\n");
            return -1;
        }
        p    += sent;
        left -= sent;
    }
    return (ssize_t)n;
}

void tcp_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
