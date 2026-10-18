# 计算机网络实验一：手搓 HTTP 代理服务器

[![ci](https://github.com/FumonLemon/http-proxy-lab/actions/workflows/ci.yml/badge.svg)](https://github.com/FumonLemon/http-proxy-lab/actions/workflows/ci.yml)

一个 C 程序：主线程在端口上 accept，每来一个连接开一个线程；线程把浏览器发来的**绝对形式**请求
解析成"主机 + 路径"，连原服务器、改写成 origin-form 转发出去，再把响应边收边转发；响应可缓存就顺手
存进内存表，下次命中先发 `If-Modified-Since` 问原服务器（304 就用缓存应答）。链路上还插了网站过滤、
用户过滤、网站引导三道判断。

设计与取舍的完整依据在 vault 的 `已处理/手搓代理服务器/`：`research.md`（研究）与 `plan.md`（计划 + 实施记录）。

## 构建

macOS / MinGW（`make` 三个目标；Windows 侧 Makefile 会自动补上 `-lws2_32`）：

```
make proxy        # 编译出代理可执行文件（Windows 上产出 proxy.exe）
make check        # 编译并运行自测（10 组 / 54 条断言，全过退出码 0）
make asan         # 带 AddressSanitizer + UBSan 再跑一遍自测（调试用，不进 CI）
make clean
```

MSVC（在仓库根目录、已进入 VS 开发者命令行）：

```
cl /W4 /std:c11 /utf-8 /Fe:proxy.exe net.c http.c cache.c rules.c proxy.c ws2_32.lib
cl /W4 /std:c11 /utf-8 /DSELFTEST_BUILD /Fe:selftest.exe tests\selftest.c net.c http.c cache.c rules.c proxy.c ws2_32.lib
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

三个扩展功能的**编译期开关**在 `proxy.c` 顶部（`ENABLE_SITE_FILTER` / `ENABLE_USER_FILTER` / `ENABLE_REDIRECT`），
编译时可用 `-DENABLE_SITE_FILTER=0` 关掉做对照；三张**名单本身**不在这里——见下一节。

## 缓存落盘与验收（cache/）

内存表只是索引，对象本体镜像到工作目录下的 `cache/`：文件名 = URL 可读名 + 8 位 hash（如
`127.0.0.1_9000_a.txt-8e1c84f1.bin`），内容是**响应头 + 响应体原文**（`head -5` 就能看到 `HTTP/1.1 200 OK`）。

**删掉镜像文件就等于清缓存**：每次查缓存前会 `stat` 一下镜像，文件不在就把内存条目一并作废。
课程验收的四步因此都能真演出来（用外网原服务器，不需要在本机起任何服务）：

```bash
# 任选一个外网纯 http 靶子（实测都能命中 304）
U=http://mirrors.tuna.tsinghua.edu.cn/debian/README

rm -f cache/*.bin && ls cache/            # ① 空文件夹
curl -x http://127.0.0.1:8080 $U          # ② 日志"缓存未命中"，cache/ 多出 xxx.bin
head -5 cache/*.bin                       #    镜像里就是响应报文原文
curl -x http://127.0.0.1:8080 $U          # ③ 日志"命中+304"
rm -f cache/*.bin && curl -x http://127.0.0.1:8080 $U      # ④ 又变回"缓存未命中"
```

**哪些站能命中**（2026-10-18 实测，`200 未命中 → 304 命中+304`）：

| 靶子 | 大小 | 首访耗时 |
| --- | --- | --- |
| `http://mirrors.tuna.tsinghua.edu.cn/debian/README` | 1198 B | ~3.1 s |
| `http://mirrors.ustc.edu.cn/debian/README` | 1198 B | ~3.1 s |
| `http://mirror.nju.edu.cn/debian/README` | 1198 B | ~3.1 s |
| `http://info.cern.ch/` | 646 B | ~3.9 s |

能命中的条件：① 纯 `http`（https 走 CONNECT，本代理回 501）；② 响应带 `Last-Modified`；③ 没有被
`Cache-Control: no-store` 禁掉；④ 单条 ≤ 4 MB。首页通常没有 `Last-Modified`（如 `www.gnu.org`），
所以优先挑**静态文件**（`README`、`.txt`、小图片）。

## 名单与热加载（rules.txt）

三张名单是运行时可改的文本文件（`rules.txt`；命令行第二个参数可指定别的路径，例如
`./proxy.exe 8080 rules.txt`；文件不存在就用内置默认名单，行为与硬编码时代一致）：

```text
[site]                          # 网站过滤：命中 → 403，代理不连原服务器
127.0.0.1

[user]                          # 用户过滤：来源 IP 命中 → 403
192.0.2.1

[redirect]                      # 网站引导：命中主机 → 目标两种写法
guide.test    self:fake.html    #   ① self:文件 → 代理把该文件当页面直接回给你，地址栏仍是原域名
old.test      http://example.com/   #   ② 网址 → 302 跳过去（地址栏会变成这个网址）
```

规则：`#` 是注释、`[小节]` 切名单、空行忽略；主机名匹配**不分大小写**，只比主机名
（`http://127.0.0.1:9999/x` 命中 `127.0.0.1`，路径不参与匹配）；只对 `http://` 绝对形式请求有意义
（https 走 `CONNECT`，在解析阶段就回 501）。`self:` 后面的路径相对于代理的工作目录，每次请求现读，
所以**改那个页面文件、刷新浏览器就生效**（实验里的"引导到模拟网站/钓鱼"要的就是第 ① 种：地址栏不变）。

**存盘即生效**：代理每处理一个请求前只 `stat` 一次名单文件的 mtime（毫秒级），变了才重读。
所以改名单**不用重启、不用重新编译**；文件被删或读不动时保留上一次成功的名单。

## 进度

- [x] A 骨架（platform.h + net + http + proxy + Makefile + 自测起步）
- [x] A2 仓库与 CI：https://github.com/FumonLemon/http-proxy-lab （Public），两个 job 全绿
      （macos-latest `make check`；windows-latest MSVC 编 `selftest.exe` 并跑通、另出 `proxy.exe`）
- [x] B 多线程 + 线程上限 + 大响应/慢响应/引导
- [x] C 缓存（If-Modified-Since + 304 + 淘汰）
- [x] D 三个扩展功能 + 开关对照
- [x] D2 名单热加载：`rules.txt` + mtime 轮询（存盘即生效）；网站引导改成真 302 跳转
- [x] D3 缓存落盘镜像：`cache/` 里留对象副本，删镜像文件即失效（对齐课程验收动作）
- [ ] E1/E2 宿舍 Windows 上人工验收（浏览器、系统代理、截图）——只有你能做
- [ ] E3/E4 报告与打包

自测当前状态：`make check` → `PASS 10/10`（54 条断言），`make asan` → `PASS 10/10`，
本机 MinGW gcc 15.2.0 下 `-Wall -Wextra -Wpedantic -Wconversion` 有两条已存在的提示
（`platform.h` 的 `FIONBIO` 符号转换、`proxy.c:203` 的 `%s` 可能截断，`snprintf` 本身安全），
CI 的 Apple clang 16 / MSVC 19.51 侧是零警告。

CI 实测记录（报告"实验环境"可直接引用）：

```
CI 编译器（windows-latest）：Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260 for x64
CI 命令：cl /W4 /std:c11 /utf-8 /DSELFTEST_BUILD /Fe:selftest.exe tests\selftest.c net.c http.c cache.c proxy.c ws2_32.lib
        （/W4 下零警告，selftest.exe 退出码 0，proxy.exe 产出成功）
本机编译器（macos-latest 同款）：Apple clang 16.0.0，make check 通过
```

## 实测踩到的六个真问题（写报告可以直接引用）

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
5. **拒绝类响应会被 RST 吞掉：带未读数据 `close` 是个陷阱。** 用户过滤的 403 是在"请求一个字节都
   没读"时就回的，随后直接 `closesocket` 会让内核发 RST，客户端**连那条 403 都收不到**
   （实测客户端 0 字节 + `WinError 10053`；同一份代码在"先读走请求再回"的网站过滤路径上就没有问题）。
   修法：收尾时先 `shutdown(SD_SEND)` 发 FIN，再把对端还没读走的数据带超时地读掉（`net_close_gracefully`，
   总排空上限 256 KB），之后才关；`handle_conn` 的收尾点与并发上限 503 那条路径共用这一个函数。
6. **POSIX 的"亚秒 mtime"字段名不统一：macOS 不是 `st_mtim`。** 名单热加载要毫秒级 mtime
   （不然同一秒内改两次会漏掉），Linux 写 `st.st_mtim.tv_nsec` 能编，到 macOS 上直接
   `error: no member named 'st_mtim' in 'struct stat'`——BSD 那边叫 `st_mtimespec`。
   修法：`#if defined(__APPLE__)` 分支取名，其余走 `st_mtim`；Windows 本来就用
   `GetFileAttributesExA`，不受影响。这条是 CI 的 macos-latest 抓的，本机 MinGW 编不出来。

前两个、第五个是平台无关的运行期 bug（自测与真实流量抓到），第三、第四、第六个是平台/编译器差异（CI 的两个 job 抓到）。

## 已知局限（都写进报告的"局限"）

- 不实现 `CONNECT`，所以 https 站点经本代理一律打不开（回 501）；验证只用纯 http 靶子。
- 转发时强制 `Connection: close`，换掉帧界定（读到 EOF = 一条响应读完），代价是没有连接复用。
- 只缓存 GET + 状态码 200 + 带 `Last-Modified` + 无 `no-store` 的响应；不做 `Vary`、不做 `max-age/Expires` 新鲜度计算。
- 缓存全表一把锁 + 线性扫描，上限 64 条 / 8 MB；内存是索引、对象镜像在 `cache/`（删镜像即失效），重启后索引清空。
- 用户过滤的判据是本机来源 IP，单机只能验证逻辑（自测里用注入值演示），多机验证未做。
- 名单只按**主机名**精确匹配（不分大小写）：不支持路径级过滤、通配、正则，也不按解析出的 IP 复核；
  名单文件没有权限保护，本机任何进程都能改（控制面与数据面没隔离）。
- 名单热加载靠文件 mtime（毫秒级），所以别在同一毫秒里连改两次；文件被删时保留上一次成功的名单。
