#pragma once

static inline int syscall0(int num) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "0"(num)
        : "memory");
    return ret;
}

static inline int syscall1(int num, int a1) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "0"(num), "b"(a1)
        : "memory");
    return ret;
}

static inline int syscall2(int num, int a1, int a2) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "0"(num), "b"(a1), "c"(a2)
        : "memory");
    return ret;
}

static inline int syscall3(int num, int a1, int a2, int a3) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "0"(num), "b"(a1), "c"(a2), "d"(a3)
        : "memory");
    return ret;
}

static inline int syscall4(int num, int a1, int a2, int a3, int a4) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "0"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4)
        : "memory");
    return ret;
}

long syscall(long num, ...);
