/* cache.c —— 内存缓存表。
   键 = 浏览器发来的完整 URL；命中后拷贝一份给调用者（不返回表内指针，避免淘汰时被 free）。
   ponytail: 一把全局锁 + 线性扫描查找/淘汰，上限 = 几十条缓存；上量再换哈希分片。 */
#include "cache.h"
#include "platform.h"
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

void cache_init(void) { MUTEX_INIT(g_lock); }

/* 淘汰：条目数或总字节超限时，反复丢掉"最久未使用"的那条（线性扫描） */
static void cache_evict_locked(void) {
    while (g_entries > CACHE_MAX_ENTRIES || g_bytes > CACHE_MAX_BYTES) {
        entry_t **q, **bestp = NULL, *best = NULL;
        for (q = &g_head; *q; q = &(*q)->next)
            if (!best || (*q)->last_used < best->last_used) { best = *q; bestp = q; }
        if (!best) break;
        *bestp = best->next;
        g_entries--; g_bytes -= best->size;
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
            entry_free(old);
            break;
        }
    }
    e->next = g_head;
    g_head = e;
    g_entries++; g_bytes += total;
    cache_evict_locked();
    mutex_unlock(g_lock);
}

int cache_fetch(const char *url, cache_obj_t *out) {
    entry_t *q;
    int hit = 0;

    memset(out, 0, sizeof *out);
    mutex_lock(g_lock);
    for (q = g_head; q; q = q->next) {
        if (strcmp(q->url, url) != 0) continue;
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
