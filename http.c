/* http.c —— HTTP 报文的解析与改写。
   解析核心：先找 \r\n\r\n 定出头部末尾，再自己逐行走；不用 strtok
   （strtok 会改原缓冲区、会把连续分隔符合并、还有全局状态，research.md 第 5.1 节）。 */
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 在 b[0..n) 里找 \r\n\r\n 的起始下标；没有返回 -1。 */
static long find_head_end(const char *b, size_t n) {
    size_t i;
    for (i = 0; i + 4 <= n; i++)
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n')
            return (long)i;
    return -1;
}

/* 值里是否含某个 token（用于 Cache-Control: no-store 判断） */
static int has_token(const char *v, size_t n, const char *tok) {
    size_t tl = strlen(tok), i;
    for (i = 0; i + tl <= n; i++)
        if (strncasecmp(v + i, tok, tl) == 0) return 1;
    return 0;
}

const char *http_find_header(const char *head, size_t len, const char *name, size_t *vlen) {
    size_t nl = strlen(name), p;

    if (!head || len == 0 || nl == 0) return NULL;
    p = 0;
    while (p + 1 < len && !(head[p] == '\r' && head[p + 1] == '\n')) p++;   /* 跳过首行 */
    p = (p + 1 < len) ? p + 2 : len;

    while (p < len) {
        size_t e = p, v, ve;
        while (e + 1 < len && !(head[e] == '\r' && head[e + 1] == '\n')) e++;
        if (e == p) break;                                   /* 空行：头部结束 */
        /* 字段名大小写不敏感（RFC 9110 §5.1）；值与名之间是第一个冒号 */
        if (e - p >= nl + 1 && strncasecmp(head + p, name, nl) == 0 && head[p + nl] == ':') {
            v = p + nl + 1;
            while (v < e && (head[v] == ' ' || head[v] == '\t')) v++;         /* 裁掉前导空白 */
            ve = e;
            while (ve > v && (head[ve - 1] == ' ' || head[ve - 1] == '\t')) ve--;  /* 裁掉尾随空白 */
            *vlen = ve - v;
            return head + v;
        }
        p = e + 2;
    }
    return NULL;
}

int http_read_head(socket_t s, char *buf, size_t cap, size_t *headlen) {
    size_t got = 0;

    *headlen = 0;
    while (got < cap) {
        int n = (int)recv(s, buf + got, SOCK_IO_LEN(cap - got), 0);
        if (n > 0) {
            long e;
            got += (size_t)n;
            e = find_head_end(buf, got);
            /* 返回 buf 里的总字节数：多出 *headlen 的部分是主体开头（同一次 recv 里带来的） */
            if (e >= 0) { *headlen = (size_t)e + 4; return (int)got; }
        } else if (n < 0) {
            if (sock_errno() == SOCK_INTR) continue;
            return -1;
        } else {
            return -1;                                      /* 对端在半条头部上就关了 */
        }
    }
    return -2;                                              /* 头部超过 cap */
}

int http_parse_request(const char *buf, size_t len, http_req_t *out) {
    const char *line = buf, *s1, *s2, *tgt, *url, *slash, *colon, *v;
    size_t ll = 0, mlen, tlen, hlen, vl = 0;
    char tmp[32];

    memset(out, 0, sizeof *out);
    out->content_length = -1;
    if (len == 0) return -1;

    while (ll + 1 < len && !(buf[ll] == '\r' && buf[ll + 1] == '\n')) ll++;   /* 请求行长度 */
    if (ll == 0) return -1;

    /* 第一个空格取方法，最后一个空格取版本：不受方法名长度影响（不用数固定字节数） */
    s1 = memchr(line, ' ', ll);
    if (!s1) return -1;
    s2 = NULL;
    for (const char *q = line + ll; q > s1; q--) if (q[-1] == ' ') { s2 = q - 1; break; }
    if (!s2 || s2 <= s1 + 1) return -1;

    mlen = (size_t)(s1 - line);
    tlen = (size_t)(s2 - s1 - 1);
    if (mlen == 0 || mlen >= sizeof out->method) return -1;
    if (tlen == 0 || tlen >= sizeof out->target) return -1;
    memcpy(out->method, line, mlen);
    tgt = s1 + 1;
    memcpy(out->target, tgt, tlen);
    if (s2 + 1 < line + ll) {
        size_t vlen = (size_t)(line + ll - s2 - 1);
        if (vlen >= sizeof out->version) return -1;
        memcpy(out->version, s2 + 1, vlen);
    }
    out->head = buf;
    out->headlen = len;

    if (strncasecmp(out->method, "CONNECT", 7) == 0) return -2;   /* 本实验不做隧道 */
    if (tlen < 7 || strncasecmp(tgt, "http://", 7) != 0) return -2;  /* 非绝对形式：只服务代理请求 */

    /* 绝对形式 http://host[:port]/path?query —— 剥掉 scheme 后按第一个 '/' 切两段 */
    url = tgt + 7;
    slash = memchr(url, '/', tlen - 7);
    hlen = (size_t)((slash ? slash : tgt + tlen) - url);
    if (hlen == 0 || hlen >= sizeof out->host) return -1;

    out->port = 80;
    colon = NULL;
    for (const char *q = url; q < url + hlen; q++) if (*q == ':') colon = q;   /* 最后一个冒号 = 端口 */
    if (colon) {
        hlen = (size_t)(colon - url);
        if (hlen == 0 || hlen >= sizeof out->host) return -1;
        out->port = atoi(colon + 1);
        if (out->port <= 0 || out->port > 65535) return -1;
    }
    memcpy(out->host, url, hlen);
    out->host[hlen] = '\0';

    if (slash) {
        size_t pl = (size_t)(tgt + tlen - slash);
        if (pl >= sizeof out->path) return -1;
        memcpy(out->path, slash, pl);
        out->path[pl] = '\0';
    } else {
        out->path[0] = '/';                 /* 没有路径就补一个 / */
        out->path[1] = '\0';
    }

    /* 请求体长度由 Content-Length 决定（请求永远不会用"关闭连接"来定界） */
    v = http_find_header(buf, len, "Content-Length", &vl);
    if (v && vl > 0 && vl < sizeof tmp) {
        memcpy(tmp, v, vl);
        tmp[vl] = '\0';
        out->content_length = atol(tmp);
        if (out->content_length < 0) return -1;
    }
    return 0;
}

int http_build_forward(const http_req_t *r, const char *ifs, char *out, size_t cap) {
    const char *h = r->head;
    size_t p = 0, hlen = r->headlen, o;
    int n;

    n = snprintf(out, cap, "%s %s %s\r\n", r->method, r->path,
                 r->version[0] ? r->version : "HTTP/1.1");
    if (n < 0 || (size_t)n >= cap) return -1;
    o = (size_t)n;

    /* 原样搬运其余头字段，剔除三类：
         1) Connection —— 下面统一补 close；
         2) If-Modified-Since / If-None-Match —— 这是**浏览器自己缓存**的条件请求头。
            不剥掉就转发，原站会回 304，而代理手里根本没有副本可回放，
            结果这条响应永远进不了缓存（实测：Edge 访问自己缓存过的站点，三次全是
            "304 196 缓存未命中"）。代理自己的条件请求在下面单独插入。 */
    while (p + 1 < hlen && !(h[p] == '\r' && h[p + 1] == '\n')) p++;
    p = (p + 1 < hlen) ? p + 2 : hlen;
    while (p < hlen) {
        size_t e = p, rest;
        int skip = 0;
        while (e + 1 < hlen && !(h[e] == '\r' && h[e + 1] == '\n')) e++;
        if (e == p) break;                                  /* 头部结束 */
        rest = e - p;
        if (rest >= 11 && strncasecmp(h + p, "Connection", 10) == 0 && h[p + 10] == ':') skip = 1;
        else if (rest >= 18 && strncasecmp(h + p, "If-Modified-Since", 17) == 0 && h[p + 17] == ':') skip = 1;
        else if (rest >= 13 && strncasecmp(h + p, "If-None-Match", 13) == 0 && h[p + 13] == ':') skip = 1;
        /* 代理认证凭据绝不能发给原服务器：只给本代理看，转发前删掉 */
        else if (rest >= 20 && strncasecmp(h + p, "Proxy-Authorization", 19) == 0 && h[p + 19] == ':') skip = 1;
        else if (rest >= 16 && strncasecmp(h + p, "Proxy-Connection", 16) == 0) skip = 1;
        if (!skip) {
            if (o + rest + 2 >= cap) return -1;
            memcpy(out + o, h + p, rest);
            o += rest;
            out[o++] = '\r'; out[o++] = '\n';
        }
        p = e + 2;
    }

    /* 命中缓存时带上条件请求（实验内容第 2 条要求的那一行） */
    if (ifs && ifs[0]) {
        n = snprintf(out + o, cap - o, "If-Modified-Since: %s\r\n", ifs);
        if (n < 0 || (size_t)n >= cap - o) return -1;
        o += (size_t)n;
    }
    /* 强制 close：原服务器响应完就关连接，"读到 EOF = 一条响应读完"于是成立（plan 决策一） */
    if (cap - o < 21) return -1;
    n = snprintf(out + o, cap - o, "Connection: close\r\n\r\n");
    if (n < 0) return -1;
    o += (size_t)n;
    return (int)o;
}

int http_parse_status(const char *head, size_t len, http_resp_t *out) {
    const char *sp, *v, *cc;
    size_t vl = 0, ccl = 0;

    memset(out, 0, sizeof *out);
    if (len < 12 || strncasecmp(head, "HTTP/", 5) != 0) return -1;
    sp = memchr(head, ' ', len);
    if (!sp || sp[1] < '0' || sp[1] > '9') return -1;
    out->status = atoi(sp + 1);
    if (out->status < 100 || out->status > 599) return -1;

    out->header_len = (int)len;
    out->bodyless = (out->status == 304 || out->status == 204 ||
                     (out->status >= 100 && out->status < 200)) ? 1 : 0;

    v = http_find_header(head, len, "Last-Modified", &vl);
    if (v && vl > 0 && vl < sizeof out->last_modified) {
        memcpy(out->last_modified, v, vl);
        out->last_modified[vl] = '\0';
    }

    /* 存储条件（RFC 9111 §3）：只存 200 + 带 Last-Modified + 没有 no-store 的 GET 响应 */
    out->cacheable = (out->status == 200 && out->last_modified[0] != '\0') ? 1 : 0;
    cc = http_find_header(head, len, "Cache-Control", &ccl);
    if (out->cacheable && cc && has_token(cc, ccl, "no-store")) out->cacheable = 0;
    return 0;
}
