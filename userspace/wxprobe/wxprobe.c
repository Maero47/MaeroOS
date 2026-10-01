/*
 * wxprobe — user pages carry the protection they were given.  The ELF loader
 * maps each PT_LOAD segment with its own p_flags, so .text and .rodata are
 * read-only; mprotect() changes what a page allows, in both directions; and a
 * forked child that writes a read-only page gets SIGSEGV instead of a private
 * copy made by the copy-on-write handler.  Every refused write must arrive as
 * SIGSEGV with si_code SEGV_ACCERR and si_addr the address written, as Linux
 * delivers it — not as a fault loop, a kernel panic or a silent success.
 *
 * Execute permission (PAE + NX, "nx" in /proc/cpuinfo): code placed on the
 * stack, the brk heap or an anonymous PROT_READ|PROT_WRITE mapping does not
 * run (SIGSEGV, SEGV_ACCERR, si_addr the instruction); mprotect(PROT_EXEC)
 * makes it run, and a JIT-style buffer flipped RW → RX → RW runs only while
 * it is RX.  Without NX (legacy paging) these checks are skipped: the hardware
 * cannot refuse execution.
 *
 * Prints "wxprobe: <case> ok" per case and "wxprobe ok" at the end;
 * tools/smoke.py waits for the latter.
 */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/syscall.h"
#include "../include/unistd.h"
#include "../include/signal.h"
#include "../include/sys/mman.h"
#include "../include/sys/wait.h"
#include "../include/fcntl.h"

#define NR_READ         3
#define NR_MMAP2        192
#define NR_MPROTECT     125

#define SEGV_ACCERR     2
#define EFAULT          14
#define PAGE            4096u

static int failures;

static const char ro_msg[] = "wxprobe read-only data";
static volatile int rw_word = 1;

static void check(const char *name, int ok, int got) {
    if (ok) printf("wxprobe: %s ok\n", name);
    else  { printf("wxprobe: %s FAILED (got %d)\n", name, got); failures++; }
}

static unsigned page_of(const volatile void *p) { return (unsigned)p & ~(PAGE - 1); }

/* The in-tree libc's mmap() is malloc(); this needs a real page mapping. */
static volatile char *map_anon(void) {
    int ret;
    __asm__ volatile(
        "push %%ebp\n"
        "mov $0, %%ebp\n"
        "int $0x80\n"
        "pop %%ebp\n"
        : "=a"(ret)
        : "a"(NR_MMAP2), "b"(0), "c"(PAGE), "d"(PROT_READ | PROT_WRITE),
          "S"(MAP_PRIVATE | MAP_ANONYMOUS), "D"(-1)
        : "memory");
    if (ret < 0 && ret > -4096) { printf("wxprobe: mmap2 failed (%d)\n", ret); exit(1); }
    return (volatile char *)ret;
}

/* A page of the brk heap (not a VMA: protection lives in the PTE alone). */
static volatile char *brk_page(void) {
    char *p = (char *)sbrk(3 * (int)PAGE);
    if (p == (char *)-1) { printf("wxprobe: sbrk failed\n"); exit(1); }
    return (volatile char *)((((unsigned)p) + PAGE - 1) & ~(PAGE - 1));
}

static int protect(unsigned page, int prot) {
    return syscall3(NR_MPROTECT, (int)page, (int)PAGE, prot);
}

/* ── In-process faults ───────────────────────────────────────────────────────
 * The handler checks why the write was refused, then makes the page writable
 * and returns: the write is restarted and must now succeed.  A kernel that
 * refused the write without a signal would loop; one that let it through
 * would never call the handler. */
static volatile unsigned fault_want;   /* address the write targets */
static volatile unsigned fault_page;   /* page to make writable in the handler */
static volatile int      fault_restore;/* prot the handler grants */
static volatile int      fault_hits;
static volatile int      fault_code;
static volatile unsigned fault_addr;

static void on_segv(int sig, void *info, void *uc) {
    (void)sig; (void)uc;
    fault_hits++;
    fault_code = ((int *)info)[2];              /* si_code */
    fault_addr = ((unsigned *)info)[3];         /* si_addr */
    if (fault_hits > 3 || protect(fault_page, fault_restore) != 0)
        _exit(99);                              /* would loop: give up */
}

static int install_segv(void (*h)(int, void *, void *), unsigned flags) {
    struct sigaction sa = { 0 };
    sa.sa_sigaction = (void (*)(int, siginfo_t *, void *))h;
    sa.sa_flags = (int)flags;
    return sigaction(SIGSEGV, &sa, 0);
}

/* Write `val` to `p`, expecting exactly one SEGV_ACCERR at p, after which the
 * handler grants `restore` and the write lands. */
static void fault_then_fix(const char *name, volatile char *p, char val, int restore) {
    fault_want = (unsigned)p;
    fault_page = page_of(p);
    fault_restore = restore;
    fault_hits = 0; fault_code = 0; fault_addr = 0;
    *p = val;
    int ok = fault_hits == 1 && fault_code == SEGV_ACCERR &&
             fault_addr == fault_want && *p == val;
    if (!ok)
        printf("wxprobe: %s: hits=%d code=%d addr=%08x want=%08x\n", name,
               fault_hits, fault_code, fault_addr, fault_want);
    check(name, ok, fault_hits);
}

/* ── Faults in a forked child ───────────────────────────────────────────────
 * Run fn in a child with SIGSEGV at its default action and report how it
 * ended: the exit code, or -(signal). */
static volatile char *child_target;

static int in_child(void (*fn)(void)) {
    int pid = fork();
    if (pid == 0) {
        install_segv(0, 0);                     /* SIG_DFL */
        fn();
        _exit(0);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return 999;
    return WIFSIGNALED(st) ? -WTERMSIG(st) : WEXITSTATUS(st);
}

static void child_write(void) { *child_target = 'C'; }

/* ── Execute permission ──────────────────────────────────────────────────── */
static int nx_active(void) {
    char buf[512];
    int fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd < 0) return 0;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *f = strstr(buf, "flags");
    return f && (strstr(f, " nx\n") || strstr(f, " nx "));
}

/* `mov eax, 42; ret` at p. */
static void put_code(volatile char *p) {
    static const unsigned char code[] = { 0xB8, 42, 0, 0, 0, 0xC3 };
    for (unsigned i = 0; i < sizeof(code); i++) p[i] = (char)code[i];
}
static int run_code(volatile char *p) { return ((int (*)(void))(unsigned)p)(); }

/* Call code at p, expecting one SEGV_ACCERR at p; the handler then grants
 * `restore` (which includes PROT_EXEC) and the call runs. */
static void exec_then_fix(const char *name, volatile char *p, int restore) {
    put_code(p);
    fault_want = (unsigned)p;
    fault_page = page_of(p);
    fault_restore = restore;
    fault_hits = 0; fault_code = 0; fault_addr = 0;
    int v = run_code(p);
    int ok = fault_hits == 1 && fault_code == SEGV_ACCERR &&
             fault_addr == fault_want && v == 42;
    if (!ok)
        printf("wxprobe: %s: hits=%d code=%d addr=%08x want=%08x ret=%d\n", name,
               fault_hits, fault_code, fault_addr, fault_want, v);
    check(name, ok, fault_hits);
}

static volatile char *child_code;
static void child_exec(void) { put_code(child_code); run_code(child_code); }
static void child_exec_stack(void) {
    volatile char buf[16];
    put_code(buf);
    run_code(buf);
}

static void nx_checks(void) {
    if (!nx_active()) {
        printf("wxprobe: no NX on this CPU/kernel, exec checks skipped\n");
        return;
    }
    const int rwx = PROT_READ | PROT_WRITE | PROT_EXEC;

    /* Stack: a child dies running its own frame's buffer; then here, the
     * same fault is fixed by making the page executable (after the child,
     * whose frame may share the page). */
    int r = in_child(child_exec_stack);
    check("fork child stack exec SIGSEGV", r == -SIGSEGV, r);
    volatile char stack_code[64];
    exec_then_fix("stack exec faults", stack_code, rwx);

    /* brk heap */
    volatile char *heap = brk_page();
    exec_then_fix("heap exec faults", heap, rwx);

    /* Anonymous PROT_READ|PROT_WRITE mapping */
    volatile char *anon = map_anon();
    exec_then_fix("anon RW exec faults", anon, rwx);

    /* A JIT buffer: written RW, flipped RX (runs, no fault, not writable),
     * flipped back RW (no longer runs). */
    volatile char *jit = map_anon();
    put_code(jit);
    check("jit mprotect RX", protect(page_of(jit), PROT_READ | PROT_EXEC) == 0, 0);
    fault_hits = 0;
    int v = run_code(jit);
    check("jit RX runs", v == 42 && fault_hits == 0, v);
    child_target = jit;
    r = in_child(child_write);
    check("jit RX write SIGSEGV", r == -SIGSEGV, r);
    check("jit mprotect RW", protect(page_of(jit), PROT_READ | PROT_WRITE) == 0, 0);
    child_code = jit;
    r = in_child(child_exec);
    check("jit RW exec SIGSEGV", r == -SIGSEGV, r);

    /* Read-only data is not code either. */
    child_code = (volatile char *)(unsigned)&rw_word;
    r = in_child(child_exec);
    check("data exec SIGSEGV", r == -SIGSEGV, r);
}
static void child_write_data(void) { rw_word = 2; }

int main(void) {
    if (install_segv(on_segv, SA_SIGINFO) != 0) {
        printf("wxprobe: rt_sigaction failed\n");
        return 1;
    }

    /* The static image: .text and .rodata are read-only, .data is not. */
    volatile char *text = (volatile char *)(unsigned)&main;
    volatile char *rodata = (volatile char *)(unsigned)ro_msg;
    char text_byte = *text;
    fault_then_fix("text write faults", text, text_byte, PROT_READ | PROT_WRITE | PROT_EXEC);
    check("text re-protect", protect(page_of(text), PROT_READ | PROT_EXEC) == 0, 0);
    /* .rodata shares the text segment (user.ld), and possibly a page with
     * code: keep PROT_EXEC on it, as the segment's p_flags have it. */
    fault_then_fix("rodata write faults", rodata, 'W', PROT_READ | PROT_WRITE | PROT_EXEC);
    *rodata = 'w';                              /* put the text back */
    check("rodata re-protect", protect(page_of(rodata), PROT_READ | PROT_EXEC) == 0, 0);
    rw_word = 3;
    check("data write works", rw_word == 3, rw_word);

    /* A read() into .rodata is refused as a whole: -EFAULT, nothing written. */
    int pfd[2];
    if (pipe(pfd) == 0) {
        write(pfd[1], "Z", 1);
        int r = syscall3(NR_READ, pfd[0], (int)ro_msg, 1);
        check("read into rodata -EFAULT", r == -EFAULT && rodata[0] == 'w', r);
        close(pfd[0]); close(pfd[1]);
    } else {
        check("pipe", 0, -1);
    }

    /* mprotect(PROT_READ) then back to RW, on an anonymous VMA page and on a
     * brk-heap page. */
    volatile char *anon = map_anon();
    anon[0] = 'A';
    check("anon mprotect RO", protect(page_of(anon), PROT_READ) == 0, 0);
    check("anon RO still readable", anon[0] == 'A', anon[0]);
    fault_then_fix("anon RO write faults", anon, 'B', PROT_READ | PROT_WRITE);
    anon[1] = 'b';
    check("anon RW write works", anon[0] == 'B' && anon[1] == 'b', anon[1]);

    volatile char *heap = brk_page();
    heap[0] = 'H';
    check("heap mprotect RO", protect(page_of(heap), PROT_READ) == 0, 0);
    fault_then_fix("heap RO write faults", heap, 'I', PROT_READ | PROT_WRITE);
    heap[1] = 'i';
    check("heap RW write works", heap[0] == 'I' && heap[1] == 'i', heap[1]);

    /* fork: a read-only page stays read-only in the child (the COW handler
     * must not hand out a writable copy) and the parent is untouched. */
    child_target = text;
    int r = in_child(child_write);
    check("fork child text write SIGSEGV", r == -SIGSEGV, r);
    check("fork parent text intact", *text == text_byte, *text);

    child_target = rodata;
    r = in_child(child_write);
    check("fork child rodata write SIGSEGV", r == -SIGSEGV, r);

    check("anon mprotect RO again", protect(page_of(anon), PROT_READ) == 0, 0);
    child_target = anon;
    r = in_child(child_write);
    check("fork child anon RO write SIGSEGV", r == -SIGSEGV, r);
    check("fork parent anon intact", anon[0] == 'B', anon[0]);

    check("heap mprotect RO again", protect(page_of(heap), PROT_READ) == 0, 0);
    child_target = heap;
    r = in_child(child_write);
    check("fork child heap RO write SIGSEGV", r == -SIGSEGV, r);
    check("fork parent heap intact", heap[0] == 'I', heap[0]);

    /* The parent can still take write permission back on the shared frames
     * (the write then breaks COW or reuses the last reference). */
    check("parent anon RW", protect(page_of(anon), PROT_READ | PROT_WRITE) == 0, 0);
    anon[0] = 'P';
    check("parent anon write after fork", anon[0] == 'P', anon[0]);
    check("parent heap RW", protect(page_of(heap), PROT_READ | PROT_WRITE) == 0, 0);
    heap[0] = 'Q';
    check("parent heap write after fork", heap[0] == 'Q', heap[0]);

    /* A writable page is still copy-on-write, not shared. */
    rw_word = 1;
    r = in_child(child_write_data);
    check("fork child data write private", r == 0 && rw_word == 1, r);

    nx_checks();

    if (failures) {
        printf("wxprobe: %d FAILED\n", failures);
        return 1;
    }
    printf("wxprobe ok\n");
    return 0;
}
