/*
 * P46 inotify: the events of each directory operation, on the filesystem of
 * the directory given as argv[1] (default /tmp).
 *
 * Linux: a watch on a directory reports IN_CREATE, IN_MODIFY, IN_CLOSE_WRITE,
 * IN_ATTRIB, IN_MOVED_FROM/IN_MOVED_TO (one cookie), IN_DELETE, and
 * IN_CREATE|IN_ISDIR / IN_DELETE|IN_ISDIR for a subdirectory, each with the
 * entry's name; a watch on a file reports IN_MODIFY, IN_ATTRIB and
 * IN_DELETE_SELF then IN_IGNORED when it is unlinked.  An empty non-blocking
 * instance reads EAGAIN; poll() and epoll see an event; FIONREAD counts its
 * bytes; a blocking read() waits for the next event; inotify_rm_watch
 * queues IN_IGNORED; past max_queued_events the queue ends in one
 * IN_Q_OVERFLOW (checked on tmpfs only: it takes thousands of operations).
 *
 * MaeroOS: inotify_init was ENOSYS.
 */
#define PROBE_NAME "p46_inotify"
#include "probe.h"
#include <poll.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static char buf[65536] __attribute__((aligned(8)));
static int blen, bpos;

static int in_fd;

/* The next event, reading more (non-blocking) as needed; NULL if none. */
static struct inotify_event *next_event(void)
{
    if (bpos >= blen) {
        blen = (int)read(in_fd, buf, sizeof buf);
        bpos = 0;
        if (blen <= 0) { blen = 0; return NULL; }
    }
    struct inotify_event *e = (struct inotify_event *)(buf + bpos);
    bpos += (int)sizeof(*e) + (int)e->len;
    return e;
}

static struct inotify_event *expect(int wd, uint32_t mask, const char *name, const char *what)
{
    struct inotify_event *e = next_event();
    if (!e) probe_fail("%s: no event (want mask %#x %s)", what, mask, name ? name : "");
    if (e->wd != wd || e->mask != mask)
        probe_fail("%s: got wd %d mask %#x \"%s\", want wd %d mask %#x", what, e->wd, e->mask,
                   e->len ? e->name : "", wd, mask);
    if (name && (!e->len || strcmp(e->name, name)))
        probe_fail("%s: name \"%s\", want \"%s\"", what, e->len ? e->name : "", name);
    if (!name && e->len) probe_fail("%s: unexpected name \"%s\"", what, e->name);
    return e;
}

static void expect_none(const char *what)
{
    struct inotify_event *e = next_event();
    if (e) probe_fail("%s: unexpected event wd %d mask %#x \"%s\"", what, e->wd, e->mask,
                      e->len ? e->name : "");
}

int main(int argc, char **argv)
{
    probe_watchdog(120);
    const char *base = argc > 1 ? argv[1] : "/tmp";
    char dir[256], path[300], path2[300], sub[300];
    snprintf(dir, sizeof dir, "%s/p46.%d", base, (int)getpid());
    if (mkdir(dir, 0755) != 0) probe_skip("mkdir %s: %s", dir, strerror(errno));
    snprintf(path, sizeof path, "%s/a", dir);
    snprintf(path2, sizeof path2, "%s/b", dir);
    snprintf(sub, sizeof sub, "%s/sub", dir);

    in_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (in_fd < 0) probe_fail("inotify_init1: %s", strerror(errno));
    uint32_t all = IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO |
                   IN_CLOSE_WRITE | IN_ATTRIB;
    int wd = inotify_add_watch(in_fd, dir, all);
    if (wd < 0) probe_fail("inotify_add_watch: %s", strerror(errno));
    char tmp[16];
    if (read(in_fd, tmp, sizeof tmp) >= 0 || errno != EAGAIN)
        probe_fail("empty non-blocking instance: want EAGAIN");

    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) probe_fail("create: %s", strerror(errno));
    struct pollfd pfd = { in_fd, POLLIN, 0 };
    if (poll(&pfd, 1, 1000) != 1 || !(pfd.revents & POLLIN)) probe_fail("poll: no POLLIN after a create");
    int avail = 0;
    if (ioctl(in_fd, FIONREAD, &avail) != 0 || avail != (int)sizeof(struct inotify_event) + 16)
        probe_fail("FIONREAD: %d", avail);
    expect(wd, IN_CREATE, "a", "create");
    if (write(fd, "hello", 5) != 5) probe_fail("write: %s", strerror(errno));
    expect(wd, IN_MODIFY, "a", "write");
    close(fd);
    expect(wd, IN_CLOSE_WRITE, "a", "close");
    if (chmod(path, 0600) != 0) probe_fail("chmod: %s", strerror(errno));
    expect(wd, IN_ATTRIB, "a", "chmod");
    if (rename(path, path2) != 0) probe_fail("rename: %s", strerror(errno));
    struct inotify_event *from = expect(wd, IN_MOVED_FROM, "a", "rename (from)");
    uint32_t cookie = from->cookie;
    struct inotify_event *to = expect(wd, IN_MOVED_TO, "b", "rename (to)");
    if (!cookie || to->cookie != cookie) probe_fail("rename cookies %u/%u", cookie, to->cookie);

    /* A watch on the file itself. */
    int fwd = inotify_add_watch(in_fd, path2, IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF);
    if (fwd < 0 || fwd == wd) probe_fail("file watch: %d %s", fwd, strerror(errno));
    fd = open(path2, O_WRONLY | O_APPEND);
    if (fd < 0 || write(fd, "x", 1) != 1) probe_fail("append: %s", strerror(errno));
    /* The file's own watch and the directory's both see the write. */
    struct inotify_event *e1 = next_event(), *e2 = next_event();
    if (!e1 || !e2 || (e1->mask & ~0u) != IN_MODIFY || e2->mask != IN_MODIFY ||
        !((e1->wd == fwd && e2->wd == wd) || (e1->wd == wd && e2->wd == fwd)))
        probe_fail("write with two watches: %d/%#x %d/%#x", e1 ? e1->wd : 0, e1 ? e1->mask : 0,
                   e2 ? e2->wd : 0, e2 ? e2->mask : 0);
    close(fd);
    expect(wd, IN_CLOSE_WRITE, "b", "close (2)");
    if (unlink(path2) != 0) probe_fail("unlink: %s", strerror(errno));
    /* Linux: IN_ATTRIB (link count) on the file, IN_DELETE on the
     * directory, IN_DELETE_SELF and IN_IGNORED on the file.  The order of
     * the first two is the kernel's; accept either. */
    int seen_del = 0, seen_self = 0, seen_ign = 0;
    struct inotify_event *e;
    while ((e = next_event())) {
        if (e->wd == wd && e->mask == IN_DELETE && e->len && !strcmp(e->name, "b")) seen_del = 1;
        else if (e->wd == fwd && e->mask == IN_DELETE_SELF) seen_self = 1;
        else if (e->wd == fwd && e->mask == IN_IGNORED) seen_ign = 1;
        else if (e->wd == fwd && e->mask == IN_ATTRIB) continue;
        else probe_fail("unlink: unexpected wd %d mask %#x", e->wd, e->mask);
    }
    if (!seen_del || !seen_self || !seen_ign)
        probe_fail("unlink: delete %d delete_self %d ignored %d", seen_del, seen_self, seen_ign);

    if (mkdir(sub, 0755) != 0) probe_fail("mkdir: %s", strerror(errno));
    expect(wd, IN_CREATE | IN_ISDIR, "sub", "mkdir");
    if (rmdir(sub) != 0) probe_fail("rmdir: %s", strerror(errno));
    expect(wd, IN_DELETE | IN_ISDIR, "sub", "rmdir");
    expect_none("after rmdir");

    /* epoll readiness, and a blocking read woken by another process. */
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = in_fd };
    if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, in_fd, &ev) != 0) probe_fail("epoll: %s", strerror(errno));
    struct epoll_event out;
    if (epoll_wait(ep, &out, 1, 0) != 0) probe_fail("epoll: ready with an empty queue");
    pid_t pid = fork();
    if (pid == 0) {
        struct timespec ts = { 0, 200000000L };
        nanosleep(&ts, NULL);
        int f = open(path, O_CREAT | O_WRONLY, 0644);
        _exit(f < 0);
    }
    if (epoll_wait(ep, &out, 1, 5000) != 1) probe_fail("epoll_wait: no event from the child's create");
    expect(wd, IN_CREATE, "a", "child create");
    waitpid(pid, NULL, 0);
    expect(wd, IN_CLOSE_WRITE, "a", "child exit closes");
    int bfd = inotify_init1(IN_CLOEXEC);                /* blocking */
    int bwd = inotify_add_watch(bfd, dir, IN_DELETE);
    pid = fork();
    if (pid == 0) {
        struct timespec ts = { 0, 200000000L };
        nanosleep(&ts, NULL);
        _exit(unlink(path) != 0);
    }
    int n = (int)read(bfd, buf, sizeof buf);
    struct inotify_event *be = (struct inotify_event *)buf;
    if (n < (int)sizeof(*be) || be->wd != bwd || be->mask != IN_DELETE)
        probe_fail("blocking read: %d bytes, mask %#x", n, n > 0 ? be->mask : 0);
    waitpid(pid, NULL, 0);
    blen = bpos = 0;
    expect(wd, IN_DELETE, "a", "child unlink");

    if (inotify_rm_watch(in_fd, wd) != 0) probe_fail("inotify_rm_watch: %s", strerror(errno));
    expect(wd, IN_IGNORED, NULL, "rm_watch");
    if (inotify_rm_watch(in_fd, wd) == 0 || errno != EINVAL) probe_fail("second rm_watch: want EINVAL");

    /* Queue overflow (tmpfs only). */
    if (!strcmp(base, "/tmp")) {
        FILE *f = fopen("/proc/sys/fs/inotify/max_queued_events", "r");
        int maxq = 0;
        if (f) { if (fscanf(f, "%d", &maxq) != 1) maxq = 0; fclose(f); }
        if (maxq > 0 && maxq <= 20000) {
            wd = inotify_add_watch(in_fd, dir, IN_CREATE | IN_DELETE);
            for (int i = 0; i < maxq / 2 + 8; i++) {
                int k = open(path, O_CREAT | O_WRONLY, 0644);
                if (k < 0) probe_fail("overflow create %d: %s", i, strerror(errno));
                close(k);
                unlink(path);
            }
            int count = 0, overflow = 0;
            blen = bpos = 0;
            while ((e = next_event())) {
                if (e->mask & IN_Q_OVERFLOW) {
                    if (e->wd != -1) probe_fail("IN_Q_OVERFLOW wd %d", e->wd);
                    overflow++;
                } else {
                    count++;
                }
            }
            if (overflow != 1 || count != maxq)
                probe_fail("overflow: %d events then %d IN_Q_OVERFLOW (max %d)", count, overflow, maxq);
        } else {
            probe_info("max_queued_events %d: overflow not checked", maxq);
        }
    }
    rmdir(dir);
    probe_pass();
}
