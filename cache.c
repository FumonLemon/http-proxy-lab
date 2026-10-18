/* cache.c —— 内存缓存表。
   键 = 浏览器发来的完整 URL；命中后拷贝一份给调用者（不返回表内指针，避免淘汰时被 free）。
   ponytail: 一把全局锁 + 线性扫描查找/淘汰，上限 = 几十条缓存；上量再换哈希分片。 */
#include "cache.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct entry {
    char  *url;
    char  *head;   size_t headlen;
    char  *body;   size_t bodylen;
    char   last_modified[64];
    time_t last_used;          /* LRU 依据 */
    size_t size;               /* headlen + bodylen */
    struct entry *next;
} entry_t;

static entry_t *g_head;                 /* 表头，最近插入的在前 */
static size_t   g_entries, g_bytes;
static mutex_t  g_lock;

/* ---- 磁盘镜像（plan.md §10）----
   内存表是索引，cache/ 目录里放对象本体（响应头 + 响应体原文）。
   为什么要有它：老师验收时要"删掉缓存文件 → 展示空文件夹 → 命中后文件夹里多出文件"，
   纯内存实现给不出这个动作。镜像不在 ⇒ 内存条目作废，所以"删文件"真的等于"清缓存"。
   写盘失败只降级（打一行警告），不影响转发与内存缓存。 */
static unsigned fnv1a32(const char *s) {
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* URL → 安全可读的文件名：cache/<可读名>-<8 位 hex>.bin
   可读名截 60 字符、非法字符换成下划线并压缩连续下划线；末尾 hash 兜住截断后的撞车。 */
int cache_mirror_path(const char *url, char *out, size_t cap) {
    char   name[80];
    size_t o = 0, i;
    const char *p = url;
    int    n;

    if (strncmp(p, "http://", 7) == 0) p += 7;          /* 去掉 scheme，文件名短一点 */
    for (i = 0; p[i] && o < 60 && o < sizeof name - 1; i++) {
        unsigned char c = (unsigned char)p[i];
        char ch;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-') ch = (char)c;
        else ch = '_';
        if (ch == '_' && o > 0 && name[o - 1] == '_') continue;
        name[o++] = ch;
    }
    name[o] = '\0';
    if (o == 0) { name[0] = 'x'; name[1] = '\0'; }

    n = snprintf(out, cap, "%s/%s-%08x.bin", CACHE_DIR, name, fnv1a32(url));
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* 写镜像：head + body 原样落盘，0 = 成功 */
static int mirror_write(const char *url, const char *head, size_t hl,
                        const char *body, size_t bl) {
    char  p[512];
    FILE *f;
    int   ok;

    if (cache_mirror_path(url, p, sizeof p) != 0) return -1;
    plat_mkdir(CACHE_DIR);                              /* 幂等：目录在不在都继续 */
    f = fopen(p, "wb");
    if (!f) return -1;
    ok = (hl == 0 || fwrite(head, 1, hl, f) == hl) &&
         (bl == 0 || fwrite(body, 1, bl, f) == bl);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static void mirror_remove(const char *url) {
    char p[512];
    if (cache_mirror_path(url, p, sizeof p) == 0) remove(p);
}

static int mirror_exists(const char *url) {
    char p[512];
    if (cache_mirror_path(url, p, sizeof p) != 0) return 0;
    return plat_file_mtime_ms(p) != 0;
}

static void *mem_dup(const void *p, size_t n) {
    void *q = malloc(n ? n : 1);
    if (q && n) memcpy(q, p, n);
    return q;
}

static char *str_dup(const char *s, size_t n) {
    char *q = malloc(n + 1);
    if (q) { memcpy(q, s, n); q[n] = '\0'; }
    return q;
}

static void entry_free(entry_t *e) {
    if (!e) return;
    free(e->url); free(e->head); free(e->body);
    free(e);
}

void cache_init(void) {
    MUTEX_INIT(g_lock);
    plat_mkdir(CACHE_DIR);          /* 镜像目录：不存在就建，已存在不算错 */
}

/* 淘汰：条目数或总字节超限时，反复丢掉"最久未使用"的那条（线性扫描） */
static void cache_evict_locked(void) {
    while (g_entries > CACHE_MAX_ENTRIES || g_bytes > CACHE_MAX_BYTES) {
        entry_t **q, **bestp = NULL, *best = NULL;
        for (q = &g_head; *q; q = &(*q)->next)
            if (!best || (*q)->last_used < best->last_used) { best = *q; bestp = q; }
        if (!best) break;
        *bestp = best->next;
        g_entries--; g_bytes -= best->size;
        mirror_remove(best->url);        /* 内存条目没了，磁盘镜像也删掉（先取 url，再 free） */
        entry_free(best);
    }
}

void cache_put(const char *url, const char *head, size_t headlen,
               const char *body, size_t bodylen, const char *last_modified) {
    entry_t *e, **pp;
    size_t total = headlen + bodylen;

    if (total > CACHEABLE_MAX) return;      /* 单条过大：只转发，不缓存（守卫在这里，所有调用者都covered） */
    e = calloc(1, sizeof *e);
    if (!e) return;                          /* 内存不够：功能降级，不影响转发 */
    e->url  = str_dup(url, strlen(url));
    e->head = mem_dup(head, headlen);
    e->body = mem_dup(body, bodylen);
    if (!e->url || !e->head || !e->body) { entry_free(e); return; }
    e->headlen = headlen;
    e->bodylen = bodylen;
    e->size = total;
    if (last_modified) {
        size_t n = strlen(last_modified);
        if (n >= sizeof e->last_modified) n = sizeof e->last_modified - 1;
        memcpy(e->last_modified, last_modified, n);
        e->last_modified[n] = '\0';
    }

    mutex_lock(g_lock);
    e->last_used = time(NULL);
    for (pp = &g_head; *pp; pp = &(*pp)->next) {          /* 同 URL 覆盖旧条目 */
        if (strcmp((*pp)->url, url) == 0) {
            entry_t *old = *pp;
            *pp = old->next;
            g_entries--; g_bytes -= old->size;
            mirror_remove(old->url);      /* 同 URL 覆盖：旧镜像一并删掉，稍后写新的 */
            entry_free(old);
            break;
        }
    }
    e->next = g_head;
    g_head = e;
    g_entries++; g_bytes += total;
    cache_evict_locked();
    mutex_unlock(g_lock);

    /* 镜像落盘放在锁外：写文件可能慢，不该占着全局锁。
       失败只降级（打一行警告），转发与内存缓存照旧——磁盘是证据副本，不是数据来源。 */
    if (mirror_write(url, head, headlen, body, bodylen) != 0)
        fprintf(stderr, "警告：缓存镜像写入失败（不影响转发）：%s\n", url);
}

int cache_fetch(const char *url, cache_obj_t *out) {
    entry_t *q, *prev = NULL;
    int hit = 0;

    memset(out, 0, sizeof *out);
    mutex_lock(g_lock);
    for (q = g_head; q; prev = q, q = q->next) {
        if (strcmp(q->url, url) != 0) continue;

        /* 镜像被删 ⇒ 条目作废：这样"删掉 cache/ 里的文件"就等于清缓存（plan §10.2）。
           代价是每次命中多一次 stat（微秒级），换来的是验收时"删文件"这个动作真实有效。 */
        if (!mirror_exists(url)) {
            if (prev) prev->next = q->next;
            else      g_head = q->next;
            g_entries--; g_bytes -= q->size;
            entry_free(q);
            break;                                  /* 当作未命中 */
        }

        out->head = mem_dup(q->head, q->headlen);
        out->body = mem_dup(q->body, q->bodylen);
        if (!out->head || !out->body) { cache_obj_free(out); break; }   /* 内存不够：当未命中 */
        out->headlen = q->headlen;
        out->bodylen = q->bodylen;
        memcpy(out->last_modified, q->last_modified, sizeof out->last_modified);
        q->last_used = time(NULL);
        hit = 1;
        break;
    }
    mutex_unlock(g_lock);
    return hit;
}

void cache_obj_free(cache_obj_t *o) {
    free(o->head);
    free(o->body);
    memset(o, 0, sizeof *o);
}

int cache_count(void) {
    int n;
    mutex_lock(g_lock); n = (int)g_entries; mutex_unlock(g_lock);
    return n;
}

size_t cache_bytes(void) {
    size_t n;
    mutex_lock(g_lock); n = g_bytes; mutex_unlock(g_lock);
    return n;
}
