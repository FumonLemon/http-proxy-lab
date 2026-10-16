/* tests/selftest.c —— 假原服务器 + 脚本化客户端 + 8 条断言（plan.md 第 3.6 节）。
   一个文件三部分：假原服务器线程、客户端辅助函数、断言。它同时是 CI 的测试入口和本机手工调试工具。
   编译：make selftest（带 -DSELFTEST_BUILD，所以 proxy.c 里的 main 不参与链接）。 */

#include "../platform.h"
#include "../net.h"
#include "../http.h"
#include "../cache.h"
#include "../rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* proxy.c 没有头文件（plan 第 2.1 节的文件清单），这两条由本文件自己声明 */
void proxy_run(int port, const char *rules_path);
void proxy_set_user_block(const char *ip);

/* ---- 固定常量：端口不随机取，Windows 上少一段代码（plan 第 2.4 节） ---- */
#define FAKE_PORT   18080      /* 假原服务器 */
#define PROXY_PORT  18081      /* 被测代理（不用 8080，免得和手动起的代理撞） */
#define BIG_BODY    200000     /* 比 64 KB 缓冲大得多 */
#define SLOW_BODY   4296       /* 头部 + 200 + 4096 */
#define SLOW_GAP_MS 300
#define SMALL_BODY  128
#define NOTMOD_BODY 512
static const char LM[] = "Wed, 29 Jun 2022 00:23:33 GMT";   /* 固定 Last-Modified：响应可比对、可复现 */

/* ---- 断言框架 ---- */
static int g_pass, g_fail, g_cur_fail;

static void expect(int cond, const char *what) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) g_cur_fail++;
}

static void finish(const char *name) {
    if (g_cur_fail == 0) { printf("PASS %s\n\n", name); g_pass++; }
    else { printf("FAIL %s（%d 处）\n\n", name, g_cur_fail); g_fail++; }
    g_cur_fail = 0;
}

/* ---- 假原服务器的状态 ---- */
static mutex_t g_origin_lock;
static int     g_origin_conns;      /* 一共接过多少条连接：T5 用它证明"代理确实没连原服务器" */
static int     g_origin_saw_ims;    /* 是否收到过 If-Modified-Since：T4 用它证明条件请求真的发出去了 */

static int origin_conns(void) {
    int n; mutex_lock(g_origin_lock); n = g_origin_conns; mutex_unlock(g_origin_lock); return n;
}
static int origin_saw_ims(void) {
    int n; mutex_lock(g_origin_lock); n = g_origin_saw_ims; mutex_unlock(g_origin_lock); return n;
}

static const char R304[] = "HTTP/1.1 304 Not Modified\r\n\r\n";
static const char RBAD[] = "HTTP/1.1 ABC X\r\n\r\n";       /* 畸形状态行：代理必须不崩 */
static const char R404[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

static void send_hdr(socket_t c, size_t bodylen) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %u\r\n"
                     "Last-Modified: %s\r\nConnection: close\r\n\r\n",
                     (unsigned)bodylen, LM);
    net_send_full(c, hdr, (size_t)n);
}

static void send_fill(socket_t c, size_t n, char fill) {
    char blk[1024];
    memset(blk, fill, sizeof blk);
    while (n) {
        size_t chunk = n < sizeof blk ? n : sizeof blk;
        if (net_send_full(c, blk, chunk) != 0) return;
        n -= chunk;
    }
}

/* 从请求行取路径（origin-form），够假服务器分流用 */
static void req_path(const char *b, size_t n, char *out, size_t cap) {
    size_t i = 0, j;
    out[0] = '/'; out[1] = '\0';
    while (i < n && b[i] != ' ') i++;
    while (i < n && b[i] == ' ') i++;
    j = i;
    while (j < n && b[j] != ' ' && b[j] != '\r') j++;
    if (j > i && j - i + 2 <= cap) { memcpy(out, b + i, j - i); out[j - i] = '\0'; }
}

static void origin_handle(socket_t c) {
    static char buf[HDR_MAX];
    char path[256];
    size_t hl = 0, vl = 0;
    long cl = 0;

    int got = http_read_head(c, buf, sizeof buf, &hl);
    if (got <= 0) return;
    if (http_find_header(buf, hl, "If-Modified-Since", &vl)) {
        mutex_lock(g_origin_lock); g_origin_saw_ims = 1; mutex_unlock(g_origin_lock);
    }
    req_path(buf, hl, path, sizeof path);

    /* 请求体（POST）：按 Content-Length 读齐，回一条把自己的"收到多少、校验和是多少"写进头部的响应 */
    vl = 0;
    {
        const char *v = http_find_header(buf, hl, "Content-Length", &vl);
        if (v && vl > 0 && vl < 32) { char t[32]; memcpy(t, v, vl); t[vl] = '\0'; cl = atol(t); }
    }
    if (cl > 0) {
        char *body = malloc((size_t)cl);
        int have = 0;
        unsigned sum = 0;
        char hdr[192];
        int n;
        if (body) {
            int extra = got - (int)hl;                       /* 同一次 recv 里带来的主体开头 */
            if (extra > 0 && extra <= cl) { memcpy(body, buf + hl, (size_t)extra); have = extra; }
            if (have < cl) {
                int r = net_recv_full(c, body + have, (size_t)(cl - have));
                if (r > 0) have += r;
            }
            for (int i = 0; i < have; i++) sum = (sum + (unsigned char)body[i]) & 0xffffu;
            free(body);
        }
        n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                     "X-Origin-Got-Len: %d\r\nX-Origin-Sum: %u\r\nConnection: close\r\n\r\n",
                     have, sum);
        net_send_full(c, hdr, (size_t)n);
        return;
    }

    if (strcmp(path, "/small") == 0)       { send_hdr(c, SMALL_BODY);  send_fill(c, SMALL_BODY, 'S'); }
    else if (strcmp(path, "/big") == 0)    { send_hdr(c, BIG_BODY);    send_fill(c, BIG_BODY, 'B'); }
    else if (strcmp(path, "/slow") == 0)   {                            /* 分段到达：先 200 字节，300 ms 后再 4096 */
        send_hdr(c, SLOW_BODY);
        send_fill(c, 200, 's');
        sleep_ms(SLOW_GAP_MS);
        send_fill(c, SLOW_BODY - 200, 's');
    }
    else if (strcmp(path, "/notmod") == 0) {
        if (vl > 0) net_send_full(c, R304, sizeof R304 - 1);             /* 有条件请求 → 304，无主体 */
        else        { send_hdr(c, NOTMOD_BODY); send_fill(c, NOTMOD_BODY, 'N'); }
    }
    else if (strcmp(path, "/bad") == 0)    { net_send_full(c, RBAD, sizeof RBAD - 1); }
    else                                   { net_send_full(c, R404, sizeof R404 - 1); }
}

/* 假原服务器：单线程顺序处理（自测是顺序发请求的，不需要并发） */
static THREAD_FN(origin_thread) {
    socket_t ls;
    (void)arg;                     /* 线程参数没用到：不写这一行编译器会报 -Wunused-parameter */
    ls = net_listen(FAKE_PORT);
    if (ls == SOCK_INVALID) { fprintf(stderr, "假原服务器监听 %d 失败\n", FAKE_PORT); return 0; }
    for (;;) {
        socket_t c = accept(ls, NULL, NULL);
        if (c == SOCK_INVALID) continue;
        mutex_lock(g_origin_lock); g_origin_conns++; mutex_unlock(g_origin_lock);
        origin_handle(c);
        sock_close(c);
    }
    return 0;
}

/* 代理线程：proxy_run 是阻塞的主循环 */
static THREAD_FN(proxy_thread) {
    (void)arg;
    proxy_run(PROXY_PORT, NULL);      /* NULL = 用内置默认名单，不受仓库里的 rules.txt 影响 */
    return 0;
}

/* ---- 客户端辅助 ---- */
static int read_all(socket_t s, char *out, size_t cap) {
    size_t got = 0;
    for (;;) {
        int n;
        if (got + 1 >= cap) break;
        n = (int)recv(s, out + got, SOCK_IO_LEN(cap - got - 1), 0);
        if (n > 0) { got += (size_t)n; continue; }
        if (n < 0 && sock_errno() == SOCK_INTR) continue;
        break;
    }
    out[got] = '\0';
    return (int)got;
}

/* 往代理发一段原始字节（用于畸形请求） */
static int proxy_raw(const char *raw, char *out, size_t cap) {
    socket_t s = net_connect("127.0.0.1", PROXY_PORT);
    int n;
    if (s == SOCK_INVALID) return -1;
    if (net_send_full(s, raw, strlen(raw)) != 0) { sock_close(s); return -1; }
    n = read_all(s, out, cap);
    sock_close(s);
    return n;
}

/* 走代理发一条绝对形式请求（浏览器就是这样发的） */
static int proxy_get(const char *url, char *out, size_t cap) {
    char req[1024];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", url);
    return proxy_raw(req, out, cap);
}

/* 走代理发一条 POST（带请求体）：验证代理按 Content-Length 把主体读齐并转发 */
static int proxy_post(const char *url, const char *body, char *out, size_t cap) {
    char req[1024];
    snprintf(req, sizeof req,
             "POST %s HTTP/1.1\r\nHost: x\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
             url, (unsigned)strlen(body), body);
    return proxy_raw(req, out, cap);
}

/* 直连假原服务器发 origin-form 请求：用来拿"标准答案"和经代理的字节比对 */
static int direct_get(const char *path, char *out, size_t cap) {
    socket_t s = net_connect("127.0.0.1", FAKE_PORT);
    char req[512];
    int n;
    if (s == SOCK_INVALID) return -1;
    n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n",
                 path, FAKE_PORT);
    if (net_send_full(s, req, (size_t)n) != 0) { sock_close(s); return -1; }
    n = read_all(s, out, cap);
    sock_close(s);
    return n;
}

static int wait_port(int port, int ms) {
    int i;
    for (i = 0; i < ms; i += 20) {
        socket_t s = net_connect("127.0.0.1", port);
        if (s != SOCK_INVALID) { sock_close(s); return 0; }
        sleep_ms(20);
    }
    return -1;
}

static char buf1[400000], buf2[400000];   /* /big 的 200000 字节要装得下 */

/* ---- 8 条断言 ---- */

/* T1 基本转发：经代理的字节与直连逐字节一致；带请求体的 POST 也要整段转过去 */
static void t1_basic(void) {
    int a = direct_get("/small", buf1, sizeof buf1);
    int b = proxy_get("http://localhost:18080/small", buf2, sizeof buf2);
    expect(a > 0, "直连假原服务器拿到响应");
    expect(b == a, "经代理的字节数与直连一致");
    expect(b > 0 && b == a && memcmp(buf1, buf2, (size_t)b) == 0, "经代理的字节内容与直连逐字节一致");
    {
        const char *body = "name=zeng&msg=hello-proxy";     /* 26 字节，故意不整对齐缓冲区 */
        unsigned sum = 0;
        char want[64];
        size_t i;
        int n;
        for (i = 0; i < strlen(body); i++) sum = (sum + (unsigned char)body[i]) & 0xffffu;
        n = proxy_post("http://localhost:18080/post", body, buf2, sizeof buf2);
        snprintf(want, sizeof want, "X-Origin-Got-Len: %u", (unsigned)strlen(body));
        expect(n > 0 && strstr(buf2, want) != NULL, "POST 的请求体被完整转发（原服务器收到的长度对）");
        snprintf(want, sizeof want, "X-Origin-Sum: %u", sum);
        expect(n > 0 && strstr(buf2, want) != NULL, "POST 的请求体内容逐字节正确（校验和对）");
    }
    finish("T1 基本转发（GET 与带主体的 POST）");
}

/* T2 大响应：200000 字节 > 65536 缓冲，必须循环转发 */
static void t2_big(void) {
    int a = direct_get("/big", buf1, sizeof buf1);
    int b = proxy_get("http://localhost:18080/big", buf2, sizeof buf2);
    expect(a > (int)BIG_BODY, "直连拿到大于 200000 字节的响应");
    expect(b == a, "经代理的大响应长度与直连一致");
    expect(b > 0 && b == a && memcmp(buf1, buf2, (size_t)b) == 0, "大响应内容逐字节一致");
    finish("T2 大响应（200000 字节，跨缓冲区循环转发）");
}

/* T3 慢响应：分两段到达也能完整转发 */
static void t3_slow(void) {
    int a = direct_get("/slow", buf1, sizeof buf1);
    long long t0 = plat_now_ms();
    int b = proxy_get("http://localhost:18080/slow", buf2, sizeof buf2);
    long long dt = plat_now_ms() - t0;
    expect(b == a, "分段到达的响应长度与直连一致（没被截断）");
    expect(b > 0 && b == a && memcmp(buf1, buf2, (size_t)b) == 0, "分段到达的响应内容逐字节一致");
    expect(dt >= 250, "耗时 ≥ 250 ms（300 ms 的那一段确实被转发出来了）");
    printf("      实测耗时 %lld ms\n", dt);
    finish("T3 慢响应（两段 300 ms 间隔）");
}

/* T4 缓存：命中 → 带 If-Modified-Since 问原服务器 → 304 → 用缓存应答；外加两条上限自检 */
static void t4_cache(void) {
    int n0 = origin_conns();
    int a = proxy_get("http://localhost:18080/notmod", buf1, sizeof buf1);
    int n1 = origin_conns();
    int b = proxy_get("http://localhost:18080/notmod", buf2, sizeof buf2);
    int n2 = origin_conns();
    int i;

    expect(a > 0, "第一次请求（未命中）拿到完整对象");
    expect(b == a && b > 0 && memcmp(buf1, buf2, (size_t)b) == 0, "第二次拿到的是缓存里的完整对象（与第一次逐字节一致）");
    expect(n1 - n0 == 1, "第一次未命中：代理连了原服务器 1 次");
    expect(n2 - n1 == 1, "第二次仍连了原服务器 1 次（去发条件请求，不是直接回缓存）");
    expect(origin_saw_ims() == 1, "原服务器收到过 If-Modified-Since（实验内容第 2 条）");

    for (i = 0; i < 70; i++) {                        /* C1 的"算过"：条目上限 */
        char u[64];
        snprintf(u, sizeof u, "http://t/%d", i);
        cache_put(u, "H\r\n", 3, "B", 1, LM);
    }
    expect(cache_count() == CACHE_MAX_ENTRIES, "插入 70 条后表内条数 = 64（条目上限生效）");
    {
        char tiny[8] = "x";                           /* 长度守卫在拷贝之前就返回了，不需要真分配 5 MB */
        cache_put("http://t/too-big", "H\r\n", 3, tiny, 5u * 1024u * 1024u, LM);
    }
    expect(cache_count() == CACHE_MAX_ENTRIES, "单条 5 MB（超 4 MB 上限）不落表");
    printf("      缓存当前 %d 条 / %u 字节\n", cache_count(), (unsigned)cache_bytes());
    finish("T4 缓存（If-Modified-Since → 304 → 用缓存应答 + 上限）");
}

/* T5 网站过滤：黑名单主机被拦，且代理确实没连原服务器 */
static void t5_site_filter(void) {
    int n0 = origin_conns();
    int b = proxy_get("http://127.0.0.1:18080/small", buf2, sizeof buf2);
    int n1 = origin_conns();
    expect(b > 0 && strstr(buf2, "403") != NULL, "黑名单主机 → 客户端拿到 403");
    expect(n1 == n0, "原服务器连接数没变（代理没有连原服务器）");
    finish("T5 网站过滤");
}

/* T6 用户过滤：单机只能注入判据验证逻辑（plan 第 7 节 T6 允许的写法，报告里如实写明） */
static void t6_user_filter(void) {
    int b;
    proxy_set_user_block("127.0.0.1");
    b = proxy_get("http://localhost:18080/small", buf2, sizeof buf2);
    expect(b > 0 && strstr(buf2, "403") != NULL, "来源 IP 在名单内 → 403");
    proxy_set_user_block(NULL);
    b = proxy_get("http://localhost:18080/small", buf2, sizeof buf2);
    expect(b > 0 && strstr(buf2, "200 OK") != NULL, "名单外的来源 IP 正常放行（对照）");
    finish("T6 用户过滤（单机注入判据）");
}

/* T7 网站引导：被引导的域名拿到 302 + Location（目标来自名单，原来是回一张写死的自制页） */
static void t7_redirect(void) {
    int n0 = origin_conns();
    int b = proxy_get("http://guide.test/anything", buf2, sizeof buf2);
    int n1 = origin_conns();
    expect(b > 0 && strstr(buf2, "302") != NULL, "引导域名 → 302");
    expect(b > 0 && strstr(buf2, "Location: http://example.com/") != NULL, "302 带上了名单里的目标网址");
    expect(n1 == n0, "没有连原服务器（引导在连原服务器之前完成）");
    finish("T7 网站引导（302 跳转）");
}

/* T8 解析与畸形输入：解析断言 + 400/502 + 进程存活 */
static void t8_parse_and_bad(void) {
    http_req_t r;
    char fwd[4096];
    int n, b;
    const char *h1 = "GET http://example.com/a/b.html HTTP/1.1\r\nHost: example.com\r\nConnection: keep-alive\r\n\r\n";
    const char *h2 = "GET http://example.com:8080/ HTTP/1.0\r\n\r\n";
    const char *h3 = "GET http://host HTTP/1.1\r\n\r\n";
    const char *h4 = "BREAK\r\n\r\n";
    const char *h5 = "GET /index.html HTTP/1.1\r\nHost: a\r\n\r\n";
    const char *h6 = "CONNECT example.com:443 HTTP/1.1\r\n\r\n";

    expect(http_parse_request(h1, strlen(h1), &r) == 0 && strcmp(r.method, "GET") == 0 &&
           strcmp(r.host, "example.com") == 0 && r.port == 80 && strcmp(r.path, "/a/b.html") == 0,
           "绝对形式请求行切出 method / host / port / path");
    expect(http_parse_request(h2, strlen(h2), &r) == 0 && r.port == 8080 && strcmp(r.path, "/") == 0,
           "带端口的 URL 切得对，路径缺省为 /");
    expect(http_parse_request(h3, strlen(h3), &r) == 0 && strcmp(r.path, "/") == 0,
           "没有路径的绝对 URL 补 /");
    expect(http_parse_request(h4, strlen(h4), &r) == -1, "畸形请求行（没有空格）判为 -1");
    expect(http_parse_request(h5, strlen(h5), &r) == -2, "非绝对形式请求判为 -2（回 501）");
    expect(http_parse_request(h6, strlen(h6), &r) == -2, "CONNECT 判为 -2（本实验不做隧道）");

    http_parse_request(h1, strlen(h1), &r);
    n = http_build_forward(&r, LM, fwd, sizeof fwd);
    expect(n > 0 && strstr(fwd, "GET /a/b.html HTTP/1.1\r\n") == fwd, "转发报文是 origin-form");
    expect(n > 0 && strstr(fwd, "Connection: close\r\n") != NULL, "转发报文里补了 Connection: close");
    expect(n > 0 && strstr(fwd, "keep-alive") == NULL, "原有的 Connection 行被删掉");
    expect(n > 0 && strstr(fwd, "If-Modified-Since: Wed, 29 Jun 2022 00:23:33 GMT\r\n") != NULL,
           "命中缓存时带上了 If-Modified-Since");

    b = proxy_raw("BREAK\r\n\r\n", buf2, sizeof buf2);
    expect(b >= 0, "畸形请求行没有让代理崩");
    expect(b == 0 || strstr(buf2, "400") != NULL, "畸形请求行 → 400 或直接关连接");

    b = proxy_get("http://localhost:18080/bad", buf2, sizeof buf2);
    expect(b > 0 && strstr(buf2, "502") != NULL, "原服务器状态行畸形 → 502（代理不崩）");

    b = proxy_get("http://localhost:18080/small", buf2, sizeof buf2);
    expect(b > 0 && strstr(buf2, "200 OK") != NULL, "复查：代理仍能正常服务（进程存活）");
    finish("T8 请求解析与畸形输入");
}

/* T9 名单热加载：改名单文件，不重启进程就生效（plan 第 3.5 节）。
   它换的是全局名单表，所以安排到最后跑，收尾时还原内置默认名单。 */
static void t9_rules_reload(void) {
    const char *p = "selftest_rules.txt";
    char dst[512];
    FILE *f;

    f = fopen(p, "wb");
    if (!f) { expect(0, "能写出测试用名单文件"); finish("T9 名单热加载"); return; }
    fputs("# selftest 用\n"
          "[site]\n"
          "blocked.test\n"
          "[user]\n"
          "nobody.invalid\n"
          "[redirect]\n"
          "jump.test    http://first.test/\n", f);
    fclose(f);

    rules_init(p);
    expect(rules_site_blocked("blocked.test") == 1, "文件里的网站名单生效");
    expect(rules_site_blocked("BLOCKED.TEST") == 1, "主机名匹配不区分大小写");
    expect(rules_user_blocked("nobody.invalid") == 1, "文件里的用户名单生效");
    expect(rules_redirect_target("jump.test", dst, sizeof dst) == 1 &&
           strcmp(dst, "http://first.test/") == 0, "引导目标取自文件");
    expect(rules_site_blocked("keep.test") == 0, "没写进文件的主机不命中（对照）");

    sleep_ms(1100);                       /* 等 mtime 走过去：粗粒度文件系统也够 */
    f = fopen(p, "wb");
    if (f) {
        fputs("[site]\nkeep.test\n\n[redirect]\njump.test    http://second.test/\n", f);
        fclose(f);
    }
    rules_reload_if_changed();            /* 这就是代理每个请求前调的那一句 */
    expect(rules_site_blocked("keep.test") == 1 && rules_site_blocked("blocked.test") == 0,
           "改文件后新条目生效、旧条目失效（进程没重启）");
    expect(rules_redirect_target("jump.test", dst, sizeof dst) == 1 &&
           strcmp(dst, "http://second.test/") == 0, "引导目标跟着文件一起改了");

    remove(p);
    rules_init(NULL);                     /* 还原内置默认名单，不影响别的用例 */
    expect(rules_site_blocked("127.0.0.1") == 1, "还原内置默认名单（127.0.0.1 又命中）");
    finish("T9 名单热加载（rules.txt）");
}

int main(void) {
    thread_t t;

    if (plat_init() != 0) { fprintf(stderr, "plat_init 失败\n"); return 1; }
    MUTEX_INIT(g_origin_lock);

    printf("== 代理自测：假原服务器 %d + 代理 %d ==\n\n", FAKE_PORT, PROXY_PORT);

    if (thread_start(&t, origin_thread, NULL) != 0) { fprintf(stderr, "起假原服务器失败\n"); return 1; }
    thread_free(&t);
    if (thread_start(&t, proxy_thread, NULL) != 0) { fprintf(stderr, "起代理线程失败\n"); return 1; }
    thread_free(&t);
    if (wait_port(FAKE_PORT, 3000) != 0 || wait_port(PROXY_PORT, 3000) != 0) {
        fprintf(stderr, "服务没起来（端口被占用？）\n");
        return 1;
    }

    t1_basic();
    t2_big();
    t3_slow();
    t4_cache();
    t5_site_filter();
    t6_user_filter();
    t7_redirect();
    t8_parse_and_bad();
    t9_rules_reload();

    printf("== %s %d/%d ==\n", g_fail ? "FAIL" : "PASS", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
