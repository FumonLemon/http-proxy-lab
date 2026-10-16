# 两个平台的构建方式见 README.md；MSVC 用一行的 cl 命令（见 README 与 CI）。
CC      ?= cc
CFLAGS  ?= -Wall -Wextra -Wpedantic -Wconversion -O2
SRC      = net.c http.c cache.c rules.c proxy.c

# Windows（MinGW）必须显式链接 ws2_32，否则一堆 undefined reference to __imp_socket；
# macOS / Linux 不需要。RUN 单独抽出来是因为 Windows 的 cmd 不认 './selftest'。
ifeq ($(OS),Windows_NT)
LDLIBS   = -lws2_32
RUN      = selftest.exe
EXE      = .exe
else
LDLIBS   =
RUN      = ./selftest
EXE      =
endif

proxy: $(SRC) platform.h net.h http.h cache.h rules.h
	$(CC) $(CFLAGS) -o proxy$(EXE) $(SRC) $(LDLIBS)

selftest: tests/selftest.c $(SRC) platform.h net.h http.h cache.h rules.h
	$(CC) $(CFLAGS) -DSELFTEST_BUILD -o selftest$(EXE) tests/selftest.c $(SRC) $(LDLIBS)

# 本机一条命令跑完自测（9 组断言，全过则退出码 0）
check: selftest
	$(RUN)

# 调试用：带 AddressSanitizer + UBSan 再跑一遍（不进 CI 的常规路径）
asan:
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -DSELFTEST_BUILD -o selftest_asan$(EXE) tests/selftest.c $(SRC) $(LDLIBS)
	./selftest_asan$(EXE)

clean:
	rm -f proxy proxy.exe selftest selftest.exe selftest_asan selftest_asan.exe *.o

.PHONY: check asan clean
