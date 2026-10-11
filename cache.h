/* cache.h —— 内存缓存表（实验内容第 2 条：Cache 功能）。
   上限与淘汰策略见 plan.md 第 3.4 节与决策三。 */
#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

#define CACHE_MAX_ENTRIES 64                    /* 条目数上限 */
#define CACHE_MAX_BYTES   (8u * 1024u * 1024u)  /* 总字节上限 8 MB */
#define CACHEABLE_MAX     (4u * 1024u * 1024u)  /* 单条超过它不进缓存，只转发 */

typedef struct {
    char  *head;      /* 响应头原文（含空行），原样回放 */
    size_t headlen;
    char  *body;      /* 响应体原文 */
    size_t bodylen;
    char   last_modified[64];   /* 用于下次的 If-Modified-Since */
} cache_obj_t;

/* 必须在任何其它 cache_* 之前调用一次（初始化互斥锁） */
void cache_init(void);

/* 查表：1 = 命中且 out 已填好拷贝（调用者用 cache_obj_free 释放），0 = 未命中 */
int  cache_fetch(const char *url, cache_obj_t *out);

/* 入表：长度超过 CACHEABLE_MAX 直接不存；同 URL 覆盖旧条目；存完做一次淘汰 */
void cache_put(const char *url, const char *head, size_t headlen,
               const char *body, size_t bodylen, const char *last_modified);

void cache_obj_free(cache_obj_t *o);

/* 自测用（C1 的"算过"要读这两个数） */
int    cache_count(void);
size_t cache_bytes(void);

#endif /* CACHE_H */
