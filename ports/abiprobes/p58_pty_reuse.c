/*
 * P58 a read asleep on a pty never reads the next pty's data.
 *
 * Linux: a thread blocked in read() on a pty slave holds the open file; when
 * another thread closes the descriptors the read ends with EIO/0 (hangup)
 * and can never see a terminal opened afterwards.
 *
 * MaeroOS: a blocked read held no reference.  Closing both ends from another
 * thread freed the pair under it; the next /dev/ptmx open (another user's
 * xterm or ssh login) got the same slot, and the sleeping read returned that
 * terminal's input -- its keystrokes and passwords.
 */
#define PROBE_NAME "p58_pty_reuse"
#include "probe.h"
#include <sys/ioctl.h>

static int sfd;
static volatile int got = -2;
static char rbuf[64];

static void *reader(void *a)
{
    (void)a;
    got = (int)read(sfd, rbuf, sizeof rbuf - 1);
    return NULL;
}

static int open_pty(int *slave)
{
    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0) probe_fail("open /dev/ptmx: %s", strerror(errno));
    unsigned n = 0;
    int unlock = 0;
    if (ioctl(m, TIOCGPTN, &n) != 0) probe_fail("TIOCGPTN: %s", strerror(errno));
    ioctl(m, TIOCSPTLCK, &unlock);
    char path[32];
    snprintf(path, sizeof path, "/dev/pts/%u", n);
    *slave = open(path, O_RDWR | O_NOCTTY);
    if (*slave < 0) probe_fail("open %s: %s", path, strerror(errno));
    return m;
}

int main(void)
{
    probe_watchdog(60);
    int m = open_pty(&sfd);
    pthread_t t;
    pthread_create(&t, NULL, reader, NULL);
    usleep(300000);                     /* the reader is asleep in read() */
    int s = sfd;
    close(s);
    close(m);
    usleep(300000);
    /* A new terminal, very likely in the freed slot. */
    int s2;
    int m2 = open_pty(&s2);
    if (write(m2, "secret-password\n", 16) != 16)
        probe_fail("write new master: %s", strerror(errno));
    for (int i = 0; i < 40 && got == -2; i++) usleep(50000);
    if (got > 0) {
        rbuf[got] = 0;
        if (strstr(rbuf, "secret"))
            probe_fail("the read asleep on a closed pty returned the next pty's "
                       "input \"%.*s\"", got - 1, rbuf);
    }
    probe_info("blocked read ended with %d", got);
    close(s2);
    close(m2);
    probe_pass();
}
