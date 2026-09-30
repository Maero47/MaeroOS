/*
 * wxpie — W^X as a dynamically linked PIE sees it.  Built against musl
 * (i686-linux-musl-gcc -fpie -pie) and started through ld-musl, which maps
 * nothing for the program itself — the kernel loaded it — but relocates it and
 * then mprotect()s its PT_GNU_RELRO range read-only.  Checks:
 *   - the program's own .text is read-only (kernel ELF loader, p_flags);
 *   - its RELRO range is read-only after ld.so startup;
 *   - its writable data is still writable;
 *   - the text of libc (the interpreter, also loaded by the kernel) is
 *     read-only;
 *   - a shared object mmap()ed PROT_READ|PROT_EXEC is read-only, as asked.
 * Each refused write must be SIGSEGV with si_code SEGV_ACCERR at the address
 * written.  Prints "wxpie: <case> ok" per case and "WXPIE_OK" at the end;
 * tools/smoke_dyn.py waits for the latter.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <link.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;
static sigjmp_buf env;
static volatile int fault_code;
static volatile void *fault_addr;
static int data_word = 1;

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;
    fault_code = si->si_code;
    fault_addr = si->si_addr;
    siglongjmp(env, 1);
}

static void check(const char *name, int ok) {
    printf("wxpie: %s %s\n", name, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

/* Store to p; 1 if refused with SEGV_ACCERR at p, 0 if it went through,
 * -1 if refused for another reason. */
static int write_refused(volatile char *p) {
    fault_code = 0;
    fault_addr = 0;
    if (sigsetjmp(env, 1) == 0) {
        char c = *p;
        *p = c;
        return 0;
    }
    if (fault_code == SEGV_ACCERR && fault_addr == (void *)p) return 1;
    printf("wxpie: fault at %p code %d (wanted %p)\n", fault_addr, fault_code, (void *)p);
    return -1;
}

static volatile char *relro_addr;

static int find_relro(struct dl_phdr_info *info, size_t size, void *arg) {
    (void)size; (void)arg;
    /* The main program is the first object reported (musl names it argv[0]). */
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_GNU_RELRO) continue;
        /* musl protects [p_vaddr & -PAGE, (p_vaddr + p_memsz) & -PAGE) */
        uintptr_t start = ph->p_vaddr & -4096UL;
        uintptr_t end   = (ph->p_vaddr + ph->p_memsz) & -4096UL;
        if (end > start)
            relro_addr = (volatile char *)(info->dlpi_addr +
                                           (ph->p_vaddr > start ? ph->p_vaddr : start));
    }
    return 1;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);

    check("pie text read-only", write_refused((volatile char *)(uintptr_t)&main) == 1);

    dl_iterate_phdr(find_relro, 0);
    if (!relro_addr) {
        check("pie has a RELRO page", 0);
    } else {
        check("pie RELRO read-only", write_refused(relro_addr) == 1);
    }

    data_word = 2;
    check("pie data writable", write_refused((volatile char *)&data_word) == 0 &&
                               data_word == 2);

    check("libc text read-only", write_refused((volatile char *)(uintptr_t)&printf) == 1);

    int fd = open("/libgreet.so.1", O_RDONLY);
    if (fd < 0) {
        check("open /libgreet.so.1", 0);
    } else {
        volatile char *m = mmap(0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
        close(fd);
        if (m == MAP_FAILED) {
            check("mmap so PROT_READ|PROT_EXEC", 0);
        } else {
            check("mmap so is the ELF", m[0] == 0x7f && m[1] == 'E');
            check("mmap so PROT_READ|PROT_EXEC read-only", write_refused(m) == 1);
            check("mprotect so RW", mprotect((void *)m, 4096, PROT_READ | PROT_WRITE) == 0);
            check("mmap so writable after mprotect", write_refused(m) == 0);
            munmap((void *)m, 4096);
        }
    }

    if (failures) {
        printf("wxpie: %d FAILED\n", failures);
        return 1;
    }
    printf("WXPIE_OK\n");
    return 0;
}
