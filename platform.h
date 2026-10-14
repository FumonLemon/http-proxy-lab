/* platform.h —— 唯一的平台差异层。
   业务代码（net.c / http.c / cache.c / proxy.c）里不出现任何 #ifdef，只用这里给出的名字。
   对照表见 plan.md 第 2.2 节；平台事实见 research.md 第 2 节。 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <time.h>
#include <string.h>

#ifdef _WIN32
  #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0601     /* Vista 以上：inet_ntop / GetTickCount64 需要 */
  #endif
  #include <winsock2.h>      /* 必须排在 windows.h 之前，否则撞 winsock1 */
  #include <ws2tcpip.h>      /* getaddrinfo / inet_ntop */
  #include <windows.h>       /* Sleep / CRITICAL_SECTION / GetTickCount64 */
  #include <process.h>       /* _beginthreadex */

  typedef SOCKET           socket_t;
  typedef int              sock_len_t;
  typedef HANDLE           thread_t;
  typedef CRITICAL_SECTION mutex_t;
  /* 线程函数指针类型：写成 typedef 而不是把调用约定宏塞进参数表（MSVC 的 C 解析器认不了后者） */
  typedef unsigned (__stdcall *thread_fn_t)(void *);

  #define SOCK_INVALID    INVALID_SOCKET
  #define sock_close(s)   closesocket(s)
  #define sock_errno()    WSAGetLastError()
  #define SOCK_WOULDBLOCK WSAEWOULDBLOCK
  #define SOCK_RESET      WSAECONNRESET
  #define SOCK_INTR       WSAEINTR
  #define strncasecmp     _strnicmp
  #define INET_NTOP(af, src, dst, cap) inet_ntop((af), (src), (dst), (size_t)(cap))
  #define SOCK_IO_LEN(n)  ((int)(n))       /* recv/send 的长度参数：Windows 要 int */
  #define SOCK_INPROGRESS WSAEWOULDBLOCK   /* 非阻塞 connect 的"进行中"在 Windows 上就是这个码 */
  #define SOCK_SELECT_NFDS(s) 0            /* Windows 的 select 忽略第一个参数 */
  #define MUTEX_INIT(m)   InitializeCriticalSection(&(m))
  #define mutex_lock(m)   EnterCriticalSection(&(m))
  #define mutex_unlock(m) LeaveCriticalSection(&(m))
  #define THREAD_FN(name) unsigned __stdcall name(void *arg)

  /* WSAStartup：Windows 上必须先初始化库，失败返回 -1（错误码用 sock_errno()） */
  static inline int plat_init(void) { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1; }
  static inline void plat_cleanup(void) { WSACleanup(); }
  static inline void sleep_ms(int ms) { Sleep((DWORD)ms); }
  static inline long long plat_now_ms(void) { return (long long)GetTickCount64(); }
  static inline void plat_localtime(struct tm *out, time_t t) { localtime_s(out, &t); }

  /* 非阻塞开关 + 等可写 + 读连接结果：只给 net_connect 的超时用 */
  static inline int sock_set_nonblock(socket_t s, int on) {
      u_long v = on ? 1u : 0u;
      return ioctlsocket(s, FIONBIO, &v);
  }
  static inline int sock_wait_writable(socket_t s, int ms) {
      fd_set w;
      struct timeval tv;
      FD_ZERO(&w); FD_SET(s, &w);
      tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
      return select(SOCK_SELECT_NFDS(s), NULL, &w, NULL, &tv) > 0 ? 0 : -1;
  }
  static inline int sock_so_error(socket_t s) {
      int err = 0, l = (int)sizeof err;
      if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &l) != 0) return -1;
      return err;                      /* 0 = 连接成功 */
  }

  static inline int thread_start(thread_t *t, thread_fn_t fn, void *arg) {
      *t = (HANDLE)_beginthreadex(NULL, 0, fn, arg, 0, NULL);   /* 失败返回 NULL */
      return *t ? 0 : -1;
  }
  static inline void thread_free(thread_t *t) { CloseHandle(*t); }   /* 句柄用完要关 */
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <pthread.h>
  #include <errno.h>
  #include <fcntl.h>      /* fcntl：非阻塞 connect 用 */

  typedef int             socket_t;
  typedef socklen_t       sock_len_t;
  typedef pthread_t       thread_t;
  typedef pthread_mutex_t mutex_t;
  typedef void *(*thread_fn_t)(void *);

  #define SOCK_INVALID    (-1)
  #define sock_close(s)   close(s)
  #define sock_errno()    (errno)
  #define SOCK_WOULDBLOCK EAGAIN
  #define SOCK_RESET      ECONNRESET
  #define SOCK_INTR       EINTR
  #define INET_NTOP(af, src, dst, cap) inet_ntop((af), (src), (dst), (socklen_t)(cap))
  #define SOCK_IO_LEN(n)  ((size_t)(n))    /* recv/send 的长度参数：POSIX 要 size_t */
  #define SOCK_INPROGRESS EINPROGRESS
  #define SOCK_SELECT_NFDS(s) ((int)(s) + 1)
  #define MUTEX_INIT(m)   pthread_mutex_init(&(m), NULL)
  #define mutex_lock(m)   pthread_mutex_lock(&(m))
  #define mutex_unlock(m) pthread_mutex_unlock(&(m))
  #define THREAD_FN(name) void *name(void *arg)

  static inline int  plat_init(void) { return 0; }
  static inline void plat_cleanup(void) {}

  static inline void sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000000L;
      nanosleep(&ts, NULL);
  }
  /* 单调毫秒计数：只用于自测里的耗时断言，不受系统时间跳变影响 */
  static inline long long plat_now_ms(void) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  }
  static inline void plat_localtime(struct tm *out, time_t t) { localtime_r(&t, out); }

  /* 非阻塞开关 + 等可写 + 读连接结果：只给 net_connect 的超时用 */
  static inline int sock_set_nonblock(socket_t s, int on) {
      int fl = fcntl(s, F_GETFL, 0);
      if (fl < 0) return -1;
      return fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
  }
  static inline int sock_wait_writable(socket_t s, int ms) {
      fd_set w;
      struct timeval tv;
      FD_ZERO(&w); FD_SET(s, &w);
      tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
      return select(SOCK_SELECT_NFDS(s), NULL, &w, NULL, &tv) > 0 ? 0 : -1;
  }
  static inline int sock_so_error(socket_t s) {
      int err = 0;
      socklen_t l = (socklen_t)sizeof err;
      if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l) != 0) return -1;
      return err;                      /* 0 = 连接成功 */
  }

  static inline int thread_start(thread_t *t, thread_fn_t fn, void *arg) {
      return pthread_create(t, NULL, fn, arg);
  }
  /* 不 join 也不留僵尸：detach 掉，线程退出时资源自动回收 */
  static inline void thread_free(thread_t *t) { pthread_detach(*t); }
#endif

#endif /* PLATFORM_H */
