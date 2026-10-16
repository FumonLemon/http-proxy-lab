/* rules.c —— 三张名单的读取、解析与热加载（plan.md 第 1 节 / 第 3.3 节）。
   只干一件事：把 rules.txt 变成三张能在运行时替换的名单。
   文件读不到 → 内置默认名单（等于改造前的行为）；运行中被删/读不动 → 保留上一次成功的名单。 */
#include "rules.h"
#include "platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RULES_MAX_FILE  65536        /* 名单文件大小上限（字节），够大；超了当畸形 */
#define RULES_MAX_ITEMS 256          /* 每张名单的条数上限 */
#define RULES_PATH_MAX  512

typedef struct { char *host; char *target; } redir_t;

typedef struct {
    char   **site;  int site_n;
    char   **user;  int user_n;
    redir_t *redir; int redir_n;
} lists_t;

/* ponytail: 一把锁护住全部名单读写；名单就几十条、匹配是字符串比较，锁竞争可忽略。
   真要上量再换读写锁 + 哈希索引。 */
static mutex_t   g_lock;
static char      g_path[RULES_PATH_MAX];
static long long g_mtime;                        /* 最近一次观察到的文件 mtime（毫秒）；0 = 还没看过文件 */
static lists_t   g_lists;                        /* 当前生效的名单 */
static char      g_src[RULES_PATH_MAX + 96];     /* 启动日志用的一行说明 */

/* 内置默认名单：改造前写死在 proxy.c 顶部的那三张，文件缺失时用它（CI 与自测因此零改动） */
static const char *DEF_SITE[]       = { "127.0.0.1", NULL };
static const char *DEF_USER[]       = { "192.0.2.1", NULL };
static const char *DEF_REDIR_HOST[] = { "guide.test", NULL };
static const char *DEF_REDIR_URL[]  = { "http://example.com/", NULL };

/* 大小写不敏感比较：主机名不分大小写（顺手堵掉 http://EXAMPLE.COM/ 绕过黑名单的口子）。
   自己写六行，免得再往 platform.h 里加一个平台比较宏。 */
static int ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* 小节标记（含方括号）是否等于 name，忽略大小写 */
static int section_is(const char *p, size_t n, const char *name) {
    size_t i, nl = strlen(name);
    if (n != nl) return 0;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)name[i])) return 0;
    return 1;
}

static void lists_free(lists_t *L) {
    int i;
    for (i = 0; i < L->site_n; i++) free(L->site[i]);
    for (i = 0; i < L->user_n; i++) free(L->user[i]);
    for (i = 0; i < L->redir_n; i++) { free(L->redir[i].host); free(L->redir[i].target); }
    free(L->site); free(L->user); free(L->redir);
    memset(L, 0, sizeof *L);
}

static int list_add(char ***v, int *n, const char *s, size_t len) {
    char **nv;
    char  *d;

    if (*n >= RULES_MAX_ITEMS) return 0;
    nv = realloc(*v, sizeof(char *) * (size_t)(*n + 1));
    if (!nv) return 0;
    *v = nv;
    d = malloc(len + 1);
    if (!d) return 0;
    memcpy(d, s, len);
    d[len] = '\0';
    (*v)[*n] = d;
    (*n)++;
    return 1;
}

static int redir_add(lists_t *L, const char *host, size_t hl, const char *url, size_t ul) {
    redir_t *nr;
    char *h, *u;

    if (L->redir_n >= RULES_MAX_ITEMS) return 0;
    nr = realloc(L->redir, sizeof(redir_t) * (size_t)(L->redir_n + 1));
    if (!nr) return 0;
    L->redir = nr;
    h = malloc(hl + 1);
    u = malloc(ul + 1);
    if (!h || !u) { free(h); free(u); return 0; }
    memcpy(h, host, hl); h[hl] = '\0';
    memcpy(u, url, ul);  u[ul] = '\0';
    L->redir[L->redir_n].host = h;
    L->redir[L->redir_n].target = u;
    L->redir_n++;
    return 1;
}

/* 逐行解析：空行与 '#' 注释跳过；[site] / [user] / [redirect] 切名单；
   [redirect] 每行两个字段（主机 + 目标网址），只写了一个字段的行忽略。 */
static int parse_lines(const char *buf, size_t len, lists_t *L) {
    size_t i = 0;
    int sec = 0;                                   /* 0 = 不在任何小节里（该段内容全忽略） */

    while (i < len) {
        size_t j = i, e, a;

        while (j < len && buf[j] != '\n') j++;      /* 行尾 */
        e = j;
        while (e > i && (buf[e - 1] == '\r' || buf[e - 1] == ' ' || buf[e - 1] == '\t')) e--;   /* 去 CR 与尾空白 */

        a = i;
        while (a < e && (buf[a] == ' ' || buf[a] == '\t')) a++;                                 /* 去首空白 */

        if (a < e && buf[a] != '#') {
            if (buf[a] == '[') {
                if (section_is(buf + a, e - a, "[site]"))          sec = 1;
                else if (section_is(buf + a, e - a, "[user]"))     sec = 2;
                else if (section_is(buf + a, e - a, "[redirect]")) sec = 3;
                else sec = 0;
            } else if (sec == 1) {
                if (!list_add(&L->site, &L->site_n, buf + a, e - a)) return 0;
            } else if (sec == 2) {
                if (!list_add(&L->user, &L->user_n, buf + a, e - a)) return 0;
            } else if (sec == 3) {
                size_t b = a, u;
                while (b < e && buf[b] != ' ' && buf[b] != '\t') b++;      /* 主机名 */
                u = b;
                while (u < e && (buf[u] == ' ' || buf[u] == '\t')) u++;    /* 目标网址 */
                if (b > a && u < e) {
                    if (!redir_add(L, buf + a, b - a, buf + u, e - u)) return 0;
                }
            }
        }
        i = (j < len) ? j + 1 : len;
    }
    return 1;
}

/* 读名单文件并装上：1 = 读到了；0 = 没读到（调用方保持原名单不动）。调用方持锁。 */
static int load_file_locked(void) {
    FILE   *f;
    char   *buf;
    size_t  got;
    int     bad;
    lists_t L;

    f = fopen(g_path, "rb");
    if (!f) return 0;
    buf = malloc(RULES_MAX_FILE + 1);
    if (!buf) { fclose(f); return 0; }

    got = fread(buf, 1, RULES_MAX_FILE, f);
    bad = ferror(f) || (got == RULES_MAX_FILE);     /* 读错误，或文件大到顶到上限（当畸形） */
    fclose(f);
    if (bad || got == 0) { free(buf); return 0; }   /* 空文件不接：编辑器清空的一瞬间别把名单抹了 */
    buf[got] = '\0';

    memset(&L, 0, sizeof L);
    if (!parse_lines(buf, got, &L)) { lists_free(&L); free(buf); return 0; }
    free(buf);

    lists_free(&g_lists);
    g_lists = L;                                   /* 先建好新表再换指针：读者看不到半截名单 */
    g_mtime = plat_file_mtime_ms(g_path);
    snprintf(g_src, sizeof g_src, "%s（%d 网站 / %d 用户 / %d 引导）",
             g_path, g_lists.site_n, g_lists.user_n, g_lists.redir_n);
    return 1;
}

/* 装内置默认名单 */
static void load_defaults_locked(void) {
    lists_t L;
    int i;

    memset(&L, 0, sizeof L);
    for (i = 0; DEF_SITE[i]; i++)
        if (!list_add(&L.site, &L.site_n, DEF_SITE[i], strlen(DEF_SITE[i]))) break;
    for (i = 0; DEF_USER[i]; i++)
        if (!list_add(&L.user, &L.user_n, DEF_USER[i], strlen(DEF_USER[i]))) break;
    for (i = 0; DEF_REDIR_HOST[i]; i++)
        if (!redir_add(&L, DEF_REDIR_HOST[i], strlen(DEF_REDIR_HOST[i]),
                       DEF_REDIR_URL[i], strlen(DEF_REDIR_URL[i]))) break;

    lists_free(&g_lists);
    g_lists = L;
    g_mtime = 0;                                   /* 没从文件读到过：文件后来出现了也能被拾起 */
    snprintf(g_src, sizeof g_src, "内置默认名单（%d 网站 / %d 用户 / %d 引导）",
             g_lists.site_n, g_lists.user_n, g_lists.redir_n);
}

static int strlist_has(char **v, int n, const char *s) {
    int i;
    for (i = 0; i < n; i++) if (ci_equal(v[i], s)) return 1;
    return 0;
}

void rules_init(const char *path) {
    MUTEX_INIT(g_lock);
    if (path && path[0]) {
        size_t n = strlen(path);
        if (n >= sizeof g_path) n = sizeof g_path - 1;
        memcpy(g_path, path, n);
        g_path[n] = '\0';
    } else {
        g_path[0] = '\0';                          /* 明确要求"用内置默认名单"：不再轮询任何文件 */
    }
    mutex_lock(g_lock);
    if (!(g_path[0] && load_file_locked())) load_defaults_locked();
    mutex_unlock(g_lock);
}

void rules_reload_if_changed(void) {
    long long m;

    if (!g_path[0]) return;                        /* 内置默认名单：没什么可轮的 */
    m = plat_file_mtime_ms(g_path);
    if (m == 0 || m == g_mtime) return;            /* 文件不在，或没变过 */

    mutex_lock(g_lock);
    if (m != g_mtime) {
        if (!load_file_locked()) g_mtime = m;      /* 读失败：记住这个版本，别每个请求都重试同一个坏文件 */
    }
    mutex_unlock(g_lock);
}

const char *rules_source(void) { return g_src; }

int rules_site_blocked(const char *host) {
    int r;
    mutex_lock(g_lock);
    r = strlist_has(g_lists.site, g_lists.site_n, host);
    mutex_unlock(g_lock);
    return r;
}

int rules_user_blocked(const char *ip) {
    int r;
    mutex_lock(g_lock);
    r = strlist_has(g_lists.user, g_lists.user_n, ip);
    mutex_unlock(g_lock);
    return r;
}

int rules_redirect_target(const char *host, char *out, size_t cap) {
    int i, hit = 0;

    if (cap == 0) return 0;
    out[0] = '\0';
    mutex_lock(g_lock);
    for (i = 0; i < g_lists.redir_n; i++) {
        if (ci_equal(g_lists.redir[i].host, host)) {
            snprintf(out, cap, "%s", g_lists.redir[i].target);   /* 拷出来：锁一放，名单可能已经被换掉 */
            hit = 1;
            break;
        }
    }
    mutex_unlock(g_lock);
    return hit;
}

void rules_counts(int *site, int *user, int *redir) {
    mutex_lock(g_lock);
    if (site)  *site  = g_lists.site_n;
    if (user)  *user  = g_lists.user_n;
    if (redir) *redir = g_lists.redir_n;
    mutex_unlock(g_lock);
}
