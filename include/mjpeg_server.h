#ifndef MJPEG_SERVER_H
#define MJPEG_SERVER_H

#include <stddef.h>
#include <stdint.h>

/*
 * MJPEG over HTTP 服务器：板子当服务器，VLC 当客户端来拉流。
 *
 * 协议就一句 HTTP：响应头声明 Content-Type: multipart/x-mixed-replace，
 * 之后每一帧一个 "--boundary" 分块，块里是 Content-Length + JPEG 字节。
 * VLC 打开 http://<板子IP>:<port> 就能直接播放。
 */

/* 创建监听 socket（IPv4），绑定 ip:port。
 * 入参: ip 监听地址（"0.0.0.0" 表示所有网卡），port 端口号。
 * 返回: 监听 fd (>=0)，失败 -1。 */
int mjpeg_server_listen(const char *ip, uint16_t port);

/* 接受一个客户端连接（阻塞，直到有客户端进来），并发送 HTTP multipart 响应头。
 * 入参: listen_fd 监听 socket。
 * 返回: 客户端 fd (>=0)，失败 -1。 */
int mjpeg_server_accept(int listen_fd);

/* 把一帧 JPEG 作为一个 multipart 分块发给客户端。
 * 入参: client_fd 客户端 socket；jpeg JPEG 数据；len 字节数。
 * 返回: 0 成功，-1 失败（客户端断开等）。 */
int mjpeg_server_send_frame(int client_fd, const uint8_t *jpeg, size_t len);

/* 关闭一个 socket（监听或客户端 fd 都行，传 -1 忽略）。
 * 入参: fd 要关闭的描述符。 */
void mjpeg_server_close(int fd);

#endif /* MJPEG_SERVER_H */
