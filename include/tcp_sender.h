#ifndef TCP_SENDER_H
#define TCP_SENDER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* 连接指定 ip:port（IPv4），成功返回 socket 描述符(>=0)，失败返回 -1 */
int tcp_connect(const char *ip, uint16_t port);

/* 把 buf 的 n 字节全部发完。send() 可能一次只发一部分（大数据量或
 * 网络拥塞时尤其常见），这里循环直到发完或出错。
 * 成功返回 n，失败返回 -1。 */
ssize_t tcp_send_all(int fd, const void *buf, size_t n);

/* 关闭连接 */
void tcp_close(int fd);

#endif /* TCP_SENDER_H */
