/* rules.h —— 三张名单（网站过滤 / 用户过滤 / 网站引导）从编译期常量改成运行时可重载的文本文件。
   调用方（proxy.c）只看这七个函数，不关心内部结构；格式与规则见 plan.md 第 1 节。 */
#ifndef RULES_H
#define RULES_H

#include <stddef.h>

/* 启动时调一次：path 为 NULL 或文件读不到 → 用内置默认名单（等价于改造前的行为） */
void rules_init(const char *path);

/* 每个请求前调一次：文件 mtime 变了才真重读；文件被删/读不动则保留上一次成功的名单 */
void rules_reload_if_changed(void);

/* 给启动日志用的一行说明（名单来源 + 条数） */
const char *rules_source(void);

/* 网站过滤：主机名是否在名单内（大小写不敏感） */
int rules_site_blocked(const char *host);

/* 用户过滤：来源 IP 是否在名单内 */
int rules_user_blocked(const char *ip);

/* 网站引导：命中返回 1 并把目标网址拷进 out（没命中返回 0）。
   写成拷进调用方缓冲而不是返回内部指针：名单可能被另一个线程重载，返回的指针会悬空。 */
int rules_redirect_target(const char *host, char *out, size_t cap);

/* 代理认证（[auth] 段，按账号区分用户）。校验请求头 Proxy-Authorization: Basic base64(user:pass)。
   返回 0 = 放行（user_out 填上用户名）；1 = 需要认证（没凭据或凭据不对，调用方回 407）；
        2 = 该用户被禁止访问 host（调用方回 403）。
   没有 [auth] 段时恒返回 0，等于不启用认证——老行为完全不变。 */
int rules_auth_check(const char *head, size_t len, const char *host,
                     char *user_out, size_t cap);

/* 配了多少个认证用户（0 = 未启用认证） */
int rules_auth_count(void);

/* 当前名单条数（自测与启动日志用） */
void rules_counts(int *site, int *user, int *redir);

#endif /* RULES_H */
