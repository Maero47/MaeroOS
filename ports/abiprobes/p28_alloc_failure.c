/*
 * P28 alloc-failure - an unprivileged process must not be able to halt the
 * machine by asking the kernel for memory.
 *
 * Every kernel allocation the probe drives here used to reach one of two
 * unconditional halts: heap_expand() printed "[HEAP] FATAL: heap exhausted"
 * and entered `for(;;) hlt` once the 256 MiB heap window was full, and
 * vmm_alloc_page() did the same on physical exhaustion (mm/heap.c,
 * mm/vmm.c).  kmalloc() therefore could not return NULL, which in turn made
 * every "if (!p) return -ENOMEM;" in the kernel unreachable code.
 *
 * The class, not one path: this drives five independent allocators and
 * requires the SAME property of each - a clean errno, the process still
 * running afterwards, and a kernel that can still fork and exec.
 *
 *   A  anonymous mmap until the address space runs out                  ENOMEM
 *      then, in a child, every page of each mapping TOUCHED until the
 *      physical allocator runs dry: the faulting touch is a SIGSEGV the
 *      child catches (fault-time OOM), and a read() into a never-touched
 *      buffer at that point must end in EFAULT/ENOMEM or a short count -
 *      the kernel's copy_to_user fault cannot be backed either
 *   B  pipes, until the descriptor table / kernel objects run out       EMFILE
 *   C  AF_UNIX socketpairs, likewise                                    EMFILE
 *   D  a large tmpfs write - file bodies used to come straight out of   ENOMEM
 *      the kernel heap (HEAP_START..HEAP_MAX); they are page frames now
 *      (p29), so this is one more path to physical exhaustion
 *   E  an execve argv the kernel must refuse rather than size a buffer  E2BIG
 *      from (4096 pointers at one 128 KiB string = 512 MiB if charged
 *      per pointer)
 *
 * Linux passes this unchanged: it returns the same errnos from the same
 * calls.  On Linux the mmap flood ends at address-space exhaustion rather
 * than at physical exhaustion (nothing is touched, so nothing is committed),
 * which is the same observable - mmap fails with ENOMEM and the process
 * lives.  Private mappings fault in lazily on both kernels, so physical
 * exhaustion needs the touching child; on Linux the OOM killer may take that
 * child with SIGKILL instead, which is accepted too (the parent and the
 * kernel live on).  Where a phase completes within its budget without failing, that is
 * reported and accepted: the assertion is conditional ("if it fails, it fails
 * cleanly"), because forcing a real Linux host to its knees is not the point.
 *
 * What is NOT conditional is the last phase: after all that abuse the kernel
 * must still fork and execve.  That is the property the halts destroyed.
 */
#define PROBE_NAME "p28_alloc_failure"
#include "probe.h"
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <setjmp.h>
#include <signal.h>

/* Private anonymous mappings are populated lazily, at first touch (MaeroOS
 * and Linux alike), so mmap() alone only uses address space; phase A's child
 * touches every page to reach physical exhaustion. */
#define CHUNK       (2u * 1024 * 1024)
#define MAX_CHUNKS  2048            /* 4 GiB of address space: 32-bit runs out */
#define MAX_FDS     4096
#define TMP_CAP     (128u * 1024 * 1024)
#define TMP_STEP    (1u * 1024 * 1024)

static void *chunks[MAX_CHUNKS];
static int   pipes[MAX_FDS][2];
static int   socks[MAX_FDS][2];

/* A syscall that must still work while the machine is starved, and a value
 * the kernel cannot fake. */
static void still_alive(const char *when)
{
    pid_t p = getpid();
    if (p <= 0)
        probe_fail("%s: getpid() returned %d", when, (int)p);
}

/* ── A: anonymous mmap until the address space runs out ─────────────────── */
static void phase_mmap(void)
{
    size_t n = 0;
    int    failed = 0;

    for (; n < MAX_CHUNKS; n++) {
        errno = 0;
        void *p = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            failed = 1;
            if (errno != ENOMEM)
                probe_fail("mmap #%zu failed with %s, expected ENOMEM",
                           n, strerror(errno));
            break;
        }
        chunks[n] = p;
    }

    still_alive("after the mmap flood");

    if (failed)
        probe_info("A: mmap refused chunk %zu with ENOMEM after %zu MiB "
                   "(the process is still running)", n, (n * CHUNK) >> 20);
    else
        probe_info("A: %zu MiB of anonymous mappings all succeeded "
                   "(budget reached, nothing to refuse)", (n * CHUNK) >> 20);

    /* Hand it all back before the next phase, and prove munmap survives a
     * kernel that has just been at its limit. */
    for (size_t i = 0; i < n; i++)
        if (munmap(chunks[i], CHUNK) != 0)
            probe_fail("munmap of chunk %zu failed: %s", i, strerror(errno));

    /* The address space must be reusable afterwards: a failed mmap that left
     * a VMA behind would show up here. */
    void *again = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (again == MAP_FAILED)
        probe_fail("mmap of one chunk failed after everything was unmapped: %s",
                   strerror(errno));
    memset(again, 0xA5, CHUNK);          /* it must be real, writable memory */
    if (((unsigned char *)again)[CHUNK - 1] != 0xA5)
        probe_fail("the reclaimed mapping did not hold what was written to it");
    munmap(again, CHUNK);
    probe_info("A: the address space is reusable after the refusal");
}

/* ── A': every page touched, until a touch cannot be backed ──────────────── */
static sigjmp_buf      oom_env;
static volatile void  *oom_addr;
static char            oom_stack[64 * 1024];

static void oom_segv(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    oom_addr = si->si_addr;
    siglongjmp(oom_env, 1);
}

/* Child exit codes: what happened, for the parent to judge. */
enum { T_BUDGET = 0, T_OOM_OK = 10, T_STRAY_SEGV = 20, T_READ_BAD = 21,
       T_SETUP = 22 };

static int touch_child(void)
{
    /* Live across the siglongjmp, so volatile. */
    volatile size_t n = 0, touched = 0;
    int zfd = open("/dev/zero", O_RDONLY);
    if (zfd < 0)
        return T_SETUP;

    /* The handler's stack and frame must not need a new page. */
    memset(oom_stack, 0, sizeof oom_stack);
    stack_t ss = { .ss_sp = oom_stack, .ss_size = sizeof oom_stack };
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = oom_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaltstack(&ss, NULL) != 0 || sigaction(SIGSEGV, &sa, NULL) != 0)
        return T_SETUP;

    unsigned char *volatile cur = NULL;
    if (sigsetjmp(oom_env, 1) == 0) {
        for (; n < MAX_CHUNKS; n++) {
            void *p = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED)
                break;                       /* address space first: budget */
            chunks[n] = p;
            cur = p;
            for (size_t off = 0; off < CHUNK; off += 4096) {
                ((volatile unsigned char *)cur)[off] = 1;
                touched += 4096;
            }
        }
        for (size_t i = 0; i < n; i++)
            munmap(chunks[i], CHUNK);
        probe_info("A': touched %zu MiB without running out (budget reached)",
                   (size_t)touched >> 20);
        fflush(stdout);
        return T_BUDGET;
    }

    /* Here a touch could not be backed.  It must have been our own chunk. */
    unsigned char *a = (unsigned char *)oom_addr;
    if (!cur || a < (unsigned char *)cur || a >= (unsigned char *)cur + CHUNK)
        return T_STRAY_SEGV;
    n++;                                     /* chunks[n] is mapped too */

    /* Still exhausted: a syscall writing into an untouched buffer. */
    int rc = T_OOM_OK;
    ssize_t r = -1;
    int err = 0;
    void *buf = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf != MAP_FAILED) {
        errno = 0;
        r = read(zfd, buf, CHUNK);
        err = errno;
        if (r < 0 && err != EFAULT && err != ENOMEM)
            rc = T_READ_BAD;
        munmap(buf, CHUNK);
    }
    for (size_t i = 0; i < n; i++)
        munmap(chunks[i], CHUNK);
    probe_info("A': SIGSEGV on the touch at %zu MiB (fault-time OOM), caught",
               (size_t)touched >> 20);
    if (buf == MAP_FAILED)
        probe_info("A': no address space left for the read() buffer");
    else if (r < 0)
        probe_info("A': read() into an untouched buffer at OOM: %s", strerror(err));
    else
        probe_info("A': read() into an untouched buffer at OOM: %zd of %u bytes",
                   r, (unsigned)CHUNK);
    fflush(stdout);
    return rc;
}

static void phase_touch(void)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        probe_fail("fork for the touch phase failed: %s", strerror(errno));
    if (pid == 0)
        _exit(touch_child());
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    still_alive("after the touch child");
    if (WIFSIGNALED(st)) {
        if (WTERMSIG(st) != SIGKILL)
            probe_fail("the touching child died of signal %d (expected a "
                       "caught SIGSEGV, or the OOM killer's SIGKILL)", WTERMSIG(st));
        probe_info("A': the touching child was OOM-killed (SIGKILL)");
        return;
    }
    switch (WIFEXITED(st) ? WEXITSTATUS(st) : -1) {
    case T_BUDGET: case T_OOM_OK:
        return;
    case T_STRAY_SEGV:
        probe_fail("fault-time OOM: SIGSEGV for an address outside the chunk being touched");
    case T_READ_BAD:
        probe_fail("fault-time OOM: read() into an untouched buffer failed with an "
                   "errno other than EFAULT/ENOMEM");
    case T_SETUP:
        probe_fail("touch phase: /dev/zero, sigaltstack or sigaction failed");
    default:
        probe_fail("the touching child exited %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    }
}

/* ── B: pipes ────────────────────────────────────────────────────────────── */
static void phase_pipes(void)
{
    int n = 0, failed = 0;

    for (; n < MAX_FDS; n++) {
        errno = 0;
        if (pipe(pipes[n]) != 0) {
            failed = 1;
            if (errno != EMFILE && errno != ENFILE && errno != ENOMEM)
                probe_fail("pipe #%d failed with %s, expected EMFILE/ENFILE/ENOMEM",
                           n, strerror(errno));
            break;
        }
    }
    still_alive("after the pipe flood");

    if (failed)
        probe_info("B: pipe #%d refused with %s after %d pipes", n,
                   strerror(errno), n);
    else
        probe_info("B: %d pipes all succeeded (budget reached)", n);

    /* Every pipe must still WORK - a kernel that handed out objects it could
     * not back would show up as a short or failed round trip, which is worse
     * than the refusal. */
    if (n > 0) {
        char c = 'k', got = 0;
        if (write(pipes[n - 1][1], &c, 1) != 1)
            probe_fail("write to the last pipe failed: %s", strerror(errno));
        if (read(pipes[n - 1][0], &got, 1) != 1 || got != 'k')
            probe_fail("read back from the last pipe gave %d", (int)got);
        probe_info("B: the last pipe created still round-trips a byte");
    }
    for (int i = 0; i < n; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

/* ── C: AF_UNIX socketpairs ──────────────────────────────────────────────── */
static void phase_sockets(void)
{
    int n = 0, failed = 0;

    for (; n < MAX_FDS; n++) {
        errno = 0;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks[n]) != 0) {
            failed = 1;
            if (errno != EMFILE && errno != ENFILE && errno != ENOMEM &&
                errno != ENOBUFS)
                probe_fail("socketpair #%d failed with %s, expected "
                           "EMFILE/ENFILE/ENOMEM/ENOBUFS", n, strerror(errno));
            break;
        }
    }
    still_alive("after the socketpair flood");

    if (failed)
        probe_info("C: socketpair #%d refused with %s after %d pairs", n,
                   strerror(errno), n);
    else
        probe_info("C: %d socketpairs all succeeded (budget reached)", n);

    if (n > 0) {
        char c = 's', got = 0;
        if (write(socks[n - 1][1], &c, 1) != 1)
            probe_fail("write to the last socketpair failed: %s", strerror(errno));
        if (read(socks[n - 1][0], &got, 1) != 1 || got != 's')
            probe_fail("read back from the last socketpair gave %d", (int)got);
        probe_info("C: the last socketpair created still round-trips a byte");
    }
    for (int i = 0; i < n; i++) {
        close(socks[i][0]);
        close(socks[i][1]);
    }
}

/* ── D: a large tmpfs write ──────────────────────────────────────────────── */
static void phase_tmpfs(void)
{
    static char buf[TMP_STEP];
    const char *path = "/tmp/p28_alloc_failure.tmp";
    unsigned written = 0;
    int failed = 0;

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        probe_info("D: cannot create %s (%s) - skipping the tmpfs phase",
                   path, strerror(errno));
        return;
    }
    memset(buf, 'D', sizeof buf);

    while (written < TMP_CAP) {
        errno = 0;
        ssize_t w = write(fd, buf, sizeof buf);
        if (w < 0) {
            failed = 1;
            /* Linux tmpfs: ENOMEM when the page allocation fails, ENOSPC when
             * the mount's size limit is hit.  Either is "nothing was written";
             * a 0 return would not be, and would spin a libc write loop. */
            if (errno != ENOMEM && errno != ENOSPC && errno != EFBIG)
                probe_fail("write to tmpfs failed with %s after %u MiB, "
                           "expected ENOMEM/ENOSPC/EFBIG",
                           strerror(errno), written >> 20);
            break;
        }
        if (w == 0)
            probe_fail("write to tmpfs returned 0 for a %u-byte request after "
                       "%u MiB - a caller cannot tell that from progress and "
                       "would loop forever", (unsigned)sizeof buf, written >> 20);
        written += (unsigned)w;
    }
    still_alive("after the tmpfs write");

    if (failed)
        probe_info("D: tmpfs write refused with %s after %u MiB",
                   strerror(errno), written >> 20);
    else
        probe_info("D: wrote %u MiB to tmpfs without a refusal (budget reached)",
                   written >> 20);

    /* Whatever it did accept must be readable back: a short write that lost
     * data would be worse than the refusal. */
    if (written > 0) {
        char probe_byte = 0;
        if (lseek(fd, (off_t)written - 1, SEEK_SET) < 0 ||
            read(fd, &probe_byte, 1) != 1)
            probe_fail("cannot read back the last byte written to tmpfs: %s",
                       strerror(errno));
        if (probe_byte != 'D')
            probe_fail("tmpfs gave back 0x%02x where 'D' was written",
                       (unsigned char)probe_byte);
        probe_info("D: the %u MiB tmpfs accepted reads back correctly",
                   written >> 20);
    }
    close(fd);
    unlink(path);
}

/* ── E: an argv the kernel must refuse rather than size a buffer from ────── */
static void phase_exec(const char *self)
{
    static char *flood[4098];
    size_t rep = 128 * 1024;
    char *repeated = malloc(rep + 1);

    if (!repeated) {
        probe_info("E: cannot allocate %zu KiB for the argv string - skipped",
                   rep >> 10);
        return;
    }
    memset(repeated, 'b', rep);
    repeated[rep] = 0;

    flood[0] = (char *)self;
    flood[1] = (char *)"--child";
    for (int i = 2; i < 4096; i++)
        flood[i] = repeated;
    flood[4096] = NULL;

    char *nev[] = { NULL };
    errno = 0;
    execve(self, flood, nev);
    if (errno != E2BIG)
        probe_fail("execve with 4096 argv pointers at one %zu KiB string failed "
                   "with %s, expected E2BIG", rep >> 10, strerror(errno));
    free(repeated);
    still_alive("after the refused execve");
    probe_info("E: 4096 argv pointers at one %zu KiB string refused with E2BIG",
               rep >> 10);
}

/* ── F: the kernel must still be able to fork and exec ───────────────────── */
static void phase_still_works(const char *self)
{
    char *av[] = { (char *)self, (char *)"--child", NULL };
    char *ev[] = { NULL };

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        probe_fail("fork after the whole run failed: %s", strerror(errno));
    if (pid == 0) {
        execve(self, av, ev);
        _exit(97);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (WIFSIGNALED(st))
        probe_fail("the post-run child died with signal %d", WTERMSIG(st));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 42)
        probe_fail("the post-run child exited %d, expected 42",
                   WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    probe_info("F: fork+execve still work after every phase");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--child") == 0)
        return 42;

    char self[4096];
    if (!probe_self_path(argv[0], self, sizeof self))
        probe_skip("cannot find own executable path (argv[0]=%s)", argv[0]);

    /* Zeroing and freeing hundreds of MiB inside a TCG-emulated guest is slow;
     * this is the budget the driver's table mirrors. */
    probe_watchdog(300);

    /* Best-effort on the host, so phases B and C reach a refusal there too
     * rather than only inside the guest.  MaeroOS's own limit (MAX_FD) is
     * lower than this anyway, and a kernel without setrlimit just ignores it. */
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 256) {
            rl.rlim_cur = 256;
            (void)setrlimit(RLIMIT_NOFILE, &rl);
        }
    }

    phase_mmap();
    phase_touch();
    phase_pipes();
    phase_sockets();
    phase_tmpfs();
    phase_exec(self);
    phase_still_works(self);

    probe_pass();
}
