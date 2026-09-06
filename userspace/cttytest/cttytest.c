#include "../include/fcntl.h"
#include "../include/signal.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/sys/ioctl.h"
#include "../include/sys/wait.h"
#include "../include/termios.h"
#include "../include/time.h"
#include "../include/unistd.h"

#define TIOCGPTN 0x80045430U
#define TIOCSPTLCK 0x40045431U

static void on_hup(int sig) {
    (void)sig;
    const char *msg = "ctty hangup received\n";
    write(1, msg, strlen(msg));
    _exit(0);
}

static int open_pair(int *master_out, int *slave_out) {
    int master = open("/dev/ptmx", O_RDWR);
    if (master < 0) {
        printf("cttytest: open /dev/ptmx failed\n");
        return -1;
    }

    int unlock = 0;
    if (ioctl(master, TIOCSPTLCK, &unlock) < 0) {
        printf("cttytest: unlock failed\n");
        close(master);
        return -1;
    }

    int n = -1;
    if (ioctl(master, TIOCGPTN, &n) < 0 || n < 0) {
        printf("cttytest: TIOCGPTN failed n=%d\n", n);
        close(master);
        return -1;
    }

    char path[16];
    sprintf(path, "/dev/pts/%d", n);
    int slave = open(path, O_RDWR);
    if (slave < 0) {
        printf("cttytest: open %s failed\n", path);
        close(master);
        return -1;
    }

    *master_out = master;
    *slave_out = slave;
    return 0;
}

static int contains(const char *haystack, int hay_len, const char *needle) {
    int needle_len = strlen(needle);
    if (needle_len == 0 || hay_len < needle_len)
        return 0;

    for (int i = 0; i <= hay_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return 1;
    }
    return 0;
}

int main(void) {
    int master = -1;
    int slave = -1;
    if (open_pair(&master, &slave) < 0)
        return 1;

    if (ioctl(master, TIOCSCTTY, 0) >= 0) {
        printf("cttytest: master accepted TIOCSCTTY\n");
        close(slave);
        close(master);
        return 1;
    }

    int pid = fork();
    if (pid < 0) {
        printf("cttytest: fork failed\n");
        close(slave);
        close(master);
        return 1;
    }

    if (pid == 0) {
        close(master);

        if (setsid() < 0)
            _exit(2);
        if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0)
            _exit(3);
        if (slave > 2)
            close(slave);

        if (ioctl(0, TIOCSCTTY, 0) < 0)
            _exit(4);

        int pgrp = getpgrp();
        if (tcgetpgrp(0) != pgrp)
            _exit(5);

        int tty = open("/dev/tty", O_RDWR);
        if (tty < 0)
            _exit(6);

        if (tcgetpgrp(tty) != pgrp)
            _exit(7);

        if (tcsetpgrp(tty, 9999) >= 0)
            _exit(8);
        if (tcsetpgrp(tty, pgrp) < 0)
            _exit(9);

        const char *msg = "ctty child through /dev/tty\n";
        if (write(tty, msg, strlen(msg)) != (int)strlen(msg))
            _exit(10);

        int worker = fork();
        if (worker < 0)
            _exit(11);
        if (worker == 0) {
            signal(SIGHUP, on_hup);
            const char *ready = "ctty hangup ready\n";
            write(1, ready, strlen(ready));
            for (;;)
                sched_yield();
        }

        struct timespec ts = { 1, 0 };
        nanosleep(&ts, 0);
        close(tty);
        _exit(0);
    }

    close(slave);

    char buf[128];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = read(master, buf + total, (int)sizeof(buf) - 1 - total);
        if (n <= 0)
            break;
        total += n;
        buf[total] = 0;
        if (contains(buf, total, "ctty child through /dev/tty") &&
            contains(buf, total, "ctty hangup received"))
            break;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    close(master);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("cttytest: child failed status=%d\n", status);
        return 1;
    }
    if (!contains(buf, total, "ctty child through /dev/tty")) {
        printf("cttytest: missing /dev/tty output, got %d bytes\n", total);
        return 1;
    }
    if (!contains(buf, total, "ctty hangup received")) {
        printf("cttytest: missing SIGHUP output, got %d bytes\n", total);
        return 1;
    }

    printf("cttytest ok\n");
    return 0;
}
