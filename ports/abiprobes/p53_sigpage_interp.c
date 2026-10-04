/*
 * P53 An ELF interpreter cannot overwrite the shared sigreturn page.
 *
 * The probe writes two tiny ELF files: a program whose PT_INTERP names an
 * interpreter, and that interpreter (ET_DYN), which has a second, writable
 * PT_LOAD at p_vaddr 0x7F7FF000 filled with int3 bytes.  It execs the
 * program in a child, then checks that signal handlers still return.
 *
 * Linux: the interpreter is mapped wherever its span fits; the exec either
 * works (the interpreter exits 0) or fails, and nothing outside the new
 * process changes.
 *
 * MaeroOS before: the interpreter is loaded at 0x40000000, so that segment
 * landed on SIGPAGE_VA (0xBF7FF000), which every process maps from one
 * physical frame.  The loader's "page already mapped" branch remapped that
 * frame writable and copied the file bytes into it: every later signal
 * handler in the system returned into int3 (SIGTRAP).
 */
#define PROBE_NAME "p53_sigpage_interp"
#include "probe.h"
#include <elf.h>
#include <sys/stat.h>
#include <sys/wait.h>

static const char *prog_path = "/tmp/p53_prog";
static const char *interp_path = "/tmp/p53_interp";

static void write_file(const char *path, const void *buf, size_t len)
{
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) probe_fail("create %s: %s", path, strerror(errno));
    if (write(fd, buf, len) != (ssize_t)len) probe_fail("write %s: %s", path, strerror(errno));
    close(fd);
    chmod(path, 0755);
}

static void build_files(void)
{
    /* The interpreter: exit(0) at 0x80; a data page at 0x7F7FF000. */
    static unsigned char ip[0x2000];
    memset(ip, 0, sizeof ip);
    Elf32_Ehdr *eh = (Elf32_Ehdr *)ip;
    memcpy(eh->e_ident, ELFMAG, SELFMAG);
    eh->e_ident[EI_CLASS] = ELFCLASS32;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type = ET_DYN;
    eh->e_machine = EM_386;
    eh->e_version = EV_CURRENT;
    eh->e_entry = 0x80;
    eh->e_phoff = sizeof(Elf32_Ehdr);
    eh->e_ehsize = sizeof(Elf32_Ehdr);
    eh->e_phentsize = sizeof(Elf32_Phdr);
    eh->e_phnum = 2;
    Elf32_Phdr *ph = (Elf32_Phdr *)(ip + eh->e_phoff);
    ph[0].p_type = PT_LOAD;
    ph[0].p_offset = 0;
    ph[0].p_vaddr = ph[0].p_paddr = 0;
    ph[0].p_filesz = ph[0].p_memsz = 0x100;
    ph[0].p_flags = PF_R | PF_X;
    ph[0].p_align = 0x1000;
    ph[1].p_type = PT_LOAD;
    ph[1].p_offset = 0x1000;
    ph[1].p_vaddr = ph[1].p_paddr = 0x7F7FF000;
    ph[1].p_filesz = ph[1].p_memsz = 0x1000;
    ph[1].p_flags = PF_R | PF_W;
    ph[1].p_align = 0x1000;
    static const unsigned char exit0[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,   /* mov eax, 1 (exit) */
        0x31, 0xDB,                     /* xor ebx, ebx      */
        0xCD, 0x80,                     /* int 0x80          */
    };
    memcpy(ip + 0x80, exit0, sizeof exit0);
    memset(ip + 0x1000, 0xCC, 0x1000);  /* int3 */
    write_file(interp_path, ip, sizeof ip);

    /* The program: PT_INTERP + one text segment (exit(1) if ever run). */
    static unsigned char pp[0x100];
    memset(pp, 0, sizeof pp);
    eh = (Elf32_Ehdr *)pp;
    memcpy(eh->e_ident, ELFMAG, SELFMAG);
    eh->e_ident[EI_CLASS] = ELFCLASS32;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type = ET_EXEC;
    eh->e_machine = EM_386;
    eh->e_version = EV_CURRENT;
    eh->e_entry = 0x08048000 + 0xC0;
    eh->e_phoff = sizeof(Elf32_Ehdr);
    eh->e_ehsize = sizeof(Elf32_Ehdr);
    eh->e_phentsize = sizeof(Elf32_Phdr);
    eh->e_phnum = 2;
    ph = (Elf32_Phdr *)(pp + eh->e_phoff);
    ph[0].p_type = PT_INTERP;
    ph[0].p_offset = 0x80;
    ph[0].p_filesz = ph[0].p_memsz = (Elf32_Word)strlen(interp_path) + 1;
    ph[0].p_flags = PF_R;
    ph[0].p_align = 1;
    ph[1].p_type = PT_LOAD;
    ph[1].p_offset = 0;
    ph[1].p_vaddr = ph[1].p_paddr = 0x08048000;
    ph[1].p_filesz = ph[1].p_memsz = sizeof pp;
    ph[1].p_flags = PF_R | PF_X;
    ph[1].p_align = 0x1000;
    memcpy(pp + 0x80, interp_path, strlen(interp_path) + 1);
    static const unsigned char exit1[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xBB, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80,
    };
    memcpy(pp + 0xC0, exit1, sizeof exit1);
    write_file(prog_path, pp, sizeof pp);
}

static volatile sig_atomic_t got;
static void on_usr1(int sig) { (void)sig; got = 1; }

/* A fresh process takes SIGUSR1 into a handler and returns from it. */
static void check_handler_returns(const char *when)
{
    pid_t c = fork();
    if (c < 0) probe_fail("fork: %s", strerror(errno));
    if (c == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_usr1;
        sigaction(SIGUSR1, &sa, NULL);
        raise(SIGUSR1);
        _exit(got ? 0 : 3);
    }
    int st;
    if (waitpid(c, &st, 0) != c) probe_fail("waitpid: %s", strerror(errno));
    if (WIFSIGNALED(st))
        probe_fail("%s: a signal handler's return killed the process with signal %d "
                   "(the shared sigreturn trampoline was overwritten)", when, WTERMSIG(st));
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        probe_fail("%s: handler check exited with status %#x", when, st);
}

int main(void)
{
    probe_watchdog(60);
    check_handler_returns("before the exec");
    build_files();

    pid_t c = fork();
    if (c < 0) probe_fail("fork: %s", strerror(errno));
    if (c == 0) {
        char *argv[] = { (char *)prog_path, NULL };
        char *envp[] = { NULL };
        execve(prog_path, argv, envp);
        _exit(127);
    }
    int st;
    if (waitpid(c, &st, 0) != c) probe_fail("waitpid: %s", strerror(errno));
    if (WIFEXITED(st))
        probe_info("crafted exec: exit status %d (0: ran, 127: refused)", WEXITSTATUS(st));
    else if (WIFSIGNALED(st))
        probe_info("crafted exec: killed by signal %d", WTERMSIG(st));

    check_handler_returns("after the exec");
    unlink(prog_path);
    unlink(interp_path);
    probe_pass();
}
