# 两个平台的构建方式见 README.md；MSVC 用一行的 cl 命令（见 README 与 CI）。
CC      ?= cc
CFLAGS  ?= -Wall -Wextra -Wpedantic -Wconversion -O2
SRC      = net.c http.c cache.c proxy.c

proxy: $(SRC) platform.h net.h http.h cache.h
	$(CC) $(CFLAGS) -o proxy $(SRC)

selftest: tests/selftest.c $(SRC) platform.h net.h http.h cache.h
	$(CC) $(CFLAGS) -DSELFTEST_BUILD -o selftest tests/selftest.c $(SRC)

# 本机一条命令跑完自测（8 条断言，全过则退出码 0）
check: selftest
	./selftest

# 调试用：带 AddressSanitizer + UBSan 再跑一遍（不进 CI 的常规路径）
asan:
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -DSELFTEST_BUILD -o selftest_asan tests/selftest.c $(SRC)
	./selftest_asan

clean:
	rm -f proxy selftest selftest_asan *.o

.PHONY: check asan clean
