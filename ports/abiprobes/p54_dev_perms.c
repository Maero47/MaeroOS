/*
 * P54 device nodes: who may open them.
 *
 * Linux (udev defaults, devpts gid=5,mode=620): /dev/null, zero, urandom,
 * tty and ptmx are 0666; console devices (input, fb, sound) are 0660 root
 * and their group, disks 0660 root:disk; a pty slave belongs to the user
 * that opened /dev/ptmx, group tty, mode 0620, so another user cannot open
 * it.  root opens anything.
 *
 * MaeroOS: most /dev nodes had mode 0, which the permission check took as
 * 0666: any user read /dev/input/event0 (every keystroke, passwords too),
 * drew on and read /dev/fb0, and opened other users' /dev/pts/N.
 *
 * Runs as root; the checks as another user run in a child that drops to
 * uid/gid 65534.  A node that does not exist here (no framebuffer, no sound
 * card, no input under the Linux reference) is skipped.
 */
#define PROBE_NAME "p54_dev_perms"
#include "probe.h"
#include <grp.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static const char *const open_to_all[] = {
    "/dev/null", "/dev/zero", "/dev/urandom", "/dev/ptmx", 0
};
static const char *const closed[] = {
    "/dev/input/event0", "/dev/input/event1", "/dev/fb0", "/dev/dsp",
    "/dev/snd/controlC0", "/dev/snd/pcmC0D0p", "/dev/hda", "/dev/sda", 0
};

static char pts[64];

/* Exit status: 0 ok, 1.. the index of the failing check (detail printed). */
static int as_nobody(void)
{
    gid_t none[1] = { 65534 };
    if (setgroups(1, none) || setgid(65534) || setuid(65534))
        return 99;
    for (int i = 0; open_to_all[i]; i++) {
        int fd = open(open_to_all[i], O_RDWR | O_NOCTTY);
        if (fd < 0) {
            printf("info p54_dev_perms: %s as nobody: %s, want it open\n",
                   open_to_all[i], strerror(errno));
            return 10 + i;
        }
        close(fd);
    }
    for (int i = 0; closed[i]; i++) {
        struct stat st;
        if (stat(closed[i], &st) != 0) continue;
        int fd = open(closed[i], O_RDONLY | O_NONBLOCK | O_NOCTTY);
        if (fd >= 0 || errno != EACCES) {
            printf("info p54_dev_perms: %s (mode %o uid %u gid %u) as nobody: "
                   "%s, want EACCES\n", closed[i], (unsigned)st.st_mode & 07777,
                   (unsigned)st.st_uid, (unsigned)st.st_gid,
                   fd >= 0 ? "opened" : strerror(errno));
            return 30 + i;
        }
    }
    int fd = open(pts, O_RDWR | O_NOCTTY);
    if (fd >= 0 || errno != EACCES) {
        printf("info p54_dev_perms: root's %s as nobody: %s, want EACCES\n",
               pts, fd >= 0 ? "opened" : strerror(errno));
        return 60;
    }
    /* Its own pty: the slave is its own. */
    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0) return 61;
    unsigned n = 0;
    if (ioctl(m, TIOCGPTN, &n) != 0) return 62;
    int unlock = 0;
    ioctl(m, TIOCSPTLCK, &unlock);
    char own[64];
    snprintf(own, sizeof own, "/dev/pts/%u", n);
    struct stat st;
    if (stat(own, &st) != 0 || st.st_uid != 65534 ||
        (st.st_mode & 0777) != 0620) {
        printf("info p54_dev_perms: own %s uid %u mode %o, want 65534 and 620\n",
               own, (unsigned)st.st_uid, (unsigned)st.st_mode & 0777);
        return 63;
    }
    int s = open(own, O_RDWR | O_NOCTTY);
    if (s < 0) {
        printf("info p54_dev_perms: own %s: %s\n", own, strerror(errno));
        return 64;
    }
    return 0;
}

int main(void)
{
    probe_watchdog(60);
    if (geteuid() != 0) probe_skip("needs root");

    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0) probe_fail("open /dev/ptmx as root: %s", strerror(errno));
    unsigned n = 0;
    if (ioctl(m, TIOCGPTN, &n) != 0) probe_fail("TIOCGPTN: %s", strerror(errno));
    int unlock = 0;
    ioctl(m, TIOCSPTLCK, &unlock);
    snprintf(pts, sizeof pts, "/dev/pts/%u", n);
    struct stat st;
    if (stat(pts, &st) != 0) probe_fail("stat %s: %s", pts, strerror(errno));
    if (st.st_uid != 0 || (st.st_mode & 0777) != 0620)
        probe_fail("root's %s uid %u mode %o, want 0 and 620", pts,
                   (unsigned)st.st_uid, (unsigned)st.st_mode & 0777);

    pid_t pid = fork();
    if (pid == 0) {
        fflush(stdout);
        _exit(as_nobody());
    }
    int ws = 0;
    if (waitpid(pid, &ws, 0) != pid) probe_fail("waitpid: %s", strerror(errno));
    if (!WIFEXITED(ws) || WEXITSTATUS(ws) != 0)
        probe_fail("check as nobody failed (status %d), see the info line",
                   WIFEXITED(ws) ? WEXITSTATUS(ws) : -1);

    /* root still opens the console devices. */
    for (int i = 0; closed[i]; i++) {
        if (stat(closed[i], &st) != 0) continue;
        if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode)) continue;
        int fd = open(closed[i], O_RDONLY | O_NONBLOCK | O_NOCTTY);
        if (fd < 0)
            probe_fail("%s as root: %s", closed[i], strerror(errno));
        close(fd);
    }
    probe_pass();
}
