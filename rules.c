/* rules.c —— 三张名单的读取、解析与热加载（plan.md 第 1 节 / 第 3.3 节）。
   只干一件事：把 rules.txt 变成三张能在运行时替换的名单。
   文件读不到 → 内置默认名单（等于改造前的行为）；运行中被删/读不动 → 保留上一次成功的名单。 */
#include "rules.h"
#include "platform.h"
#include "http.h"          /* 只是借 http_find_header 找 Proxy-Authorization 头 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RULES_MAX_FILE  65536        /* 名单文件大小上限（字节），够大；超了当畸形 */
#define RULES_MAX_ITEMS 256          /* 每张名单的条数上限 */
#define RULES_PATH_MAX  512
#define AUTH_LIMIT      8            /* 认证用户数上限（够演示；多了该换哈希表） */
#define AUTH_USER_MAX   32
#define AUTH_PASS_MAX   64
#define AUTH_DENY_MAX   4            /* 每个用户最多禁几台主机 */
#define AUTH_HOST_MAX   128

typedef struct { char *host; char *target; } redir_t;

/* 一个认证用户：用户名 / 密码 / 被禁访问的主机（0～AUTH_DENY_MAX 个） */
typedef struct {
    char user[AUTH_USER_MAX];
    char pass[AUTH_PASS_MAX];
    char deny[AUTH_DENY_MAX][AUTH_HOST_MAX];
    int  deny_n;
} auth_t;

typedef struct {
    char   **site;  int site_n;
    char   **user;  int user_n;
    redir_t *redir; int redir_n;
    auth_t  *auth;  int auth_n;
} lists_t;

/* 解析一行 [auth]（实现在下面）：parse_lines 先用，故前置声明 */
static int auth_add(lists_t *L, const char *line, size_t len);

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
    free(L->site); free(L->user); free(L->redir); free(L->auth);
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

/* 逐行解析：空行与 '#' 注释跳过；[site] / [user] / [redirect] / [auth] 切名单；
   [redirect] 每行两个字段（主机 + 目标网址），只写了一个字段的行忽略；
   [auth] 每行 user:pass[:deny=host,host]，格式不对的行忽略。 */
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
                else if (section_is(buf + a, e - a, "[auth]"))     sec = 4;
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
            } else if (sec == 4) {
                if (!auth_add(L, buf + a, e - a)) return 0;
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
    {
        int n = snprintf(g_src, sizeof g_src, "%s（%d 网站 / %d 用户 / %d 引导",
                         g_path, g_lists.site_n, g_lists.user_n, g_lists.redir_n);
        if (n > 0 && (size_t)n < sizeof g_src) {
            if (g_lists.auth_n > 0)
                snprintf(g_src + n, sizeof g_src - (size_t)n, " / 认证 %d 个账号）", g_lists.auth_n);
            else
                snprintf(g_src + n, sizeof g_src - (size_t)n, " / 认证关闭）");
        }
    }
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
    snprintf(g_src, sizeof g_src, "内置默认名单（%d 网站 / %d 用户 / %d 引导 / 认证关闭）",
             g_lists.site_n, g_lists.user_n, g_lists.redir_n);
}

/* ---- 代理认证（实验内容 3 的"用户过滤"按账号实现）----
   [auth] 段一行一个用户：user:pass            → 该用户无限制
                          user:pass:deny=a.com,b.com → 该用户被禁止访问这些主机
   客户端按 RFC 7235 发 Proxy-Authorization: Basic base64(user:pass)。
   没有 [auth] 段 = 不启用认证，其余功能完全不受影响。 */

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* base64 解码：in[0..n) → out（自动补 0 结尾）。返回解码字节数，-1 = 非法输入。 */
static int b64_decode(const char *in, size_t n, char *out, size_t cap) {
    size_t i, o = 0;
    int quad[4], q = 0;

    if (n == 0 || cap == 0) return -1;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)in[i], v;
        if (c == '=') break;                     /* padding：后面没有数据了 */
        v = b64_val(c);
        if (v < 0) return -1;                    /* 非法字符（含空格）→ 当凭据格式错 */
        quad[q++] = v;
        if (q == 4) {
            if (o + 3 >= cap) return -1;
            out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
            out[o++] = (char)(((quad[1] & 15) << 4) | (quad[2] >> 2));
            out[o++] = (char)(((quad[2] & 3) << 6) | quad[3]);
            q = 0;
        }
    }
    if (q == 2) {
        if (o + 1 >= cap) return -1;
        out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
    } else if (q == 3) {
        if (o + 2 >= cap) return -1;
        out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
        out[o++] = (char)(((quad[1] & 15) << 4) | (quad[2] >> 2));
    } else if (q != 0) {
        return -1;                               /* 只有一个字符：不可能是合法 base64 */
    }
    out[o] = 0;
    return (int)o;
}

/* 解析一行 [auth]：成功或"这行不算"都返回 1，只有内存不足返回 0（让整份名单作废）。 */
static int auth_add(lists_t *L, const char *line, size_t len) {
    const char *end = line + len;
    const char *c1, *c2;
    size_t ul, pl;
    auth_t *na;

    if (L->auth_n >= AUTH_LIMIT) return 1;
    c1 = memchr(line, ':', len);
    if (!c1) return 1;                                   /* 没有冒号：跳过 */
    c2 = memchr(c1 + 1, ':', (size_t)(end - c1 - 1));
    ul = (size_t)(c1 - line);
    pl = c2 ? (size_t)(c2 - c1 - 1) : (size_t)(end - c1 - 1);
    if (ul == 0 || ul >= AUTH_USER_MAX || pl == 0 || pl >= AUTH_PASS_MAX) return 1;

    na = realloc(L->auth, sizeof(auth_t) * (size_t)(L->auth_n + 1));
    if (!na) return 0;
    L->auth = na;
    memset(&na[L->auth_n], 0, sizeof na[L->auth_n]);
    memcpy(na[L->auth_n].user, line, ul);
    memcpy(na[L->auth_n].pass, c1 + 1, pl);
    na[L->auth_n].user[ul] = 0;
    na[L->auth_n].pass[pl] = 0;

    if (c2) {                                            /* 可选字段 deny=host1,host2 */
        const char *d = c2 + 1;
        if ((size_t)(end - d) > 5 && strncasecmp(d, "deny=", 5) == 0) {
            d += 5;
            while (d < end && L->auth[L->auth_n].deny_n < AUTH_DENY_MAX) {
                const char *comma = memchr(d, ',', (size_t)(end - d));
                size_t hl = comma ? (size_t)(comma - d) : (size_t)(end - d);
                if (hl > 0 && hl < AUTH_HOST_MAX) {
                    memcpy(L->auth[L->auth_n].deny[L->auth[L->auth_n].deny_n], d, hl);
                    L->auth[L->auth_n].deny_n++;
                }
                if (!comma) break;
                d = comma + 1;
            }
        }
    }
    L->auth_n++;
    return 1;
}

/* 认证 + 授权。返回值见 rules.h。凭据只在内存里比对，绝不转发给上游。 */
int rules_auth_check(const char *head, size_t len, const char *host,
                     char *user_out, size_t cap) {
    const char *v;
    size_t vl = 0;
    char b64buf[256];
    char user[AUTH_USER_MAX], pass[AUTH_PASS_MAX];
    const char *colon;
    int i, r = 1, enabled;

    if (user_out && cap) user_out[0] = 0;

    mutex_lock(g_lock);
    enabled = g_lists.auth_n > 0;
    mutex_unlock(g_lock);
    if (!enabled) return 0;                       /* 没配 [auth]：不启用认证 */

    v = http_find_header(head, len, "Proxy-Authorization", &vl);
    if (!v) return 1;                             /* 没带凭据 → 407 */
    while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
    if (vl < 7 || strncasecmp(v, "Basic ", 6) != 0) return 1;
    v += 6;
    vl -= 6;
    while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
    if (b64_decode(v, vl, b64buf, sizeof b64buf) <= 0) return 1;

    colon = strchr(b64buf, ':');
    if (!colon || (size_t)(colon - b64buf) >= sizeof user) return 1;
    snprintf(user, sizeof user, "%.*s", (int)(colon - b64buf), b64buf);
    snprintf(pass, sizeof pass, "%s", colon + 1);

    mutex_lock(g_lock);
    for (i = 0; i < g_lists.auth_n; i++) {
        if (strcmp(g_lists.auth[i].user, user) != 0) continue;
        if (strcmp(g_lists.auth[i].pass, pass) != 0) break;   /* 用户名对但密码错 → 仍需认证 */
        r = 0;
        {
            int k;
            for (k = 0; k < g_lists.auth[i].deny_n; k++)
                if (ci_equal(g_lists.auth[i].deny[k], host)) r = 2;
        }
        if (user_out && cap) snprintf(user_out, cap, "%s", g_lists.auth[i].user);
        break;
    }
    mutex_unlock(g_lock);
    return r;
}

int rules_auth_count(void) {
    int n;
    mutex_lock(g_lock);
    n = g_lists.auth_n;
    mutex_unlock(g_lock);
    return n;
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
