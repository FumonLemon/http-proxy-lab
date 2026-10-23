/* proxy.c —— 代理主程序：主线程 accept，每来一个连接开一个 worker 线程。
   一次请求的完整时序就是 plan.md 第 1.2 节那张图，一行不差。
   三个扩展功能（网站过滤 / 用户过滤 / 网站引导）在这条链路的两处各插一道判断。
   三张名单不再写死在本文件里：来自 rules.txt，存盘即生效（rules.h / rules.c）。
   源代码注释按报告第 6 项要求写足：每个函数写清做什么、参数、返回、失败行为。 */

#include "platform.h"
#include "net.h"
#include "http.h"
#include "cache.h"
#include "rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LISTEN_PORT 8080     /* 代理监听端口，命令行第一个参数可覆盖 */
#define STREAM_BUF  65536    /* 收发缓冲区：malloc 一份，不放线程栈（plan 决策二） */
#define MAX_THREADS 32       /* 同时在线 worker 上限，超了回 503 */

/* 三个功能各有一个开关，编译时 -DENABLE_XXX=0 可关掉做对照（plan 第 7 节 D3） */
#ifndef ENABLE_SITE_FILTER
#define ENABLE_SITE_FILTER 1
#endif
#ifndef ENABLE_USER_FILTER
#define ENABLE_USER_FILTER 1
#endif
#ifndef ENABLE_REDIRECT
#define ENABLE_REDIRECT 1
#endif

/* ---- 三张名单搬去了 rules.txt（值会变，所以做成运行时可重载的文件）。
       默认名单：网站 127.0.0.1 / 用户 192.0.2.1 / 引导 guide.test → http://example.com/
       文件读不到时 rules.c 里那三张内置默认名单顶上，行为与改造前一致。 ---- */

/* ---- 自制页面：都带上可核对的标记串，报告里截图用 ---- */
#if ENABLE_SITE_FILTER
static const char BODY_403_SITE[] =
    "<html><body><h1>403 Forbidden（网站过滤）</h1>"
    "<p>你访问的主机在代理的网站过滤名单内，代理没有连接原服务器。</p></body></html>";
#endif
#if ENABLE_USER_FILTER
static const char BODY_403_USER[] =
    "<html><body><h1>403 Forbidden（用户过滤）</h1>"
    "<p>你的来源 IP 在代理的用户过滤名单内，代理不为你服务。</p></body></html>";
#endif
#if ENABLE_REDIRECT
/* 网站引导不再回自制页：命中名单就 302 跳到名单里写的目标网址，见 send_redirect() */
#endif
static const char BODY_400[] =
    "<html><body><h1>400 Bad Request</h1><p>请求报文畸形，无法解析。</p></body></html>";
static const char BODY_501[] =
    "<html><body><h1>501 Not Implemented</h1>"
    "<p>本实验的代理只支持 http 的绝对形式请求（GET/POST），不支持 CONNECT/HTTPS。</p></body></html>";
static const char BODY_502[] =
    "<html><body><h1>502 Bad Gateway</h1><p>无法连接原服务器，或原服务器响应无法解析。</p></body></html>";
static const char BODY_503[] =
    "<html><body><h1>503 Service Unavailable</h1><p>代理并发连接数已达上限，请稍后再试。</p></body></html>";

/* 线程参数：用堆分配的结构体传（栈上的变量出了作用域就没了） */
typedef struct {
    socket_t c;
} conn_arg_t;

static mutex_t g_threads_lock;   /* 保护 g_threads */
static int     g_threads;

/* 自测注入点：把某来源 IP 也当作"用户过滤名单内"（plan 第 7 节 T6 允许的注入值）。
   生产用文件顶部那张表；这个函数只给单机上的自测用，因为本机客户端 IP 恒为 127.0.0.1。 */
static const char *g_user_block_extra;

void proxy_set_user_block(const char *ip) {
    mutex_lock(g_threads_lock);
    g_user_block_extra = ip;
    mutex_unlock(g_threads_lock);
}

#if ENABLE_USER_FILTER
/* 用户过滤判据：对端 IP 是否在名单内（名单来自 rules.txt），再叠上自测的注入值。
   rules_* 自己带锁，这里那把锁只护着注入值。 */
static int user_blocked(const char *ip) {
    int extra;
    mutex_lock(g_threads_lock);
    extra = (g_user_block_extra && strcmp(g_user_block_extra, ip) == 0);
    mutex_unlock(g_threads_lock);
    return rules_user_blocked(ip) || extra;
}
#endif

/* 给客户端回一条代理自制的响应（含 Content-Length 与 Connection: close）。失败行为：发不出去就算了。 */
static void send_page(socket_t c, int code, const char *reason, const char *body) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n\r\n%s",
                     code, reason, (unsigned)strlen(body), body);
    if (n > 0 && (size_t)n < sizeof buf) net_send_full(c, buf, (size_t)n);
}

/* ---- 网站引导的两种落地方式（目标写在 rules.txt 的 [redirect] 里）----
   目标以 http:// 开头   → 302 + Location 跳过去，但浏览器地址栏会变成目标网址；
   目标写成 self:路径   → 代理直接把这个文件当页面回给客户端，地址栏仍是原域名
                          （实验里"引导到模拟网站/钓鱼"要的就是这个效果，且改文件即生效）。
   两个函数只在 ENABLE_REDIRECT 下存在，免得开关全关时被 -Wunused-function 报。 */
#if ENABLE_REDIRECT

/* 302：不带体，Content-Length: 0。失败行为：发不出去就算了，调用方照常收尾。 */
static void send_redirect(socket_t c, const char *url) {
    char buf[1024];
    int n = snprintf(buf, sizeof buf,
                     "HTTP/1.1 302 Found\r\n"
                     "Location: %s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n\r\n", url);
    if (n > 0 && (size_t)n < sizeof buf) net_send_full(c, buf, (size_t)n);
}

/* self:目标 —— 把本地文件当页面回给客户端。每次请求现读，所以改那个文件立即生效
   （跟名单一个道理）。成功返回发出的页面字节数；文件读不到 / 超过 256 KB / 发失败返回 0。 */
static int send_local_file(socket_t c, const char *path) {
    const size_t cap = 256u * 1024u;
    FILE  *f;
    char  *buf;
    size_t got;
    int    sent = 0;

    f = fopen(path, "rb");
    if (!f) return 0;
    buf = malloc(cap);
    if (!buf) { fclose(f); return 0; }
    got = fread(buf, 1, cap, f);
    fclose(f);
    if (got > 0 && got < cap) {
        char head[256];
        int h = snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/html; charset=utf-8\r\n"
                         "Content-Length: %u\r\n"
                         "Connection: close\r\n\r\n", (unsigned)got);
        if (h > 0 && (size_t)h < sizeof head &&
            net_send_full(c, head, (size_t)h) == 0 &&
            net_send_full(c, buf, got) == 0) sent = (int)got;
    }
    free(buf);
    return sent;
}

#endif /* ENABLE_REDIRECT */

/* 日志过滤：--hide <状态码> 时该状态码不打印（默认 0 = 全打）。
   Edge 在后台会建几十个 https 连接，每个都是 CONNECT → 501，日志被刷屏；
   加这个开关只是不写日志，代码路径一条没少。 */
static int g_hide_status = 0;

/* 日志一行到 stderr（协议数据只走 socket，日志绝不许混进客户端字节流）。
   字段固定：时间 客户端IP:端口 请求行 目标host:端口 状态码 转发字节数 缓存标记 [附加信息] */
static void log_line(const char *peer, const char *reqline, const char *hostport,
                     int status, long bytes, const char *tag, const char *extra) {
    time_t t = time(NULL);
    struct tm tm;
    char ts[32];

    if (g_hide_status && status == g_hide_status) return;

    plat_localtime(&tm, t);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s] %s %s -> %s %d %ld %s%s\n", ts, peer, reqline, hostport,
            status, bytes, tag, extra ? extra : "");
    fflush(stderr);      /* 日志重定向到文件时 stderr 会被缓冲，不刷就看不到实时输出 */
}

/* 增量攒响应体（用于入缓存）。超过 CACHEABLE_MAX 就放弃攒，置 stop（不再缓存这条）。 */
typedef struct {
    char  *p;
    size_t len, cap;
    int    stop;
} acc_t;

static void acc_add(acc_t *a, const void *d, size_t n) {
    if (a->stop) return;
    if (a->len + n > CACHEABLE_MAX) {       /* 太大：丢掉已攒的，这条不缓存 */
        free(a->p);
        a->p = NULL; a->len = a->cap = 0; a->stop = 1;
        return;
    }
    if (a->len + n > a->cap) {
        size_t nc = a->cap ? a->cap : 8192;
        char *np;
        while (nc < a->len + n) nc *= 2;
        np = realloc(a->p, nc);
        if (!np) { a->stop = 1; return; }
        a->p = np; a->cap = nc;
    }
    memcpy(a->p + a->len, d, n);
    a->len += n;
}

/* 处理一条客户端连接（worker 的主体）。资源所有权：c 由调用者关；u 本函数内开、本函数内关。 */
static void handle_conn(socket_t c) {
    char       *hbuf = NULL, *fwd_buf = NULL, *sbuf = NULL;
    char        peer[64] = "?", hostport[300] = "-", reqline[600] = "-", tag[24] = "-";
    char        extra[96] = "";      /* 命中时填 IMS=<值>，附在日志行末，验收时能直接看到条件请求 */
    socket_t    u = SOCK_INVALID;
    http_req_t  r;
    http_resp_t rs;
    cache_obj_t co;
    acc_t       acc;
    size_t      headlen = 0, rheadlen = 0;
    int         got = 0, pr = 0, fl = 0, status = 0, hit = 0;
    long        fwd_bytes = 0, cl;
    const char *ifs = NULL;

    rules_reload_if_changed();      /* 名单热加载：名单文件存盘后，下一个请求就用新名单 */

    memset(&co, 0, sizeof co);
    memset(&acc, 0, sizeof acc);
    memset(&r, 0, sizeof r);
    memset(&rs, 0, sizeof rs);

    hbuf    = malloc(HDR_MAX);
    fwd_buf = malloc(HDR_MAX + 128);
    sbuf    = malloc(STREAM_BUF);
    if (!hbuf || !fwd_buf || !sbuf) goto done;   /* 内存不够：直接关连接 */

    /* 1. 取客户端 IP —— 用户过滤的判据 */
    net_peer_ip(c, peer, sizeof peer);

    /* 2. 收完整请求头（循环 recv 直到读到空行，不假设一次 recv 就是一条报文） */
    got = http_read_head(c, hbuf, HDR_MAX, &headlen);
    if (got == -2) { send_page(c, 400, "Bad Request", BODY_400); status = 400; goto done; }
    if (got <= 0) goto done;                    /* 对端直接关了：无事可做，不打日志 */

    /* 3. 解析请求行与头部 */
    pr = http_parse_request(hbuf, (size_t)got, &r);
    if (pr == -2) { send_page(c, 501, "Not Implemented", BODY_501); status = 501; goto done; }
    if (pr != 0)  { send_page(c, 400, "Bad Request", BODY_400);     status = 400; goto done; }
    snprintf(reqline, sizeof reqline, "%s %s %s", r.method, r.target, r.version);
    snprintf(hostport, sizeof hostport, "%s:%d", r.host, r.port);

    /* 4. 用户过滤：判据是来源 IP。放在解析之后，日志才能同时写出"谁"和"他想访问什么"；
       早于解析则只能记一个 IP，验收时看不出被拦的是哪次请求。 */
#if ENABLE_USER_FILTER
    if (user_blocked(peer)) {
        send_page(c, 403, "Forbidden", BODY_403_USER);
        status = 403; strcpy(tag, "用户过滤");
        goto done;
    }
#endif

    /* 5. 网站过滤：命中就不连原服务器（报告里用"原服务器没接到连接"证明它生效） */
#if ENABLE_SITE_FILTER
    if (rules_site_blocked(r.host)) {
        send_page(c, 403, "Forbidden", BODY_403_SITE);
        status = 403; strcpy(tag, "网站过滤");
        goto done;
    }
#endif

    /* 6. 网站引导：目标写网址就 302 跳过去；写 self:路径就直接回那个文件（地址栏不变） */
#if ENABLE_REDIRECT
    {
        char dst[1024];
        if (rules_redirect_target(r.host, dst, sizeof dst)) {
            if (strncmp(dst, "self:", 5) == 0) {
                fwd_bytes = send_local_file(c, dst + 5);
                if (fwd_bytes > 0) {
                    status = 200; strcpy(tag, "网站引导(自制页)");
                } else {
                    fwd_bytes = 0;
                    send_page(c, 502, "Bad Gateway", BODY_502);   /* 引导页文件读不到就如实回 502 */
                    status = 502; strcpy(tag, "引导页读不到");
                }
                goto done;
            }
            send_redirect(c, dst);
            status = 302; strcpy(tag, "网站引导");
            goto done;
        }
    }
#endif

    /* 7. 查缓存（键 = 浏览器发来的完整 URL） */
    hit = cache_fetch(r.target, &co);
    if (hit) {
        ifs = co.last_modified;                 /* 命中就带条件请求去问原服务器 */
        snprintf(extra, sizeof extra, " IMS=%s", co.last_modified);   /* 让日志直接显示发出去的是哪条条件 */
    }
    strcpy(tag, hit ? "缓存命中" : "缓存未命中");

    /* 8. 连原服务器 */
    u = net_connect(r.host, r.port);
    if (u == SOCK_INVALID) {
        send_page(c, 502, "Bad Gateway", BODY_502);
        status = 502; strcpy(tag, "连不上原服务器");
        goto done;
    }

    /* 9. 拼转发报文（origin-form + Connection: close [+ If-Modified-Since]）并发出去 */
    fl = http_build_forward(&r, ifs, fwd_buf, HDR_MAX + 128);
    if (fl < 0) { send_page(c, 500, "Internal Server Error", BODY_502); status = 500; goto done; }
    if (net_send_full(u, fwd_buf, (size_t)fl) != 0) { status = 502; goto done; }

    /* 10. 有请求体（POST）就按 Content-Length 转发：先转"跟着头部一起收到的那一截"，
       剩下的再边读边转。不扣掉这一截，就会一直等一段其实已经收到的字节（自测当场挂住）。 */
    cl = r.content_length;
    if (cl > 0) {
        long have = (long)got - (long)headlen;
        if (have > cl) have = cl;
        if (have > 0) {
            if (net_send_full(u, hbuf + headlen, (size_t)have) != 0) { status = 502; goto done; }
            cl -= have;
        }
    }
    while (cl > 0) {
        int want = (int)((size_t)cl < STREAM_BUF ? (size_t)cl : STREAM_BUF);
        int n = net_recv_full(c, sbuf, (size_t)want);
        if (n <= 0) break;
        if (net_send_full(u, sbuf, (size_t)n) != 0) break;
        cl -= n;
    }

    /* 11. 读响应头并判定（304/1xx/204 无主体；200 且带 Last-Modified 才可缓存） */
    got = http_read_head(u, hbuf, HDR_MAX, &rheadlen);
    if (got <= 0 || http_parse_status(hbuf, rheadlen, &rs) != 0) {
        send_page(c, 502, "Bad Gateway", BODY_502);
        status = 502; strcpy(tag, "响应无法解析");
        goto done;
    }

    /* 12. 命中缓存且原服务器回 304 → 用缓存里的对象应答（实验内容第 2 条的效果） */
    if (hit && rs.status == 304) {
        net_send_full(c, co.head, co.headlen);
        net_send_full(c, co.body, co.bodylen);
        status = 304;
        fwd_bytes = (long)(co.headlen + co.bodylen);
        strcpy(tag, "命中+304");
        goto done;
    }

    /* 13. 流式转发：先把已读到的字节发出去（头部，可能还带主体开头），再边收边发 */
    if (net_send_full(c, hbuf, (size_t)got) != 0) { status = rs.status; goto done; }
    fwd_bytes = got;
    if (rs.cacheable) acc_add(&acc, hbuf + rheadlen, (size_t)got - rheadlen);
    if (!rs.bodyless) {
        for (;;) {
            int n = (int)recv(u, sbuf, STREAM_BUF, 0);
            if (n == 0) break;                          /* 强制 close ⇒ 读到 EOF 就是一条响应读完 */
            if (n < 0) { if (sock_errno() == SOCK_INTR) continue; break; }
            if (net_send_full(c, sbuf, (size_t)n) != 0) break;
            fwd_bytes += n;
            if (rs.cacheable) acc_add(&acc, sbuf, (size_t)n);
        }
    }
    status = rs.status;

    /* 14. 收完 → 可缓存且攒全了才入库（部分响应不存；非 GET 的响应按 RFC 9111 §3 不进缓存） */
    if (rs.cacheable && !acc.stop && strncasecmp(r.method, "GET", 3) == 0) {
        cache_put(r.target, hbuf, rheadlen, acc.p, acc.len, rs.last_modified);
        if (hit) strcpy(tag, "命中+200(已更新)");
    }

done:
    if (u != SOCK_INVALID) sock_close(u);
    cache_obj_free(&co);
    free(hbuf);
    free(fwd_buf);
    free(sbuf);
    free(acc.p);
    if (status != 0) log_line(peer, reqline, hostport, status, fwd_bytes, tag, extra);

    /* 收尾：把客户端还没读走的字节排空再关（worker 随后 sock_close(c)）。
       缺这一步，拒绝类响应（403/503）会因为"带未读数据 close"触发 RST，客户端什么都收不到。 */
    net_close_gracefully(c, 500);
}

/* 每个连接一个线程：跑完把并发计数减回去（参数是堆分配的，线程自己负责 free）。 */
static THREAD_FN(worker) {
    conn_arg_t *a = (conn_arg_t *)arg;
    socket_t c = a->c;
    free(a);
    handle_conn(c);
    sock_close(c);
    mutex_lock(g_threads_lock);
    g_threads--;
    mutex_unlock(g_threads_lock);
    return 0;                     /* POSIX 侧返回 0 就是 NULL */
}

/* 主循环：accept → 判并发上限 → 开线程。阻塞函数，Ctrl-C 退出。
   rules_path：名单文件路径（NULL 或读不到 → 用 rules.c 里的内置默认名单）。 */
void proxy_run(int port, const char *rules_path) {
    socket_t ls;

    rules_init(rules_path);
    cache_init();
    MUTEX_INIT(g_threads_lock);
    ls = net_listen(port);
    if (ls == SOCK_INVALID) {
        fprintf(stderr, "监听 %d 端口失败（errno=%d）\n", port, (int)sock_errno());
        return;
    }
    fprintf(stderr, "HTTP 代理已启动：监听 %d（Ctrl-C 退出）\n", port);
    fprintf(stderr, "名单来源：%s\n", rules_source());

    for (;;) {
        socket_t c = accept(ls, NULL, NULL);
        conn_arg_t *a;
        thread_t t;

        if (c == SOCK_INVALID) {
            if (sock_errno() == SOCK_INTR) continue;
            break;
        }
        mutex_lock(g_threads_lock);
        if (g_threads >= MAX_THREADS) {          /* 连接风暴：直接拒，不无限开线程 */
            mutex_unlock(g_threads_lock);
            send_page(c, 503, "Service Unavailable", BODY_503);
            net_close_gracefully(c, 500);        /* 同样要排空再关，否则 503 会被 RST 吞掉 */
            sock_close(c);
            continue;
        }
        g_threads++;
        mutex_unlock(g_threads_lock);

        a = malloc(sizeof *a);
        if (!a) { sock_close(c); mutex_lock(g_threads_lock); g_threads--; mutex_unlock(g_threads_lock); continue; }
        a->c = c;
        if (thread_start(&t, worker, a) != 0) {
            free(a);
            mutex_lock(g_threads_lock); g_threads--; mutex_unlock(g_threads_lock);
            sock_close(c);
            continue;
        }
        thread_free(&t);                         /* detach：不 join，也不留僵尸线程 */
    }
    sock_close(ls);
}

#ifndef SELFTEST_BUILD
int main(int argc, char **argv) {
    int port = (argc > 1) ? atoi(argv[1]) : LISTEN_PORT;
    const char *rules_path = (argc > 2) ? argv[2] : "rules.txt";   /* 名单文件，默认取当前目录的 rules.txt */
    int i;

    for (i = 3; i < argc; i++) {                 /* 可选开关：--hide <状态码> */
        if (strcmp(argv[i], "--hide") == 0 && i + 1 < argc) g_hide_status = atoi(argv[++i]);
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "用法: %s [端口，默认 %d] [名单文件，默认 rules.txt] [--hide <状态码>]\n",
                argv[0], LISTEN_PORT);
        fprintf(stderr, "  --hide 501   不打印 501（Edge 后台的 https 握手会刷屏）\n");
        return 1;
    }
    if (plat_init() != 0) {                      /* Windows 上这一步是 WSAStartup */
        fprintf(stderr, "网络库初始化失败（errno=%d）\n", (int)sock_errno());
        return 1;
    }
    proxy_run(port, rules_path);
    plat_cleanup();
    return 0;
}
#endif
