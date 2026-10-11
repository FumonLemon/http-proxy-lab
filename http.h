/* http.h —— HTTP 报文的解析与改写（report 关键技术一节的代码依据）。 */
#ifndef HTTP_H
#define HTTP_H

#include "platform.h"
#include <stddef.h>

#define HDR_MAX 8192   /* 请求/响应头部的缓冲上限，超出即认为畸形 */

typedef struct {
    char   method[16];        /* GET / POST / CONNECT ... */
    char   target[2048];      /* 请求行里的原始 request-target；绝对形式时它就是缓存键 */
    char   version[16];       /* HTTP/1.1 */
    char   host[256];         /* 从 request-target 里切出来的主机名 */
    int    port;              /* 缺省 80 */
    char   path[2048];        /* 含查询串的路径，至少是 "/" */
    long   content_length;    /* 无 Content-Length 时为 -1 */
    const char *head;         /* 指向原始请求头部（用于原样搬运其余头字段） */
    size_t      headlen;
} http_req_t;

typedef struct {
    int  status;              /* 200 / 304 / ... */
    int  header_len;          /* 头部字节数，含最后的空行 */
    int  bodyless;            /* 304 / 1xx / 204 → 1（RFC 9112 §6.3 第 1 条） */
    int  cacheable;           /* 200 且带 Last-Modified 且无 no-store → 1（RFC 9111 §3） */
    char last_modified[64];   /* 空串表示原服务器没给 */
} http_resp_t;

/* 解析请求头：0 = 成功，-1 = 畸形（回 400），-2 = 本实验不支持的形式（CONNECT / 非绝对形式，回 501） */
int http_parse_request(const char *buf, size_t len, http_req_t *out);

/* 循环读到 \r\n\r\n：返回 buf 里的总字节数（≥ *headlen，多出来的是主体开头），-1 = 出错/对端关，-2 = 超上限 */
int http_read_head(socket_t s, char *buf, size_t cap, size_t *headlen);

/* 大小写不敏感地找字段；命中返回值的起始指针并置 *vlen，找不到返回 NULL */
const char *http_find_header(const char *head, size_t len, const char *name, size_t *vlen);

/* 拼出发往原服务器的请求报文（origin-form + Connection: close，命中缓存时加 If-Modified-Since）；返回长度，-1 = 装不下 */
int http_build_forward(const http_req_t *r, const char *ifs, char *out, size_t cap);

/* 解析响应头部：0 = 成功，-1 = 状态行畸形（调用方回 502） */
int http_parse_status(const char *head, size_t len, http_resp_t *out);

#endif /* HTTP_H */
