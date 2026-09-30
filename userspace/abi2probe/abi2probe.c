/*
 * abi2probe — Linux-ABI regressions for creation modes, O_APPEND, write
 * offsets, supplementary groups, access(), TIOCSPGRP, unlink(dir) and TCP
 * socket flags.
 *
 *   abi2probe                     file/credential cases (run as root;
 *                                 tools/smoke_toybox.py)
 *   abi2probe net <idle> <reset>  TCP cases against two host ports
 *                                 (tools/smoke_net.py): <idle> accepts and
 *                                 stays silent, <reset> accepts and closes
 *                                 with a reset
 *
 * Prints "abi2probe: <case> ok" per case, "abi2probe: <case> FAIL ..." on a
 * failure, and "abi2probe ok" / "abi2probe net ok" when every case passed.
 */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/syscall.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"
#include "../include/errno.h"
#include "../include/signal.h"
#include "../include/grp.h"
#include "../include/poll.h"
#include "../include/sys/stat.h"
#include "../include/sys/wait.h"
#include "../include/sys/socket.h"
#include "../include/netinet/in.h"
#include "../include/arpa/inet.h"

#define NR_CREAT        8
#define NR_OPEN         5
#define NR_WRITE        4
#define NR_UNLINK       10
#define NR_MKNOD        14
#define NR_ACCESS       33
#define NR_MKDIR        39
#define NR_IOCTL        54
#define NR_GETGROUPS16  80
#define NR_SETGROUPS16  81
#define NR_FCHOWN32     207
#define NR_CHOWN32      212
#define NR_GETGROUPS32  205
#define NR_SETGROUPS32  206
#define NR_SETRESUID32  208
#define NR_SETRESGID32  210
#define NR_OPENAT       295
#define NR_FACCESSAT2   439

#define K_AT_FDCWD      (-100)
#define K_AT_EACCESS    0x200
#define K_TIOCSPGRP     0x5410

#define GRP_SUPP   500     /* a supplementary group only the test user holds */
#define USER_UID   1000
#define USER_GID   100

static int failures;

static void check(const char *name, int ok, int got) {
    if (ok) printf("abi2probe: %s ok\n", name);
    else  { printf("abi2probe: %s FAIL (got %d)\n", name, got); failures++; }
}

static int file_mode(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int)(st.st_mode & 07777) : -1;
}

static int file_gid(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int)st.st_gid : -1;
}

/* Create `path` with `text`, owned root:gid with `mode`. */
static void make_file(const char *path, const char *text, int mode, int gid) {
    syscall1(NR_UNLINK, (int)path);
    int fd = syscall3(NR_OPEN, (int)path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { printf("abi2probe: cannot create %s (%d)\n", path, fd); return; }
    syscall3(NR_WRITE, fd, (int)text, (int)strlen(text));
    syscall3(NR_FCHOWN32, fd, 0, gid);
    close(fd);
    chmod(path, mode);
}

/* Run fn in a child that has become uid 1000 / gid 100 with the given
 * supplementary groups; the child's exit status is its failure count. */
static int as_user(const unsigned *groups, int ngroups, int keep_euid0,
                   int (*fn)(void)) {
    int pid = fork();
    if (pid == 0) {
        int r = syscall2(NR_SETGROUPS32, ngroups, (int)groups);
        if (r == 0) r = syscall3(NR_SETRESGID32, USER_GID, USER_GID, USER_GID);
        if (r == 0) r = keep_euid0
            ? syscall3(NR_SETRESUID32, USER_UID, 0, 0)
            : syscall3(NR_SETRESUID32, USER_UID, USER_UID, USER_UID);
        if (r != 0) { printf("abi2probe: dropping ids failed (%d)\n", r); exit(1); }
        exit(fn());
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid) return 1;
    return (st & 0x7f) ? 1 : ((st >> 8) & 0xff);
}

/* ── creation modes ──────────────────────────────────────────────────────── */
static void creation_modes(void) {
    umask(022);
    syscall1(NR_UNLINK, (int)"/tmp/a2_mode");
    int fd = syscall3(NR_OPEN, (int)"/tmp/a2_mode", O_CREAT | O_WRONLY | O_EXCL, 0640);
    check("open(O_CREAT, 0640) under umask 022 is 0640",
          fd >= 0 && file_mode("/tmp/a2_mode") == 0640, file_mode("/tmp/a2_mode"));
    if (fd >= 0) close(fd);
    fd = syscall3(NR_OPEN, (int)"/tmp/a2_mode", O_CREAT | O_WRONLY | O_EXCL, 0640);
    check("O_CREAT|O_EXCL on an existing file is EEXIST", fd == -EEXIST, fd);

    syscall1(NR_UNLINK, (int)"/tmp/a2_creat");
    fd = syscall2(NR_CREAT, (int)"/tmp/a2_creat", 0777);
    check("creat(0777) under umask 022 is 0755",
          fd >= 0 && file_mode("/tmp/a2_creat") == 0755, file_mode("/tmp/a2_creat"));
    if (fd >= 0) close(fd);

    syscall1(NR_UNLINK, (int)"/tmp/a2_at");
    fd = syscall4(NR_OPENAT, K_AT_FDCWD, (int)"/tmp/a2_at", O_CREAT | O_RDWR, 0604);
    check("openat(O_CREAT, 0604) is 0604",
          fd >= 0 && file_mode("/tmp/a2_at") == 0604, file_mode("/tmp/a2_at"));
    if (fd >= 0) close(fd);

    /* A file created read-only by this very open is still writable through
     * the descriptor the open returns. */
    syscall1(NR_UNLINK, (int)"/tmp/a2_ro");
    fd = syscall3(NR_OPEN, (int)"/tmp/a2_ro", O_CREAT | O_RDWR, 0444);
    int w = fd >= 0 ? syscall3(NR_WRITE, fd, (int)"x", 1) : fd;
    check("open(O_CREAT|O_RDWR, 0444) returns a writable fd",
          w == 1 && file_mode("/tmp/a2_ro") == 0444, w);
    if (fd >= 0) close(fd);

    rmdir("/tmp/a2_dir");
    int r = syscall2(NR_MKDIR, (int)"/tmp/a2_dir", 0750);
    check("mkdir(0750) is 0750", r == 0 && file_mode("/tmp/a2_dir") == 0750,
          file_mode("/tmp/a2_dir"));
    rmdir("/tmp/a2_dir");

    syscall1(NR_UNLINK, (int)"/tmp/a2_fifo");
    r = syscall3(NR_MKNOD, (int)"/tmp/a2_fifo", 010662, 0);
    check("mknod(S_IFIFO|0662) under umask 022 is 0640",
          r == 0 && file_mode("/tmp/a2_fifo") == 0640, file_mode("/tmp/a2_fifo"));
    syscall1(NR_UNLINK, (int)"/tmp/a2_fifo");

    umask(077);
    syscall1(NR_UNLINK, (int)"/tmp/a2_mode");
    fd = syscall3(NR_OPEN, (int)"/tmp/a2_mode", O_CREAT | O_WRONLY, 0666);
    check("open(O_CREAT, 0666) under umask 077 is 0600",
          fd >= 0 && file_mode("/tmp/a2_mode") == 0600, file_mode("/tmp/a2_mode"));
    if (fd >= 0) close(fd);
    umask(022);

    /* A set-group-ID directory hands its group to new entries, and its
     * set-group-ID bit to new subdirectories. */
    rmdir("/tmp/a2_sgid/sub");
    syscall1(NR_UNLINK, (int)"/tmp/a2_sgid/f");
    rmdir("/tmp/a2_sgid");
    mkdir("/tmp/a2_sgid", 0755);
    syscall3(NR_CHOWN32, (int)"/tmp/a2_sgid", 0, GRP_SUPP);
    chmod("/tmp/a2_sgid", 02775);
    fd = syscall3(NR_OPEN, (int)"/tmp/a2_sgid/f", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    mkdir("/tmp/a2_sgid/sub", 0755);
    check("file in a set-gid dir takes the dir's group",
          file_gid("/tmp/a2_sgid/f") == GRP_SUPP, file_gid("/tmp/a2_sgid/f"));
    check("subdir of a set-gid dir is set-gid",
          file_gid("/tmp/a2_sgid/sub") == GRP_SUPP &&
          (file_mode("/tmp/a2_sgid/sub") & 02000), file_mode("/tmp/a2_sgid/sub"));
    rmdir("/tmp/a2_sgid/sub");
    syscall1(NR_UNLINK, (int)"/tmp/a2_sgid/f");
    rmdir("/tmp/a2_sgid");
}

/* ── O_APPEND, write offsets ─────────────────────────────────────────────── */
static void append_and_offsets(void) {
    char buf[16];
    make_file("/tmp/a2_app", "abc", 0644, 0);
    int fd = open("/tmp/a2_app", O_WRONLY | O_APPEND);
    int fl = fd >= 0 ? fcntl(fd, F_GETFL) : -1;
    check("F_GETFL reports O_APPEND given at open", fl >= 0 && (fl & O_APPEND), fl);
    lseek(fd, 0, SEEK_SET);
    write(fd, "de", 2);
    close(fd);
    fd = open("/tmp/a2_app", O_RDONLY);
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    check("O_APPEND at open appends after lseek(0)", strcmp(buf, "abcde") == 0, n);

    /* A failed write must not move the offset. */
    fd = open("/tmp/a2_app", O_RDWR);
    lseek(fd, 2, SEEK_SET);
    int r = syscall3(NR_WRITE, fd, 0x10, 4);            /* bad buffer */
    int off = lseek(fd, 0, SEEK_CUR);
    check("failed write leaves the offset alone", r < 0 && off == 2, off);
    close(fd);
    syscall1(NR_UNLINK, (int)"/tmp/a2_app");
}

/* ── unlink of a directory ───────────────────────────────────────────────── */
static void unlink_dir(void) {
    rmdir("/tmp/a2_ud");
    mkdir("/tmp/a2_ud", 0755);
    int r = syscall1(NR_UNLINK, (int)"/tmp/a2_ud");
    check("unlink(dir) is EISDIR", r == -EISDIR, r);
    check("rmdir removes it", rmdir("/tmp/a2_ud") == 0, errno);
}

/* ── supplementary groups and access() ───────────────────────────────────── */
static int child_in_group(void) {
    int bad = 0;
    unsigned g[4] = {0};
    int n = syscall2(NR_GETGROUPS32, 4, (int)g);
    if (n != 1 || g[0] != GRP_SUPP) { printf("abi2probe: groups not kept (%d)\n", n); bad++; }
    int fd = syscall3(NR_OPEN, (int)"/tmp/a2_grp", O_RDONLY, 0);
    if (fd < 0) { printf("abi2probe: group-readable open failed (%d)\n", fd); bad++; }
    else close(fd);
    int r = syscall2(NR_SETGROUPS32, 0, 0);
    if (r != -EPERM) { printf("abi2probe: setgroups as user got %d\n", r); bad++; }
    r = syscall2(NR_SETGROUPS16, 0, 0);
    if (r != -EPERM) { printf("abi2probe: setgroups16 as user got %d\n", r); bad++; }
    r = syscall2(NR_ACCESS, (int)"/tmp/a2_root", R_OK);
    if (r != -EACCES) { printf("abi2probe: access(R_OK) 0600 root got %d\n", r); bad++; }
    r = syscall2(NR_ACCESS, (int)"/tmp/a2_root", F_OK);
    if (r != 0) { printf("abi2probe: access(F_OK) got %d\n", r); bad++; }
    r = syscall2(NR_ACCESS, (int)"/tmp/a2_grp", R_OK);
    if (r != 0) { printf("abi2probe: access(R_OK) via group got %d\n", r); bad++; }
    r = syscall2(NR_ACCESS, (int)"/tmp/a2_grp", W_OK);
    if (r != -EACCES) { printf("abi2probe: access(W_OK) 0640 got %d\n", r); bad++; }
    /* chmod g+s by an owner outside the file's group drops the bit. */
    chmod("/tmp/a2_mine", 02755);
    if ((file_mode("/tmp/a2_mine") & 02000) == 0) {
        printf("abi2probe: member's chmod g+s was dropped\n"); bad++;
    }
    return bad;
}

static int child_no_group(void) {
    int bad = 0;
    int fd = syscall3(NR_OPEN, (int)"/tmp/a2_grp", O_RDONLY, 0);
    if (fd != -EACCES) { printf("abi2probe: non-member open got %d\n", fd); bad++; }
    if (fd >= 0) close(fd);
    unsigned short g16[2];
    int n = syscall2(NR_GETGROUPS16, 2, (int)g16);
    if (n != 0) { printf("abi2probe: empty group list reads %d\n", n); bad++; }
    chmod("/tmp/a2_mine", 02755);
    if (file_mode("/tmp/a2_mine") != 0755) {
        printf("abi2probe: non-member chmod g+s kept %o\n", file_mode("/tmp/a2_mine"));
        bad++;
    }
    return bad;
}

/* ruid 1000, euid 0: access() answers for the real user, open() for the
 * effective one, and AT_EACCESS switches access to the effective ids. */
static int child_setuid(void) {
    int bad = 0;
    int r = syscall2(NR_ACCESS, (int)"/tmp/a2_root", R_OK);
    if (r != -EACCES) { printf("abi2probe: access with ruid 1000 got %d\n", r); bad++; }
    r = syscall4(NR_FACCESSAT2, K_AT_FDCWD, (int)"/tmp/a2_root", R_OK, K_AT_EACCESS);
    if (r != 0) { printf("abi2probe: faccessat2(AT_EACCESS) got %d\n", r); bad++; }
    int fd = syscall3(NR_OPEN, (int)"/tmp/a2_root", O_RDONLY, 0);
    if (fd < 0) { printf("abi2probe: open with euid 0 got %d\n", fd); bad++; }
    else close(fd);
    return bad;
}

static void groups_and_access(void) {
    unsigned g[2] = { GRP_SUPP, 0 };
    int r = syscall2(NR_SETGROUPS32, 1, (int)g);
    unsigned got[4] = {0};
    int n = syscall2(NR_GETGROUPS32, 0, 0);
    int n2 = syscall2(NR_GETGROUPS32, 4, (int)got);
    check("setgroups/getgroups as root", r == 0 && n == 1 && n2 == 1 && got[0] == GRP_SUPP, n2);
    unsigned two[2] = { GRP_SUPP, GRP_SUPP + 1 };
    syscall2(NR_SETGROUPS32, 2, (int)two);
    r = syscall2(NR_GETGROUPS32, 1, (int)got);
    check("getgroups into a too-small list is EINVAL", r == -EINVAL, r);
    r = syscall2(NR_SETGROUPS32, 99, (int)two);
    check("setgroups past the limit is EINVAL", r == -EINVAL, r);
    syscall2(NR_SETGROUPS32, 1, (int)g);

    make_file("/tmp/a2_grp", "grp\n", 0640, GRP_SUPP);
    make_file("/tmp/a2_root", "root\n", 0600, 0);
    make_file("/tmp/a2_mine", "mine\n", 0755, GRP_SUPP);
    syscall3(NR_CHOWN32, (int)"/tmp/a2_mine", USER_UID, GRP_SUPP);

    unsigned supp[1] = { GRP_SUPP };
    check("supplementary-group member reads a 0640 file; access() uses mode bits; "
          "setgroups as non-root is EPERM", as_user(supp, 1, 0, child_in_group) == 0, 0);
    chmod("/tmp/a2_mine", 0755);
    check("non-member is refused; chmod g+s dropped", as_user(supp, 0, 0, child_no_group) == 0, 0);
    check("access() checks the real ids, AT_EACCESS the effective",
          as_user(supp, 0, 1, child_setuid) == 0, 0);

    syscall1(NR_UNLINK, (int)"/tmp/a2_grp");
    syscall1(NR_UNLINK, (int)"/tmp/a2_root");
    syscall1(NR_UNLINK, (int)"/tmp/a2_mine");
    syscall2(NR_SETGROUPS32, 0, 0);
}

/* ── libc: /etc/group, initgroups ────────────────────────────────────────── */
static void libc_groups(void) {
    struct group *gr = getgrnam("users");
    check("getgrnam(users) is gid 100 with member user",
          gr && gr->gr_gid == 100 && gr->gr_mem && gr->gr_mem[0] &&
          strcmp(gr->gr_mem[0], "user") == 0 && !gr->gr_mem[1], gr ? (int)gr->gr_gid : -1);
    gr = getgrgid(0);
    check("getgrgid(0) is root with no members",
          gr && strcmp(gr->gr_name, "root") == 0 && gr->gr_mem && !gr->gr_mem[0], 0);
    check("getgrnam of a missing group is NULL", getgrnam("nosuchgroup") == 0, 0);

    int pid = fork();
    if (pid == 0) {
        gid_t list[8];
        int r = initgroups("user", 100);
        int n = getgroups(8, list);
        exit(r == 0 && n == 1 && list[0] == 100 ? 0 : 1);
    }
    int st = 1;
    waitpid(pid, &st, 0);
    check("initgroups(user, 100) sets the group list", st == 0, st);
}

/* ── TIOCSPGRP ───────────────────────────────────────────────────────────── */
static void tiocspgrp(void) {
    int pg = getpgrp();
    int r = syscall3(NR_IOCTL, 0, K_TIOCSPGRP, (int)&pg);
    check("TIOCSPGRP to our own group on our terminal", r == 0, r);
    int none = 30000;
    r = syscall3(NR_IOCTL, 0, K_TIOCSPGRP, (int)&none);
    check("TIOCSPGRP to a missing group is ESRCH", r == -ESRCH, r);

    int ready[2];
    pipe(ready);
    int pid = fork();
    if (pid == 0) {
        close(ready[0]);
        setsid();                              /* another session */
        int me = getpgrp();
        int rr = syscall3(NR_IOCTL, 0, K_TIOCSPGRP, (int)&me);
        write(ready[1], &rr, sizeof(rr));
        usleep(300000);                        /* stay alive for the parent */
        exit(0);
    }
    close(ready[1]);
    int child_r = 1;
    read(ready[0], &child_r, sizeof(child_r));
    close(ready[0]);
    check("TIOCSPGRP from another session is ENOTTY", child_r == -ENOTTY, child_r);
    int other = pid;                           /* the child's group */
    r = syscall3(NR_IOCTL, 0, K_TIOCSPGRP, (int)&other);
    check("TIOCSPGRP to another session's group is EPERM", r == -EPERM, r);
    int st;
    waitpid(pid, &st, 0);
    syscall3(NR_IOCTL, 0, K_TIOCSPGRP, (int)&pg);
}

/* ── TCP ─────────────────────────────────────────────────────────────────── */
static volatile int sigpipes;
static void on_sigpipe(int sig) { (void)sig; sigpipes++; }

static int tcp_connect_to(int port, int type_flags) {
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM | type_flags, 0);
    if (fd < 0) return -errno;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = inet_addr("10.0.2.2");
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

/* Keep sending into a connection the peer has reset until the socket
 * reports it: at most one ECONNRESET, then EPIPE.  Returns the final errno
 * (EPIPE expected) or 0 if the error never came. */
static int send_until_epipe(int fd, int flags) {
    for (int i = 0; i < 200; i++) {
        int r = send(fd, "ping", 4, flags);
        if (r < 0 && errno == EPIPE) return EPIPE;
        if (r < 0 && errno != ECONNRESET) return errno;
        usleep(20000);
    }
    return 0;
}

static int net_main(int idle_port, int reset_port) {
    char buf[64];
    signal(SIGPIPE, on_sigpipe);

    /* Non-blocking connect: EINPROGRESS, then writable with SO_ERROR 0. */
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)idle_port);
    sa.sin_addr.s_addr = inet_addr("10.0.2.2");
    int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    check("non-blocking connect is EINPROGRESS", r < 0 && errno == EINPROGRESS, errno);
    struct pollfd pfd = { fd, POLLOUT, 0 };
    int pr = poll(&pfd, 1, 5000);
    int soerr = -1;
    socklen_t sl = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    check("connect completes: POLLOUT and SO_ERROR 0", pr == 1 && soerr == 0, soerr);
    r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    check("connect again is EISCONN", r < 0 && errno == EISCONN, errno);

    /* Nothing sent by the peer: a non-blocking recv is EAGAIN. */
    r = recv(fd, buf, sizeof(buf), 0);
    check("O_NONBLOCK recv on an empty socket is EAGAIN", r < 0 && errno == EAGAIN, errno);
    close(fd);

    fd = tcp_connect_to(idle_port, 0);
    r = fd >= 0 ? recv(fd, buf, sizeof(buf), MSG_DONTWAIT) : fd;
    check("MSG_DONTWAIT recv on an empty socket is EAGAIN", r < 0 && errno == EAGAIN, r);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    r = read(fd, buf, sizeof(buf));
    check("read() on an F_SETFL O_NONBLOCK socket is EAGAIN", r < 0 && errno == EAGAIN, r);
    if (fd >= 0) close(fd);

    /* Peer resets the connection: send ends in EPIPE with SIGPIPE... */
    fd = tcp_connect_to(reset_port, 0);
    sigpipes = 0;
    int e = fd >= 0 ? send_until_epipe(fd, 0) : -fd;
    check("send after the peer closed is EPIPE", e == EPIPE, e);
    check("... and raises SIGPIPE", sigpipes >= 1, sigpipes);
    int before = sigpipes;
    r = write(fd, "x", 1);
    check("write() on it is EPIPE and raises SIGPIPE",
          r < 0 && errno == EPIPE && sigpipes == before + 1, errno);
    r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    check("recv after the reset reports no data", r <= 0, r);
    if (fd >= 0) close(fd);

    /* ...but not with MSG_NOSIGNAL. */
    fd = tcp_connect_to(reset_port, 0);
    sigpipes = 0;
    e = fd >= 0 ? send_until_epipe(fd, MSG_NOSIGNAL) : -fd;
    check("MSG_NOSIGNAL send after the peer closed is EPIPE", e == EPIPE, e);
    check("... without SIGPIPE", sigpipes == 0, sigpipes);
    if (fd >= 0) close(fd);

    if (failures) { printf("abi2probe net: %d FAILED\n", failures); return 1; }
    printf("abi2probe net ok\n");
    return 0;
}

/* abi2probe tty: run from a shell that replaced one which exited while a
 * background job of its session lived on.  The console must have been freed
 * by the old leader's exit, so the new shell owns it and made this probe's
 * group the foreground one. */
#define K_TIOCGPGRP 0x540F
static int tty_main(void) {
    int fg = -1, me = getpgrp();
    syscall3(NR_IOCTL, 0, K_TIOCGPGRP, (int)&fg);
    check("new login's job is the console's foreground group", fg == me, fg);
    int r = syscall3(NR_IOCTL, 0, K_TIOCSPGRP, (int)&me);
    check("TIOCSPGRP works for the new session", r == 0, r);
    if (failures) { printf("abi2probe tty: %d FAILED\n", failures); return 1; }
    printf("abi2probe tty ok\n");
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "tty") == 0)
        return tty_main();
    if (argc >= 4 && strcmp(argv[1], "net") == 0)
        return net_main(atoi(argv[2]), atoi(argv[3]));
    if (getuid() != 0) { printf("abi2probe: run as root\n"); return 1; }

    creation_modes();
    append_and_offsets();
    unlink_dir();
    groups_and_access();
    libc_groups();
    tiocspgrp();

    if (failures) { printf("abi2probe: %d FAILED\n", failures); return 1; }
    printf("abi2probe ok\n");
    return 0;
}
