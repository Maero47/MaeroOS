/*
 * kwprobe — a user write to a present, writable KERNEL page must raise SIGSEGV.
 *
 * The page-fault handler once took any write-protection fault on a present,
 * writable, non-COW entry for a stale TLB entry and retried the instruction,
 * without checking the U/S bit: a user write into kernel memory then faulted
 * forever at 100% CPU instead of killing the process.  Each case runs in a
 * child; the parent checks it died of the expected signal (a regression hangs
 * in waitpid, which the smoke test's timeout reports).  Also checks that a
 * user int3 is a breakpoint (SIGTRAP), not a #GP (SIGSEGV).
 */
#include "../include/stdio.h"
#include "../include/unistd.h"
#include "../include/sys/wait.h"

#define SIGTRAP 5
#define SIGSEGV 11

static int expect_signal(const char *what, unsigned addr, int want) {
    int pid = fork();
    if (pid < 0) {
        printf("kwprobe: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        if (addr)
            *(volatile unsigned *)addr = 0;   /* supervisor page: must fault */
        else
            __asm__ volatile("int3");
        _exit(0);                             /* not reached if correct      */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        printf("kwprobe: waitpid failed for %s\n", what);
        return 1;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != want) {
        printf("kwprobe: %s: status 0x%x, want signal %d\n", what, status, want);
        return 1;
    }
    printf("kwprobe: %s -> signal %d\n", what, want);
    return 0;
}

int main(void) {
    int bad = 0;
    /* Direct map of low RAM (kernel .data/.bss/heap frames): present, writable,
     * supervisor-only. */
    bad |= expect_signal("write 0xC0100000", 0xC0100000U, SIGSEGV);
    bad |= expect_signal("write 0xC0400000", 0xC0400000U, SIGSEGV);
    /* The recursive page-directory window: the PD itself. */
    bad |= expect_signal("write 0xFFFFF000", 0xFFFFF000U, SIGSEGV);
    bad |= expect_signal("int3", 0, SIGTRAP);
    printf(bad ? "kwprobe FAILED\n" : "kwprobe ok\n");
    return bad;
}
