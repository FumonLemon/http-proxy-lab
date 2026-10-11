/* net.c —— 连接与收发工具。
   两个 full 版循环、SO_REUSEADDR、getaddrinfo：出处 research.md 第 4.2/4.3/4.6 节。 */
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 单个地址的连接超时（毫秒）：不可达的地址最多拖这么久就换下一个。
   调大 = 更能容忍慢服务器；调小 = 双栈站点里换地址更快。 */
#define CONNECT_TIMEOUT_MS 3000

/* 服务器端：socket → setsockopt(SO_REUSEADDR) → bind → listen。
   SO_REUSEADDR 必须在 bind 之前，否则程序重启时端口处于 TIME_WAIT 会 bind 失败。 */
socket_t net_listen(int port) {
    socket_t s;
    struct sockaddr_in a;
    int on = 1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_INVALID) return SOCK_INVALID;

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, (sock_len_t)sizeof on);

    memset(&a, 0, sizeof a);              /* 结构体必须清零再用 */
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);   /* 端口要转网络字节序 */

    if (bind(s, (struct sockaddr *)&a, (sock_len_t)sizeof a) != 0) { sock_close(s); return SOCK_INVALID; }
    if (listen(s, 64) != 0) { sock_close(s); return SOCK_INVALID; }
    return s;
}

/* 客户端：getaddrinfo 解析 host:port，逐个地址试着连（一个主机名可能对应多个地址）。
   连接带超时：某个地址被黑洞时不能死等——实测 neverssl.com 的 A 记录就是这样
   （IPv4 无响应、IPv6 正常），阻塞 connect 会一直挂到内核放弃。CONNECT_TIMEOUT_MS 是那个调节旋钮。 */
socket_t net_connect(const char *host, int port) {
    struct addrinfo hints, *res, *p;
    char portstr[16];
    socket_t fd = SOCK_INVALID;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;        /* 不限定协议族：IPv4 / IPv6 都试 */
    hints.ai_socktype = SOCK_STREAM;    /* 要 TCP */
    snprintf(portstr, sizeof portstr, "%d", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0) return SOCK_INVALID;

    for (p = res; p; p = p->ai_next) {
        int rc;

        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == SOCK_INVALID) continue;

        if (sock_set_nonblock(fd, 1) != 0) { sock_close(fd); fd = SOCK_INVALID; continue; }
        rc = connect(fd, p->ai_addr, (sock_len_t)p->ai_addrlen);
        if (rc != 0 && (sock_errno() == SOCK_INPROGRESS || sock_errno() == SOCK_WOULDBLOCK)) {
            if (sock_wait_writable(fd, CONNECT_TIMEOUT_MS) != 0) {
                sock_close(fd); fd = SOCK_INVALID; continue;   /* 这个地址超时：试下一个 */
            }
            rc = sock_so_error(fd);                            /* 可写之后还要问一次连接结果 */
        }
        if (rc != 0) { sock_close(fd); fd = SOCK_INVALID; continue; }
        sock_set_nonblock(fd, 0);                              /* 恢复阻塞：后面收发都在阻塞模式 */
        break;
    }
    freeaddrinfo(res);                  /* res 是 getaddrinfo 分配的，必须还 */
    return fd;
}

/* TCP 是字节流：一次 recv 可能只拿到一部分，必须循环凑满。
   三种返回值语义不同：>0 数据、0 对端关闭、<0 错误（EINTR 要重试而不是失败）。 */
int net_recv_full(socket_t s, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = (int)recv(s, (char *)buf + got, SOCK_IO_LEN(len - got), 0);
        if (n > 0) { got += (size_t)n; continue; }
        if (n == 0) break;                              /* 对端关闭：提前结束 */
        if (sock_errno() == SOCK_INTR) continue;        /* 被信号打断：重试 */
        return -1;                                      /* 真错误 */
    }
    return (int)got;
}

/* 短写不是"可能"而是"必然"：客户端接收窗口满了，内核能收的就少于你给的一段。 */
int net_send_full(socket_t s, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = (int)send(s, (const char *)buf + sent, SOCK_IO_LEN(len - sent), 0);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && sock_errno() == SOCK_INTR) continue;
        return -1;
    }
    return 0;
}

/* 取对端 IP：用户过滤的判据，也是日志的第一个字段。 */
int net_peer_ip(socket_t s, char *out, size_t cap) {
    struct sockaddr_in a;
    sock_len_t alen = (sock_len_t)sizeof a;

    memset(&a, 0, sizeof a);
    out[0] = '\0';
    if (getpeername(s, (struct sockaddr *)&a, &alen) != 0) return -1;
    return INET_NTOP(AF_INET, &a.sin_addr, out, cap) ? 0 : -1;
}
