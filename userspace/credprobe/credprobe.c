/*
 * credprobe — the syscall layer must enforce Unix credentials, permissions and
 * descriptor access modes the way Linux does.
 *
 * Run as root (smoke-disk does), it prepares root-owned fixtures, checks the
 * root-side rules (atomic rename that keeps the renamed file's mode and owner,
 * O_EXCL, "#!" interpreter checks), then drops its real ids to uid 1000 /
 * gid 100 and re-executes a set-uid root (04755) copy of itself.  The disk
 * image ships /disk/credprobe as a plain 0755 program: the root half copies it
 * into a fresh root-owned 0755 directory (SUID_DIR, which no other user can
 * add to), sets the bit on the copy only for the length of the run and
 * removes it afterwards.  That second half starts as ruid 1000 / euid 0 /
 * suid 0, checks the saved-set-uid rules, drops for good with
 * setresuid(1000,1000,1000), and then checks that an ordinary user cannot
 * unlink, rename or truncate root's files, write through a read-only
 * descriptor, map one writable, signal init, move another process's group,
 * or power off, restart or halt the machine with reboot(2).
 *
 * Prints "credprobe: <case> ok" per case and "credprobe ok" at the end;
 * tools/smoke_disk.py waits for the latter and fails on "FAILED".
 */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/syscall.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"
#include "../include/errno.h"
#include "../include/signal.h"
#include "../include/sys/stat.h"
#include "../include/sys/wait.h"

#define NR_UNLINK       10
#define NR_EXECVE       11
#define NR_KILL         37
#define NR_RENAME       38
#define NR_WRITE        4
#define NR_OPEN         5
#define NR_FCNTL        55
#define NR_SETPGID      57
#define NR_TRUNCATE     92
#define NR_FTRUNCATE    93
#define NR_MPROTECT     125
#define NR_PWRITE64     181
#define NR_REBOOT       88
#define NR_MMAP2        192
#define NR_MUNMAP       91
#define NR_FCHOWN32     207
#define NR_SETRESUID32  208
#define NR_GETRESUID32  209
#define NR_SETRESGID32  210
#define NR_SETUID32     213
#define NR_SYMLINK      83
#define NR_FALLOCATE    324

#define K_O_RDONLY   0
#define K_O_WRONLY   1
#define K_O_RDWR     2
#define K_O_CREAT    0x40
#define K_O_EXCL     0x80
#define K_O_TRUNC    0x200
#define K_O_APPEND   0x400
#define K_O_NONBLOCK 0x800
#define K_O_NOFOLLOW 0x20000
#define K_F_SETFL    4

#define K_PROT_READ  1
#define K_PROT_WRITE 2
#define K_MAP_SHARED 1

#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EBADF   9
#define EACCES  13
#define EEXIST  17
#define EXDEV   18
#define EINVAL  22
#define ELOOP   40

#define USER_UID 1000
#define USER_GID 100

/* Fixtures.  /home is root:root 0755 on the disk image; /tmp is the sticky
 * world-writable tmpfs. */
#define ROOT_FILE     "/home/cp_root.txt"
#define TMP_ROOT_FILE "/tmp/cp_sticky"
#define TMP_ROOT_DIR  "/tmp/cp_rdir"
#define TMP_DIR_FILE  "/tmp/cp_rdir/f"
/* The set-uid copy lives in a directory only root can write to, created
 * fresh for the run; files the set-uid half creates go there too, never
 * into the shared /tmp where another user could plant a symlink. */
#define SUID_DIR      "/tmp/cp_suid"
#define SUID_BIN      SUID_DIR "/credprobe"
#define SUID_MADE     SUID_DIR "/made"

static int failures;

static void check(const char *name, int ok, int got) {
    if (ok) printf("credprobe: %s ok\n", name);
    else  { printf("credprobe: %s FAILED (got %d)\n", name, got); failures++; }
}

static int sys_mmap2(unsigned len, int prot, int flags, int fd) {
    int ret;
    __asm__ volatile(
        "push %%ebp\n"
        "mov $0, %%ebp\n"                  /* pgoffset */
        "int $0x80\n"
        "pop %%ebp\n"
        : "=a"(ret)
        : "a"(NR_MMAP2), "b"(0), "c"(len), "d"(prot), "S"(flags), "D"(fd)
        : "memory");
    return ret;
}

static int kopen(const char *path, int flags) {
    return syscall3(NR_OPEN, (int)path, flags, 0644);
}

/* Create `path` holding `text`, owned by uid:gid with `mode`.  The old file
 * is removed first and the new one made with O_EXCL|O_NOFOLLOW, so a symlink
 * planted in /tmp is never followed. */
static int make_file(const char *path, const char *text, int mode, int uid, int gid) {
    syscall1(NR_UNLINK, (int)path);
    int fd = kopen(path, K_O_WRONLY | K_O_CREAT | K_O_EXCL | K_O_NOFOLLOW);
    if (fd < 0) { printf("credprobe: cannot create %s (%d)\n", path, fd); return -1; }
    syscall3(NR_WRITE, fd, (int)text, (int)strlen(text));
    syscall3(NR_FCHOWN32, fd, uid, gid);
    close(fd);
    chmod(path, mode);
    return 0;
}

static int file_has(const char *path, const char *text) {
    char buf[64];
    int fd = kopen(path, K_O_RDONLY);
    if (fd < 0) return 0;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

/* rename replaces the target in one step and keeps the SOURCE's inode: its
 * mode, owner and contents move with it. */
static void rename_keeps_inode(const char *tag, const char *from, const char *to) {
    char name[64];
    make_file(from, "new", 0600, USER_UID, USER_GID);
    make_file(to, "old", 0644, 0, 0);
    int r = syscall2(NR_RENAME, (int)from, (int)to);
    snprintf(name, sizeof(name), "%s rename over target", tag);
    check(name, r == 0, r);
    struct stat st;
    int sr = stat(to, &st);
    snprintf(name, sizeof(name), "%s rename keeps mode and owner", tag);
    int ok = sr == 0 && (st.st_mode & 07777) == 0600 &&
             st.st_uid == USER_UID && st.st_gid == USER_GID;
    if (!ok && sr == 0)
        printf("credprobe: %s: mode %o uid %d gid %d\n", to,
               (int)(st.st_mode & 07777), (int)st.st_uid, (int)st.st_gid);
    check(name, ok, sr);
    snprintf(name, sizeof(name), "%s rename moves contents", tag);
    check(name, file_has(to, "new"), 0);
    snprintf(name, sizeof(name), "%s rename removes source", tag);
    check(name, kopen(from, K_O_RDONLY) == -ENOENT, 0);
    syscall1(NR_UNLINK, (int)to);
}

/* execve() of `path` in a child; returns the errno it failed with (0 if the
 * exec succeeded and the child exited 0). */
static int exec_errno(const char *path) {
    int pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)path, (char *)0 };
        char *envp[] = { (char *)0 };
        int r = syscall3(NR_EXECVE, (int)path, (int)argv, (int)envp);
        _exit(-r);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WEXITSTATUS(st);
}

static void root_checks(void) {
    /* passwd replaces /etc/shadow by rename; it must stay root's and 0600. */
    struct stat sh;
    int sr = stat("/etc/shadow", &sh);
    check("/etc/shadow is root 0600", sr == 0 && sh.st_uid == 0 &&
          (sh.st_mode & 07777) == 0600, sr == 0 ? (int)(sh.st_mode & 07777) : sr);

    rename_keeps_inode("ext2", "/home/cp_new", "/home/cp_old");
    rename_keeps_inode("tmpfs", "/tmp/cp_new", "/tmp/cp_old");

    make_file("/tmp/cp_xdev", "x", 0644, 0, 0);
    int r = syscall2(NR_RENAME, (int)"/tmp/cp_xdev", (int)"/home/cp_xdev");
    check("rename across filesystems is EXDEV", r == -EXDEV, r);
    syscall1(NR_UNLINK, (int)"/tmp/cp_xdev");

    /* The same through a symlink: /tmp/cp_l -> /tmp/cp_a/b, so
     * "/tmp/cp_l/x" is inside /tmp/cp_a although the path does not say so.
     * Letting it through would link cp_a below its own child. */
    mkdir("/tmp/cp_a", 0755);
    mkdir("/tmp/cp_a/b", 0755);
    syscall2(NR_SYMLINK, (int)"/tmp/cp_a/b", (int)"/tmp/cp_l");
    r = syscall2(NR_RENAME, (int)"/tmp/cp_a", (int)"/tmp/cp_l/x");
    check("rename dir under itself via symlink is EINVAL", r == -EINVAL, r);
    struct stat ast;
    check("dir still in place after refused rename",
          stat("/tmp/cp_a/b", &ast) == 0 && S_ISDIR(ast.st_mode), 0);
    syscall1(NR_UNLINK, (int)"/tmp/cp_l");
    rmdir("/tmp/cp_a/x");
    rmdir("/tmp/cp_a/b");
    rmdir("/tmp/cp_a");

    mkdir("/tmp/cp_loopdir", 0755);
    r = syscall2(NR_RENAME, (int)"/tmp/cp_loopdir", (int)"/tmp/cp_loopdir/sub");
    check("rename dir into itself is EINVAL", r == -EINVAL, r);
    rmdir("/tmp/cp_loopdir");

    r = kopen("/etc/passwd", K_O_WRONLY | K_O_CREAT | K_O_EXCL);
    check("O_CREAT|O_EXCL on existing is EEXIST", r == -EEXIST, r);
    if (r >= 0) close(r);
    r = kopen("/tmp/cp_excl", K_O_WRONLY | K_O_CREAT | K_O_EXCL);
    check("O_CREAT|O_EXCL on new creates", r >= 0, r);
    if (r >= 0) close(r);
    syscall1(NR_UNLINK, (int)"/tmp/cp_excl");

    /* "#!" chain: the interpreter needs execute permission, and a chain
     * that never reaches a binary ends in ELOOP. */
    make_file("/tmp/cp_noexec", "not a program\n", 0644, 0, 0);
    make_file("/tmp/cp_script", "#!/tmp/cp_noexec\n", 0755, 0, 0);
    make_file("/tmp/cp_loop", "#!/tmp/cp_loop\n", 0755, 0, 0);
    r = exec_errno("/tmp/cp_script");
    check("script interpreter without x is EACCES", r == EACCES, r);
    r = exec_errno("/tmp/cp_loop");
    check("self-interpreting script is ELOOP", r == ELOOP, r);
    syscall1(NR_UNLINK, (int)"/tmp/cp_noexec");
    syscall1(NR_UNLINK, (int)"/tmp/cp_script");
    syscall1(NR_UNLINK, (int)"/tmp/cp_loop");
}

/* ── The set-uid half: starts as ruid 1000, euid 0, suid 0 ─────────────── */

static void getres(int *r, int *e, int *s) {
    syscall3(NR_GETRESUID32, (int)r, (int)e, (int)s);
}

static void setuid_checks(void) {
    int r, e, s;
    getres(&r, &e, &s);
    check("exec of set-uid binary sets euid and suid",
          r == USER_UID && e == 0 && s == 0, e);
    if (e != 0) {
        printf("credprobe: %s is not set-uid root; skipping\n", SUID_BIN);
        return;
    }

    /* Files are created with the effective ids.  Only in the root-owned
     * SUID_DIR, and never through a link: this half runs at euid 0 on behalf
     * of whoever started it. */
    int fd = kopen(SUID_MADE, K_O_WRONLY | K_O_CREAT | K_O_EXCL | K_O_NOFOLLOW);
    struct stat st;
    int sr = (fd >= 0) ? fstat(fd, &st) : fd;
    check("set-uid creates files as euid", sr == 0 && st.st_uid == 0,
          sr == 0 ? (int)st.st_uid : sr);
    if (fd >= 0) close(fd);
    syscall1(NR_UNLINK, (int)SUID_MADE);

    /* A temporary drop keeps the saved id, so it can be undone. */
    int x = syscall3(NR_SETRESUID32, -1, USER_UID, -1);
    getres(&r, &e, &s);
    check("seteuid(ruid) keeps saved uid", x == 0 && e == USER_UID && s == 0, x);
    x = syscall3(NR_SETRESUID32, -1, 0, -1);
    check("seteuid back to saved uid", x == 0, x);

    /* A permanent drop cannot be undone. */
    x = syscall3(NR_SETRESUID32, USER_UID, USER_UID, USER_UID);
    getres(&r, &e, &s);
    check("setresuid(r,r,r) drops all three",
          x == 0 && r == USER_UID && e == USER_UID && s == USER_UID, x);
    x = syscall1(NR_SETUID32, 0);
    check("setuid(0) after drop is EPERM", x == -EPERM, x);
    x = syscall3(NR_SETRESUID32, -1, 0, -1);
    check("seteuid(0) after drop is EPERM", x == -EPERM, x);
    x = syscall3(NR_SETRESGID32, 0, 0, 0);
    check("setresgid(0,0,0) unprivileged is EPERM", x == -EPERM, x);
    getres(&r, &e, &s);
    check("ids unchanged by refused calls",
          r == USER_UID && e == USER_UID && s == USER_UID, e);
}

static void user_checks(void) {
    if (geteuid() != USER_UID) {
        printf("credprobe: not unprivileged (euid %d); skipping\n", geteuid());
        failures++;
        return;
    }
    int r;

    /* Root's file in root's directory. */
    r = syscall1(NR_UNLINK, (int)ROOT_FILE);
    check("unlink root file is EACCES", r == -EACCES, r);
    r = syscall2(NR_RENAME, (int)ROOT_FILE, (int)"/home/cp_moved");
    check("rename root file is EACCES", r == -EACCES, r);
    r = syscall2(NR_TRUNCATE, (int)ROOT_FILE, 0);
    check("truncate root file is EACCES", r == -EACCES, r);
    r = syscall2(NR_RENAME, (int)TMP_DIR_FILE, (int)TMP_ROOT_DIR "/g");
    check("rename in root tmpfs dir is EACCES", r == -EACCES, r);
    r = syscall1(NR_UNLINK, (int)TMP_DIR_FILE);
    check("unlink in root tmpfs dir is EACCES", r == -EACCES, r);
    check("root file untouched", file_has(ROOT_FILE, "root"), 0);

    /* Sticky /tmp: world-writable, yet only your own entries go. */
    r = syscall1(NR_UNLINK, (int)TMP_ROOT_FILE);
    check("unlink other's file in sticky /tmp is EPERM", r == -EPERM, r);
    make_file("/tmp/cp_mine", "mine", 0644, USER_UID, USER_GID);
    r = syscall2(NR_RENAME, (int)"/tmp/cp_mine", (int)TMP_ROOT_FILE);
    check("rename over other's file in sticky /tmp is EPERM", r == -EPERM, r);
    r = syscall1(NR_UNLINK, (int)"/tmp/cp_mine");
    check("unlink own file in sticky /tmp", r == 0, r);

    /* A read-only descriptor stays read-only whatever F_SETFL adds. */
    int fd = kopen(TMP_ROOT_FILE, K_O_RDONLY);
    check("open root file read-only", fd >= 0, fd);
    if (fd >= 0) {
        r = syscall3(NR_WRITE, fd, (int)"x", 1);
        check("write to O_RDONLY fd is EBADF", r == -EBADF, r);
        syscall3(NR_FCNTL, fd, K_F_SETFL, K_O_NONBLOCK);
        r = syscall3(NR_WRITE, fd, (int)"x", 1);
        check("write after F_SETFL O_NONBLOCK is EBADF", r == -EBADF, r);
        syscall3(NR_FCNTL, fd, K_F_SETFL, K_O_APPEND);
        r = syscall3(NR_WRITE, fd, (int)"x", 1);
        check("write after F_SETFL O_APPEND is EBADF", r == -EBADF, r);
        r = syscall4(NR_PWRITE64, fd, (int)"x", 1, 0);
        check("pwrite to O_RDONLY fd is EBADF", r == -EBADF, r);
        r = syscall2(NR_FTRUNCATE, fd, 0);
        check("ftruncate O_RDONLY fd is EINVAL", r == -EINVAL, r);
        /* fallocate(fd, 0, offset 0, len 4096): the 64-bit offset and length
         * are register pairs, len's high word in EBP, which only syscall()
         * sets. */
        r = (int)syscall(NR_FALLOCATE, fd, 0, 0, 0, 4096, 0);
        if (r < 0) r = -errno;
        check("fallocate O_RDONLY fd is EBADF", r == -EBADF, r);

        r = sys_mmap2(4096, K_PROT_READ | K_PROT_WRITE, K_MAP_SHARED, fd);
        check("MAP_SHARED|PROT_WRITE on O_RDONLY fd is EACCES", r == -EACCES, r);
        r = sys_mmap2(4096, K_PROT_READ, K_MAP_SHARED, fd);
        int mapped = !(r < 0 && r > -4096);
        check("MAP_SHARED|PROT_READ on O_RDONLY fd", mapped, r);
        if (mapped) {
            int m = syscall3(NR_MPROTECT, r, 4096, K_PROT_READ | K_PROT_WRITE);
            check("mprotect it writable is EACCES", m == -EACCES, m);
            syscall2(NR_MUNMAP, r, 4096);
        }
        close(fd);
    }
    check("root tmpfs file untouched", file_has(TMP_ROOT_FILE, "sticky"), 0);
    fd = kopen(TMP_ROOT_FILE, K_O_WRONLY);
    check("open root 0644 file for writing is EACCES", fd == -EACCES, fd);
    if (fd >= 0) close(fd);

    /* Other processes. */
    r = syscall2(NR_KILL, 1, 0);
    check("kill(1, 0) is EPERM", r == -EPERM, r);
    r = syscall2(NR_SETPGID, getppid(), getppid());
    check("setpgid on parent is ESRCH", r == -ESRCH, r);
    r = syscall2(NR_SETPGID, 1, 1);
    check("setpgid on init is ESRCH", r == -ESRCH, r);

    /* reboot(2) needs CAP_SYS_BOOT: with valid magic numbers, every command
     * is EPERM for an ordinary user (a failure here ends the run loudly). */
    r = syscall3(NR_REBOOT, (int)0xFEE1DEADu, 672274793, (int)0xCDEF0123u);
    check("reboot(HALT) is EPERM", r == -EPERM, r);
    r = syscall3(NR_REBOOT, (int)0xFEE1DEADu, 672274793, (int)0x4321FEDCu);
    check("reboot(POWER_OFF) is EPERM", r == -EPERM, r);
    r = syscall3(NR_REBOOT, (int)0xFEE1DEADu, 672274793, 0x01234567);
    check("reboot(RESTART) is EPERM", r == -EPERM, r);

    /* A pipeline: the first stage leads the job's group and may have exited
     * (unreaped) before the next stage joins it.  The zombie leader still
     * holds the group, so the join must work. */
    int first = fork();
    if (first == 0) { syscall2(NR_SETPGID, 0, 0); _exit(0); }
    usleep(300000);                          /* let it exit; do not reap */
    int second = fork();
    if (second == 0) { usleep(5000000); _exit(0); }
    r = syscall2(NR_SETPGID, second, first);
    check("setpgid into a zombie leader's group", r == 0, r);
    r = syscall2(NR_KILL, -first, SIGKILL);
    check("kill that group", r == 0, r);
    int pst = 0;
    waitpid(first, &pst, 0);
    waitpid(second, &pst, 0);

    /* ...while one's own children are still fair game. */
    int child = fork();
    if (child == 0) { usleep(5000000); _exit(0); }
    r = syscall2(NR_SETPGID, child, child);
    check("setpgid on own child", r == 0, r);
    r = syscall2(NR_KILL, child, SIGKILL);
    check("kill own child", r == 0, r);
    int st = 0;
    waitpid(child, &st, 0);
}

/* Copy /disk/credprobe to SUID_BIN, owned by root with mode 04755.  SUID_DIR
 * must be new (mkdir fails on anything already there, a link included), so
 * no other user can have a hand in what ends up set-uid. */
static int make_suid_copy(void) {
    if (mkdir(SUID_DIR, 0755) != 0) {
        printf("credprobe: cannot create %s (left over or planted?)\n", SUID_DIR);
        return -1;
    }
    chmod(SUID_DIR, 0755);
    int in = kopen("/disk/credprobe", K_O_RDONLY);
    int out = kopen(SUID_BIN, K_O_WRONLY | K_O_CREAT | K_O_EXCL | K_O_NOFOLLOW);
    int ok = in >= 0 && out >= 0;
    char buf[4096];
    while (ok) {
        int n = read(in, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0 || write(out, buf, n) != n) ok = 0;
    }
    if (in >= 0) close(in);
    if (out >= 0) {
        if (ok && fchmod(out, 04755) != 0) ok = 0;
        close(out);
    }
    if (!ok) printf("credprobe: cannot copy /disk/credprobe to %s\n", SUID_BIN);
    return ok ? 0 : -1;
}

static void remove_suid_copy(void) {
    syscall1(NR_UNLINK, (int)SUID_MADE);
    syscall1(NR_UNLINK, (int)SUID_BIN);
    rmdir(SUID_DIR);
}

static int run_user_half(void) {
    setuid_checks();
    user_checks();
    return failures ? 1 : 0;
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "--user") == 0)
        return run_user_half();

    if (getuid() != 0) {
        printf("credprobe: run me as root\n");
        return 1;
    }

    root_checks();

    /* Fixtures for the unprivileged half. */
    make_file(ROOT_FILE, "root", 0644, 0, 0);
    make_file(TMP_ROOT_FILE, "sticky", 0644, 0, 0);
    mkdir(TMP_ROOT_DIR, 0755);
    chmod(TMP_ROOT_DIR, 0755);
    make_file(TMP_DIR_FILE, "f", 0644, 0, 0);

    /* The shipped program must not be set-uid. */
    struct stat dst;
    int sr = stat("/disk/credprobe", &dst);
    check("/disk/credprobe is not set-uid", sr == 0 && !(dst.st_mode & 06000),
          sr == 0 ? (int)dst.st_mode : sr);

    int made = make_suid_copy();
    check("set-uid copy made", made == 0, made);
    int pid = made == 0 ? fork() : -1;
    if (pid == 0) {
        syscall3(NR_SETRESGID32, USER_GID, USER_GID, USER_GID);
        syscall3(NR_SETRESUID32, USER_UID, USER_UID, USER_UID);
        char *uargv[] = { "credprobe", "--user", (char *)0 };
        char *uenvp[] = { (char *)0 };
        execve(SUID_BIN, uargv, uenvp);
        printf("credprobe: exec of %s FAILED\n", SUID_BIN);
        _exit(1);
    }
    int st = 0;
    int w = pid > 0 ? waitpid(pid, &st, 0) : -1;
    check("unprivileged half passed", w == pid && !WIFSIGNALED(st) &&
          WEXITSTATUS(st) == 0, WEXITSTATUS(st));
    remove_suid_copy();
    sr = stat(SUID_DIR, &dst);
    check("set-uid copy removed", sr != 0, sr);

    syscall1(NR_UNLINK, (int)ROOT_FILE);
    syscall1(NR_UNLINK, (int)TMP_ROOT_FILE);
    syscall1(NR_UNLINK, (int)TMP_DIR_FILE);
    rmdir(TMP_ROOT_DIR);

    if (failures) {
        printf("credprobe: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("credprobe ok\n");
    return 0;
}
