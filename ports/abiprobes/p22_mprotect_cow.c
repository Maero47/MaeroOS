/*
 * P22 mprotect vs copy-on-write - a write to a page the process made read-only
 * must fault, whether or not that page is inside an mmap()ed region.
 *
 * Linux: mprotect(PROT_READ) removes write permission from the pages of the
 * range whatever they are (mm/mprotect.c change_protection); fork() then marks
 * them copy-on-write like everything else, and do_wp_page is only reached when
 * the VMA still permits writing (mm/memory.c access_error), so a later write
 * raises SIGSEGV in both parent and child.
 *
 * The bug this catches: a kernel whose write-fault handler decides "may I break
 * COW?" by looking the address up in its mmap registry and treating "not found"
 * as "yes".  The ELF image (ld.so's PT_GNU_RELRO lives there), the brk heap and
 * the main stack are not in that registry, so mprotect(PROT_READ) on such a
 * page plus a fork lets the next write succeed by copying the page: no fault,
 * and the two address spaces silently diverge.  The same happens with no live
 * sharer at all, because a leftover COW page with one reference takes the
 * "reuse the frame" shortcut and simply re-grants write permission.
 *
 * The page under test is a page-aligned, page-sized object in the executable's
 * own image, exactly like RELRO and outside any mmap registry.  Each case runs
 * in a forked child so a correct SIGSEGV is a wait status, not a dead probe.
 *
 * Cases:
 *   A  mprotect(PROT_READ) then fork, child writes      -> SIGSEGV
 *   B  fork then mprotect(PROT_READ) in the child       -> SIGSEGV
 *   C  fork, sharer exits, then mprotect, then write    -> SIGSEGV
 *      (the one-reference "reuse" path)
 *   D  control: an ordinary COW break still works and stays private
 *   E  control: mprotect(PROT_READ|PROT_WRITE) restores writability
 */
#define PROBE_NAME "p22_mprotect_cow"
#include "probe.h"
#include <sys/mman.h>
#include <sys/wait.h>

#define PAGE 4096

/* In the image, one page to itself: the alignment plus the size mean no other
 * object shares this page and mprotect cannot disturb anything else. */
static volatile unsigned char guarded[PAGE] __attribute__((aligned(PAGE))) = { 1 };

/* A second one for the controls. */
static volatile unsigned char plain[PAGE] __attribute__((aligned(PAGE))) = { 1 };

static void protect_ro(volatile unsigned char *p)
{
    if (mprotect((void *)(uintptr_t)p, PAGE, PROT_READ) != 0)
        probe_fail("mprotect(PROT_READ) on %p: %s", (void *)p, strerror(errno));
}

/* Run `body` in a child and report how it ended: 0 = exited 0, 1 = exited 1,
 * -SIGNUM = killed by a signal. */
static int run_child(void (*body)(void))
{
    pid_t pid = fork();
    int st = 0;
    if (pid < 0)
        probe_fail("fork: %s", strerror(errno));
    if (pid == 0) {
        body();
        _exit(0);
    }
    if (waitpid(pid, &st, 0) != pid)
        probe_fail("waitpid: %s", strerror(errno));
    if (WIFSIGNALED(st))
        return -WTERMSIG(st);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

static void child_write_guarded(void)
{
    guarded[0] = 0xAA;          /* must fault */
    _exit(0);                   /* reached only if the write was allowed */
}

static void child_sole_owner_then_write(void)
{
    /* Make the page copy-on-write by forking, let the sharer go away, and only
     * then take write permission off.  The page is now COW with a single
     * reference, which is the shortcut path. */
    pid_t g = fork();
    int st = 0;
    if (g < 0)
        _exit(2);
    if (g == 0)
        _exit(0);               /* grandchild: just be a sharer, then leave */
    if (waitpid(g, &st, 0) != g)
        _exit(2);
    protect_ro(guarded);
    guarded[2] = 0xCC;          /* must fault */
    _exit(0);
}

static void expect_segv(const char *label, int outcome)
{
    if (outcome == -SIGSEGV) {
        probe_info("%s: write to the read-only page raised SIGSEGV", label);
        return;
    }
    if (outcome == 0)
        probe_fail("%s: the write to a PROT_READ page SUCCEEDED (no fault)", label);
    if (outcome < 0)
        probe_fail("%s: killed by signal %d, expected SIGSEGV (%d)", label,
                   -outcome, SIGSEGV);
    probe_fail("%s: child exited %d, expected death by SIGSEGV", label, outcome);
}

int main(void)
{
    int r;

    probe_watchdog(60);

    /* Touch both pages so they are populated and dirty before anything else. */
    guarded[0] = 0x11;
    plain[0] = 0x22;

    /* ── A: mprotect, then fork, then the child writes ──────────────────── */
    protect_ro(guarded);
    expect_segv("A (mprotect then fork)", run_child(child_write_guarded));

    /* The parent must not be able to write it either. */
    expect_segv("A (parent)", run_child(child_write_guarded));

    /* ── B: fork first, mprotect inside the child ───────────────────────── */
    /* `guarded` is already read-only here, so use the other page: the child
     * makes it read-only itself while the parent still shares it. */
    {
        pid_t pid = fork();
        int st = 0;
        if (pid < 0)
            probe_fail("fork: %s", strerror(errno));
        if (pid == 0) {
            protect_ro(plain);
            plain[1] = 0xBB;    /* must fault */
            _exit(0);
        }
        if (waitpid(pid, &st, 0) != pid)
            probe_fail("waitpid: %s", strerror(errno));
        expect_segv("B (fork then mprotect)",
                    WIFSIGNALED(st) ? -WTERMSIG(st)
                                    : (WIFEXITED(st) ? WEXITSTATUS(st) : 99));
    }
    /* The parent never asked for read-only, so its own write still works. */
    plain[2] = 0x33;
    if (plain[2] != 0x33)
        probe_fail("B: the parent's page lost its write permission");

    /* ── C: sole owner of a COW page, then mprotect, then write ─────────── */
    expect_segv("C (one reference, reuse path)",
                run_child(child_sole_owner_then_write));

    /* ── D: control — an ordinary COW break still works and stays private ─ */
    plain[3] = 0x44;
    {
        pid_t pid = fork();
        int st = 0;
        if (pid < 0)
            probe_fail("fork: %s", strerror(errno));
        if (pid == 0) {
            plain[3] = 0x55;                    /* legitimate COW break */
            _exit(plain[3] == 0x55 ? 0 : 1);
        }
        if (waitpid(pid, &st, 0) != pid)
            probe_fail("waitpid: %s", strerror(errno));
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            probe_fail("D: a normal COW write failed (status 0x%x) — the fix "
                       "over-blocks", st);
        if (plain[3] != 0x44)
            probe_fail("D: the child's COW write leaked into the parent (%02x)",
                       plain[3]);
        probe_info("D: a normal copy-on-write break still works and stays private");
    }

    /* ── E: control — granting write back really grants it ──────────────── */
    if (mprotect((void *)(uintptr_t)guarded, PAGE, PROT_READ | PROT_WRITE) != 0)
        probe_fail("mprotect(PROT_READ|PROT_WRITE): %s", strerror(errno));
    guarded[4] = 0x66;
    if (guarded[4] != 0x66)
        probe_fail("E: the page is still unwritable after mprotect(PROT_WRITE)");
    r = run_child(child_write_guarded);
    if (r != 0)
        probe_fail("E: a child still cannot write the page after PROT_WRITE was "
                   "restored (outcome %d)", r);
    probe_info("E: mprotect(PROT_READ|PROT_WRITE) restores writability in both "
               "the process and its children");

    probe_pass();
}
