/*
 * memprobe — the kernel must touch user memory only through the fault-safe
 * copies.  Every case below hands the kernel a user pointer that passes a
 * range check but cannot take the access (a read-only page, an unmapped page,
 * a kernel address), and expects what Linux does: -EFAULT from a syscall, or
 * SIGSEGV for the process when the pointer is a signal stack.  Before the
 * fixes each of these was a ring-0 page fault outside __ex_table, i.e. a
 * kernel panic — or, for the kernel-address signal stack, a signal frame
 * written into kernel memory.
 *
 * Prints "memprobe: <case> ok" per case and "memprobe ok" at the end;
 * tools/smoke.py waits for the latter.
 */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/syscall.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"
#include "../include/signal.h"
#include "../include/sys/mman.h"
#include "../include/sys/wait.h"

#define NR_READ         3
#define NR_KILL         37
#define NR_MUNMAP       91
#define NR_MMAP2        192
#define NR_FCNTL        55
#define NR_IOCTL        54
#define NR_SIGRETURN    119
#define NR_CLONE        120
#define NR_MPROTECT     125
#define NR_RT_SIGACTION 174
#define NR_SIGALTSTACK  186
#define NR_SET_TID_ADDR 258

#define EFAULT          14
#define SA_ONSTACK      0x08000000
#define CLONE_PARENT_SETTID 0x00100000
#define TCGETS          0x5401
#define KERNEL_ADDR     0xC0100000u

static int failures;

/* The in-tree libc's mmap() is malloc(); these need real page mappings. */
static void *map_anon(unsigned len) {
    int ret;
    __asm__ volatile(
        "push %%ebp\n"
        "mov $0, %%ebp\n"                  /* pgoffset */
        "int $0x80\n"
        "pop %%ebp\n"
        : "=a"(ret)
        : "a"(NR_MMAP2), "b"(0), "c"(len), "d"(PROT_READ | PROT_WRITE),
          "S"(MAP_PRIVATE | MAP_ANONYMOUS), "D"(-1)
        : "memory");
    if (ret < 0 && ret > -4096) { printf("memprobe: mmap2 failed (%d)\n", ret); exit(1); }
    return (void *)ret;
}

static void unmap(void *p, unsigned len) { syscall2(NR_MUNMAP, (int)p, (int)len); }

static void check(const char *name, int ok, int got) {
    if (ok) printf("memprobe: %s ok\n", name);
    else  { printf("memprobe: %s FAILED (got %d)\n", name, got); failures++; }
}

/* A fresh page mapped PROT_READ; `touch` faults it in first, so both the
 * "not yet populated" and the "present but read-only" cases get exercised. */
static void *ro_page(int touch) {
    void *p = map_anon(4096);
    if (touch) ((volatile char *)p)[0] = 1;
    if (syscall3(NR_MPROTECT, (int)p, 4096, PROT_READ) != 0) {
        printf("memprobe: mprotect failed\n"); exit(1);
    }
    return p;
}

/* A page-sized hole: mapped, then unmapped again. */
static void *hole_page(void) {
    void *p = map_anon(4096);
    unmap(p, 4096);
    return p;
}

static void on_usr1(int sig) { (void)sig; }

static int install(int sig, void (*h)(int), unsigned flags) {
    unsigned kact[8];
    memset(kact, 0, sizeof(kact));
    kact[0] = (unsigned)h;
    kact[1] = flags;
    return syscall4(NR_RT_SIGACTION, sig, (int)kact, 0, 8);
}

/* Run fn in a child and report how the child ended: the exit code, or
 * -(signal) if it was killed. */
static int in_child(void (*fn)(void)) {
    int pid = fork();
    if (pid == 0) { fn(); _exit(0); }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return 999;
    return WIFSIGNALED(st) ? -WTERMSIG(st) : WEXITSTATUS(st);
}

/* kill(self, SIGUSR1) issued with esp = `sp`: the handler's frame has to be
 * built there.  If the kernel did not kill us, put esp back and exit 0. */
static void raise_with_sp(unsigned sp) {
    int pid = getpid();
    int ret;
    __asm__ volatile(
        "mov %%esp, %%edi\n"
        "mov %[sp], %%esp\n"
        "int $0x80\n"
        "mov %%edi, %%esp\n"
        : "=a"(ret)
        : "a"(NR_KILL), "b"(pid), "c"(SIGUSR1), [sp] "S"(sp)
        : "edi", "memory");
    (void)ret;
}

static void child_kernel_sp(void) {
    install(SIGUSR1, on_usr1, 0);
    raise_with_sp(KERNEL_ADDR);
}

static void child_ro_sp(void) {
    char *ro = ro_page(1);
    install(SIGUSR1, on_usr1, 0);
    raise_with_sp((unsigned)ro + 4096);
}

static void child_unmapped_altstack(void) {
    char *stk = map_anon(16384);
    unsigned ss[3] = { (unsigned)stk, 0, 16384 };
    if (syscall2(NR_SIGALTSTACK, (int)ss, 0) != 0) _exit(2);
    unmap(stk, 16384);                     /* the alternate stack is now a hole */
    install(SIGUSR1, on_usr1, SA_ONSTACK);
    kill(getpid(), SIGUSR1);
}

static void child_bad_sigreturn(void) {
    int ret;
    char *hole = hole_page();
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(NR_SIGRETURN), "c"(hole), "d"(0)
                     : "memory");
    (void)ret;
}

static void child_ro_clear_tid(void) {
    char *ro = ro_page(1);
    syscall1(NR_SET_TID_ADDR, (int)ro);   /* zeroed + futex-woken at exit */
}

int main(void) {
    char *ro_fresh = ro_page(0), *ro_touched = ro_page(1);
    int r;

    /* (a) fcntl(F_GETLK) writes l_type back into the caller's struct flock. */
    r = syscall3(NR_FCNTL, 0, F_GETLK, (int)ro_touched);
    check("fcntl F_GETLK read-only", r == -EFAULT, r);
    r = syscall3(NR_FCNTL, 0, F_GETLK, (int)ro_fresh);
    check("fcntl F_GETLK unpopulated read-only", r == -EFAULT, r);
    r = syscall3(NR_FCNTL, 0, F_GETLK, (int)KERNEL_ADDR);
    check("fcntl F_GETLK kernel address", r == -EFAULT, r);

    /* (b) signal frames that cannot be written: SIGSEGV, never a panic. */
    r = in_child(child_kernel_sp);
    check("signal frame at kernel esp", r == -SIGSEGV, r);
    r = in_child(child_ro_sp);
    check("signal frame on read-only stack", r == -SIGSEGV, r);
    r = in_child(child_unmapped_altstack);
    check("signal frame on unmapped sigaltstack", r == -SIGSEGV, r);
    r = in_child(child_bad_sigreturn);
    check("sigreturn from unmapped frame", r == -SIGSEGV, r);

    /* ioctl: the argument is bounced, a bad one is -EFAULT. */
    int tty = open("/dev/tty", O_RDWR);
    if (tty >= 0) {
        r = syscall3(NR_IOCTL, tty, TCGETS, (int)ro_touched);
        check("ioctl TCGETS read-only", r == -EFAULT, r);
        close(tty);
    }

    /* read(): a pipe keeps its data when the buffer is bad; a file read into
     * a read-only buffer is refused before the VFS memcpy()s into it. */
    int p[2];
    if (pipe(p) == 0) {
        char buf[4] = { 0 };
        write(p[1], "xyz", 3);
        r = syscall3(NR_READ, p[0], (int)ro_touched, 3);
        int again = read(p[0], buf, 3);
        check("pipe read into read-only", r == -EFAULT && again == 3 &&
              memcmp(buf, "xyz", 3) == 0, r);
        close(p[0]); close(p[1]);
    }
    int fd = open("/hello.txt", O_RDONLY);
    if (fd >= 0) {
        r = syscall3(NR_READ, fd, (int)ro_touched, 16);
        check("file read into read-only", r == -EFAULT, r);
        close(fd);
    }

    /* clone(CLONE_PARENT_SETTID) with ptid in a read-only page: the tid
     * store is skipped, the clone itself still succeeds. */
    r = syscall3(NR_CLONE, CLONE_PARENT_SETTID | SIGCHLD, 0, (int)ro_touched);
    if (r == 0) _exit(0);
    if (r > 0) { int st; waitpid(r, &st, 0); }
    check("clone PARENT_SETTID read-only", r > 0, r);

    /* A clear_child_tid in a read-only page is stored at exit. */
    r = in_child(child_ro_clear_tid);
    check("exit with read-only clear_child_tid", r == 0, r);

    if (failures) { printf("memprobe: %d failures\n", failures); return 1; }
    printf("memprobe ok\n");
    return 0;
}
