/* net.h —— 连接与收发工具。这是报告第 1 项（Socket 编程的客户端/服务器端主要步骤）的代码依据。 */
#ifndef NET_H
#define NET_H

#include "platform.h"
#include <stddef.h>

/* 创建监听 socket 并 bind+listen；失败返回 SOCK_INVALID。内部已设 SO_REUSEADDR。 */
socket_t net_listen(int port);

/* 解析 host:port 并逐个地址尝试连接；全部失败返回 SOCK_INVALID。用 getaddrinfo（IPv4/IPv6 都覆盖）。 */
socket_t net_connect(const char *host, int port);

/* 读到 len 字节或对端关闭才返回：返回实读字节数（可能 < len，表示对端提前关），-1 = 错误 */
int net_recv_full(socket_t s, void *buf, size_t len);

/* 写完全部 len 字节才返回：0 = 成功，-1 = 失败 */
int net_send_full(socket_t s, const void *buf, size_t len);

/* 取对端 IP 字符串（网站/用户过滤与日志用）：0 = 成功，-1 = 失败 */
int net_peer_ip(socket_t s, char *out, size_t cap);

#endif /* NET_H */
