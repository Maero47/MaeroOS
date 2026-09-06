#pragma once
#include <stdint.h>

/* Linux i386 struct termios (36 bytes) */
typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int  speed_t;

#define NCCS 19
struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
};

/* c_lflag bits */
#define ISIG    0x00000001
#define ICANON  0x00000002
#define ECHO    0x00000008
#define ECHOE   0x00000010
#define ECHOK   0x00000020
#define ECHONL  0x00000040
#define NOFLSH  0x00000080
#define TOSTOP  0x00000100
#define ECHOCTL 0x00000200
#define ECHOKE  0x00000800
#define IEXTEN  0x00008000

/* c_iflag bits */
#define ICRNL   0x00000100
#define IXON    0x00000400
#define IXANY   0x00000800
#define IUTF8   0x00004000

/* c_oflag bits */
#define OPOST   0x00000001
#define ONLCR   0x00000004

/* c_cflag bits */
#define CS8     0x00000030
#define CREAD   0x00000080
#define CLOCAL  0x00000800

/* c_cc indices */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSTART  8
#define VSTOP   9
#define VSUSP   10

/* tcsetattr actions */
#define TCSANOW   0x5402
#define TCSADRAIN 0x5403
#define TCSAFLUSH 0x5404

/* ioctl requests */
#define TCGETS    0x5401
#define TCSETS    0x5402
#define TIOCGWINSZ 0x5413
#define TIOCGPGRP  0x5414
#define TIOCSPGRP  0x5415
#define TIOCSCTTY  0x540E
#define TCIFLUSH  0

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int action, const struct termios *t);
int tcflush(int fd, int queue_selector);
int cfsetspeed(struct termios *t, speed_t speed);
void cfmakeraw(struct termios *t);
int tcgetpgrp(int fd);
int tcsetpgrp(int fd, int pgrp);
