/*
 * mntprobe — mount(2)/umount2(2) cases busybox cannot reach.
 *
 *   mntprobe <block device>   (run as root; the device holds an ext2/3/4
 *                              filesystem with a directory /dir)
 *
 *   - A bind mount pins the mount its source lives on: umount of the ext4
 *     mount is EBUSY while a bind of one of its directories (or of its root)
 *     exists, and the bind keeps reading after the attempt.
 *   - umount picks the mount the path names, not the one in the highest table
 *     slot: a bind of the ext4 root made into a lower, reused slot is the one
 *     `umount <bind>` removes, and the ext4 mount stays.
 *   - Descriptor-based changes on a read-only mount are EROFS: fchmod and
 *     fchown on the ext4 mount and on a tmpfs remounted read-only; remount,ro
 *     is EBUSY while a file of the mount is open for writing.
 *
 * Prints "mntprobe ok" when everything holds, "mntprobe FAIL: ..." otherwise.
 */
#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/syscall.h"
#include "../include/sys/stat.h"
#include "../include/unistd.h"

#define MS_RDONLY  1
#define MS_REMOUNT 0x20
#define MS_BIND    0x1000
#define EBUSY      16
#define EROFS      30

static int fails;

static void expect(int got, int want, const char *what) {
    if (got != want) {
        printf("mntprobe FAIL: %s (got %d, want %d)\n", what, got, want);
        fails++;
    }
}

static int sys_mount(const char *src, const char *tgt, const char *type, int flags) {
    return syscall5(21, (int)src, (int)tgt, (int)type, flags, 0);
}

static int sys_umount(const char *tgt) {
    return syscall2(52, (int)tgt, 0);
}

static int read_first(const char *path, char *buf, int n) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    int r = (int)read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = '\0';
    return r;
}

static int mounted_at(const char *target) {
    char buf[2048], pat[300];
    if (read_first("/proc/mounts", buf, sizeof(buf)) < 0) return 0;
    snprintf(pat, sizeof(pat), " %s ", target);
    return strstr(buf, pat) != NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: mntprobe <block device>\n");
        return 2;
    }
    const char *dev = argv[1];
    char buf[64];
    mkdir("/mnt", 0755);
    mkdir("/tmp/mp_bind", 0755);
    mkdir("/tmp/mp_x", 0755);
    mkdir("/tmp/mp_ro", 0755);

    /* 1. A bind of a directory inside the ext4 mount pins it. */
    expect(sys_mount(dev, "/mnt", "ext4", MS_RDONLY), 0, "mount ext4 on /mnt");
    expect(sys_mount("/mnt/dir", "/tmp/mp_bind", 0, MS_BIND), 0, "bind /mnt/dir");
    expect(sys_umount("/mnt"), -EBUSY, "umount /mnt while bound elsewhere");
    expect(read_first("/tmp/mp_bind/e0001", buf, sizeof(buf)) > 0 &&
           strcmp(buf, "1\n") == 0, 1, "bind still reads after the refused umount");
    expect(sys_umount("/tmp/mp_bind"), 0, "umount the bind");
    expect(sys_umount("/mnt"), 0, "umount /mnt once the bind is gone");

    /* 2. Slot reuse: the bind lands in a lower slot than the ext4 mount. */
    expect(sys_mount("none", "/tmp/mp_x", "tmpfs", 0), 0, "tmpfs on /tmp/mp_x");
    expect(sys_mount(dev, "/mnt", "ext4", MS_RDONLY), 0, "mount ext4 again");
    expect(sys_umount("/tmp/mp_x"), 0, "free the lower slot");
    expect(sys_mount("/mnt", "/tmp/mp_bind", 0, MS_BIND), 0, "bind the ext4 root");
    expect(sys_umount("/mnt"), -EBUSY, "umount /mnt while its root is bound");
    expect(sys_umount("/tmp/mp_bind"), 0, "umount the bind of the root");
    expect(mounted_at("/mnt"), 1, "the ext4 mount survived umount of the bind");
    expect(mounted_at("/tmp/mp_bind"), 0, "the bind is gone");
    expect(read_first("/mnt/dir/e0002", buf, sizeof(buf)) > 0 &&
           strcmp(buf, "2\n") == 0, 1, "ext4 still readable");

    /* 3. fchmod/fchown through a descriptor on the read-only ext4 mount. */
    int fd = open("/mnt/dir/e0003", O_RDONLY, 0);
    expect(fd >= 0, 1, "open a file on the ext4 mount");
    struct stat st0, st1;
    fstat(fd, &st0);
    expect(syscall2(94, fd, 0777), -EROFS, "fchmod on ext4");
    expect(syscall3(207, fd, 1000, 100), -EROFS, "fchown on ext4");
    fstat(fd, &st1);
    expect(st1.st_mode == st0.st_mode && st1.st_uid == st0.st_uid, 1,
           "ext4 file unchanged");
    close(fd);
    expect(sys_umount("/mnt"), 0, "umount ext4");

    /* 4. tmpfs remounted read-only. */
    expect(sys_mount("none", "/tmp/mp_ro", "tmpfs", 0), 0, "tmpfs on /tmp/mp_ro");
    int wfd = open("/tmp/mp_ro/f", O_RDWR | O_CREAT, 0644);
    expect(wfd >= 0, 1, "create a file on the tmpfs");
    expect(sys_mount(0, "/tmp/mp_ro", 0, MS_REMOUNT | MS_RDONLY), -EBUSY,
           "remount,ro while a file is open for writing");
    close(wfd);
    fd = open("/tmp/mp_ro/f", O_RDONLY, 0);
    expect(sys_mount(0, "/tmp/mp_ro", 0, MS_REMOUNT | MS_RDONLY), 0,
           "remount,ro with only readers");
    expect(syscall2(94, fd, 0600), -EROFS, "fchmod on the read-only tmpfs");
    expect(syscall3(207, fd, 1000, 100), -EROFS, "fchown on the read-only tmpfs");
    fstat(fd, &st1);
    expect((int)(st1.st_mode & 0777), 0644, "tmpfs file mode unchanged");
    expect((int)st1.st_uid, 0, "tmpfs file owner unchanged");
    expect(sys_mount(0, "/tmp/mp_ro", 0, MS_REMOUNT), 0, "remount,rw");
    expect(syscall2(94, fd, 0600), 0, "fchmod works again after remount,rw");
    close(fd);
    expect(sys_umount("/tmp/mp_ro"), 0, "umount the tmpfs");

    if (fails) return 1;
    printf("mntprobe ok\n");
    return 0;
}
