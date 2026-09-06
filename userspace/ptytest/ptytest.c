#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/sys/ioctl.h"
#include "../include/termios.h"
#include "../include/unistd.h"

#define TIOCGPTN 0x80045430U
#define TIOCSPTLCK 0x40045431U
#define PTY_REUSE_ROUNDS 16

static int open_pair(int *master_out, int *slave_out) {
    int master = open("/dev/ptmx", O_RDWR);
    if (master < 0) {
        printf("ptytest: open /dev/ptmx failed\n");
        return -1;
    }

    int unlock = 0;
    if (ioctl(master, TIOCSPTLCK, &unlock) < 0) {
        printf("ptytest: unlock failed\n");
        close(master);
        return -1;
    }

    int n = -1;
    if (ioctl(master, TIOCGPTN, &n) < 0 || n < 0) {
        printf("ptytest: TIOCGPTN failed n=%d\n", n);
        close(master);
        return -1;
    }

    char path[16];
    sprintf(path, "/dev/pts/%d", n);
    int slave = open(path, O_RDWR);
    if (slave < 0) {
        printf("ptytest: open %s failed\n", path);
        close(master);
        return -1;
    }

    *master_out = master;
    *slave_out = slave;
    return 0;
}

int main(void) {
    int master = -1;
    int slave = -1;

    if (open_pair(&master, &slave) < 0)
        return 1;

    struct termios tio;
    if (tcgetattr(slave, &tio) < 0) {
        printf("ptytest: tcgetattr failed\n");
        close(slave);
        close(master);
        return 1;
    }
    tio.c_lflag &= ~(ICANON | ECHO);
    tio.c_oflag &= ~(OPOST | ONLCR);
    if (tcsetattr(slave, TCSANOW, &tio) < 0) {
        printf("ptytest: tcsetattr failed\n");
        close(slave);
        close(master);
        return 1;
    }

    char c = 0;
    if (write(master, "a", 1) != 1 || read(slave, &c, 1) != 1 || c != 'a') {
        printf("ptytest: master to slave failed c=%c\n", c);
        close(slave);
        close(master);
        return 1;
    }
    if (write(slave, "b", 1) != 1 || read(master, &c, 1) != 1 || c != 'b') {
        printf("ptytest: slave to master failed c=%c\n", c);
        close(slave);
        close(master);
        return 1;
    }

    close(slave);
    close(master);

    for (int i = 0; i < PTY_REUSE_ROUNDS; i++) {
        if (open_pair(&master, &slave) < 0) {
            printf("ptytest: reuse failed at round %d\n", i);
            return 1;
        }
        close(slave);
        close(master);
    }

    printf("ptytest ok\n");
    return 0;
}
