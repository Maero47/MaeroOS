#include "../include/termios.h"
#include "../include/syscall.h"

int tcgetattr(int fd, struct termios *t) {
    return syscall3(54, fd, 0x5401, (int)t);  /* ioctl(fd, TCGETS, t) */
}

int tcsetattr(int fd, int action, const struct termios *t) {
    int req = (action == 0) ? 0x5402 : (action == 1) ? 0x5403 : 0x5404;
    return syscall3(54, fd, req, (int)t);
}

int tcgetpgrp(int fd) {
    int pgrp = -1;
    if (syscall3(54, fd, TIOCGPGRP, (int)&pgrp) < 0)
        return -1;
    return pgrp;
}

int tcsetpgrp(int fd, int pgrp) {
    return syscall3(54, fd, TIOCSPGRP, (int)&pgrp);
}

void cfmakeraw(struct termios *t) {
    t->c_iflag &= ~(ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cc[VMIN]  = 1;
    t->c_cc[VTIME] = 0;
}
