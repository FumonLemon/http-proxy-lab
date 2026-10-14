# 计算机网络实验一：手搓 HTTP 代理服务器

[![ci](https://github.com/FumonLemon/http-proxy-lab/actions/workflows/ci.yml/badge.svg)](https://github.com/FumonLemon/http-proxy-lab/actions/workflows/ci.yml)

一个 C 程序：主线程在端口上 accept，每来一个连接开一个线程；线程把浏览器发来的**绝对形式**请求
解析成"主机 + 路径"，连原服务器、改写成 origin-form 转发出去，再把响应边收边转发；响应可缓存就顺手
存进内存表，下次命中先发 `If-Modified-Since` 问原服务器（304 就用缓存应答）。链路上还插了网站过滤、
用户过滤、网站引导三道判断。

设计与取舍的完整依据在同目录的 `research.md` / `plan.md`（vault：苹果3号/已处理/手搓代理服务器/）。

## 构建

macOS / MinGW（`make` 三个目标）：

```
make proxy        # 编译出代理可执行文件
make check        # 编译并运行自测（8 条断言，全过退出码 0）
make asan         # 带 AddressSanitizer + UBSan 再跑一遍自测（调试用，不进 CI）
make clean
```

MSVC（在仓库根目录、已进入 VS 开发者命令行）：

```
cl /W4 /std:c11 /utf-8 /Fe:proxy.exe net.c http.c cache.c proxy.c ws2_32.lib
cl /W4 /std:c11 /utf-8 /DSELFTEST_BUILD /Fe:selftest.exe tests\selftest.c net.c http.c cache.c proxy.c ws2_32.lib
selftest.exe
```

`/utf-8` 不能省：源码是 UTF-8，里面有中文注释、中文日志和自制页面，MSVC 默认按系统 ANSI 代码页读源文件，
中文串会被拆错字节（在中文 Windows 上尤其明显）。

不使用任何第三方库；Windows 侧只多链接系统的 `ws2_32.lib`。

## 跑法

```
./proxy 8080                                             # 端口默认 8080，可省略
curl -v -x http://127.0.0.1:8080 http://info.cern.ch/    # 命令行验证（浏览器也可以：设 http 代理为 127.0.0.1:8080）
```

日志走 stderr，一行一条请求，字段固定：

```
[2026-10-11 15:19:29] 127.0.0.1 GET http://info.cern.ch/ HTTP/1.1 -> info.cern.ch:80 200 878 缓存未命中
[2026-10-11 15:19:30] 127.0.0.1 GET http://info.cern.ch/ HTTP/1.1 -> info.cern.ch:80 304 878 命中+304
```

三个扩展功能的开关在 `proxy.c` 顶部（`ENABLE_SITE_FILTER` / `ENABLE_USER_FILTER` / `ENABLE_REDIRECT`），
编译时可用 `-DENABLE_SITE_FILTER=0` 关掉做对照；三张名单也在 `proxy.c` 顶部硬编码。

## 进度

- [x] A 骨架（platform.h + net + http + proxy + Makefile + 自测起步）
- [x] A2 仓库与 CI：https://github.com/FumonLemon/http-proxy-lab （Public），两个 job 全绿
      （macos-latest `make check`；windows-latest MSVC 编 `selftest.exe` 并跑通、另出 `proxy.exe`）
- [x] B 多线程 + 线程上限 + 大响应/慢响应/引导
- [x] C 缓存（If-Modified-Since + 304 + 淘汰）
- [x] D 三个扩展功能 + 开关对照
- [ ] E1/E2 宿舍 Windows 上人工验收（浏览器、系统代理、截图）——只有你能做
- [ ] E3/E4 报告与打包

自测当前状态：`make check` → `PASS 8/8`，`make asan` → `PASS 8/8`，
`-Wall -Wextra -Wpedantic -Wconversion` 零警告（含各开关组合）。

CI 实测记录（报告"实验环境"可直接引用）：

```
CI 编译器（windows-latest）：Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260 for x64
CI 命令：cl /W4 /std:c11 /utf-8 /DSELFTEST_BUILD /Fe:selftest.exe tests\selftest.c net.c http.c cache.c proxy.c ws2_32.lib
        （/W4 下零警告，selftest.exe 退出码 0，proxy.exe 产出成功）
本机编译器（macos-latest 同款）：Apple clang 16.0.0，make check 通过
```

## 实测踩到的四个真问题（写报告可以直接引用）

1. **一个地址被黑洞时会死等。** 首次实跑 `http://neverssl.com/`（本实验的经典纯 http 靶子）时，
   代理卡死、curl 15 秒拿不到任何字节。查明：该域名同时有 A 与 AAAA 记录，从本网络看
   **IPv4 无响应、IPv6 也时通时不通**；阻塞式 `connect()` 会一直挂到内核放弃。
   现在 `net_connect` 改成非阻塞 connect + `select` 超时（`net.c` 顶部 `CONNECT_TIMEOUT_MS`，
   默认 3 秒/地址），超时换下一个地址，全部失败才回 502。实测不可达主机现在 3 秒内返回 502。
2. **POST 的请求体可能和头部在同一次 `recv` 里到达。** 若只按 `Content-Length` 去 socket 上再读，
   代理会等一段已经收下的字节而挂死（自测 T1 一加 POST 断言就当场挂住）。现在先转发"跟着头部一起
   收到的那一截"，再读剩余的（`proxy.c` 第 10 步）。
3. **MSVC 默认按系统 ANSI 代码页读源码，中文会把引号吃掉。** 源码是 UTF-8 且含中文注释/中文页面，
   Windows 首次编译直接报语法错误（本地按 GBK 模拟解读，40 处引号被前一个汉字的尾巴字节配对吃掉）。
   修法：`cl` 加 `/utf-8`（同时定下源码与执行字符集）。
4. **MSVC 的 C 解析器不接受把调用约定宏塞进函数参数表。** `static inline int thread_start(thread_t *,
   THREAD_FN((*fn)), void *)` 在 MSVC 下报 `C2061/C2146/C2059`；改成
   `typedef unsigned (__stdcall *thread_fn_t)(void *)` 再写成普通参数即可，两平台共用。

前两个是平台无关的运行期 bug（自测与真实流量抓到），后两个只在 MSVC 上出现（CI 的 windows-latest 抓到）。

## 已知局限（都写进报告的"局限"）

- 不实现 `CONNECT`，所以 https 站点经本代理一律打不开（回 501）；验证只用纯 http 靶子。
- 转发时强制 `Connection: close`，换掉帧界定（读到 EOF = 一条响应读完），代价是没有连接复用。
- 只缓存 GET + 状态码 200 + 带 `Last-Modified` + 无 `no-store` 的响应；不做 `Vary`、不做 `max-age/Expires` 新鲜度计算。
- 缓存全表一把锁 + 线性扫描，上限 64 条 / 8 MB；只在内存里，重启即失。
- 用户过滤的判据是本机来源 IP，单机只能验证逻辑（自测里用注入值演示），多机验证未做。
