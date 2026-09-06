/* xprobe — raw X11 protocol client that validates maeroX's connection
 * handshake.  It spawns the real maeroX server (headless), connects to
 * /tmp/.X11-unix/X0, performs the X11 connection setup, validates the reply,
 * and prints XHANDSHAKE_OK.  Self-contained: one foreground command. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static int rd(int fd, unsigned char *b, int n) {
    int got = 0;
    while (got < n) {
        int r = read(fd, b + got, n - got);
        if (r > 0) got += r;
        else if (r == 0) return got;
        else return -1;
    }
    return got;
}

static unsigned u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned u32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static int connect_x(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/.X11-unix/X0");
    socklen_t alen = sizeof(addr.sun_family) + strlen(addr.sun_path);
    if (connect(fd, (struct sockaddr *)&addr, alen) == 0) return fd;
    close(fd);
    return -1;
}

int main(void) {
    /* Spawn the real maeroX server, headless. */
    pid_t srv = fork();
    if (srv == 0) {
        execl("/maerox", "maerox", "-H", (char *)0);
        execl("/disk/maerox", "maerox", "-H", (char *)0);
        _exit(127);
    }

    /* Retry the connect until the server has bound + listened (~up to 4s). */
    int fd = -1;
    for (int i = 0; i < 200 && fd < 0; i++) {
        fd = connect_x();
        if (fd < 0) usleep(20000);
    }
    if (fd < 0) { printf("XHANDSHAKE_FAIL connect\n"); kill(srv, 9); return 1; }

    unsigned char req[12] = {0};
    req[0] = 'l';
    req[2] = 11; req[3] = 0;
    if (write(fd, req, 12) != 12) { printf("XHANDSHAKE_FAIL write\n"); kill(srv, 9); return 1; }

    unsigned char hdr[8];
    if (rd(fd, hdr, 8) != 8 || hdr[0] != 1) {
        printf("XHANDSHAKE_FAIL reply success=%d\n", hdr[0]); kill(srv, 9); return 1;
    }
    unsigned major = u16(hdr + 2), extra = u16(hdr + 6) * 4;
    if (major != 11) { printf("XHANDSHAKE_FAIL major=%u\n", major); kill(srv, 9); return 1; }

    unsigned char body[1024];
    if (extra > sizeof(body)) extra = sizeof(body);
    if ((unsigned)rd(fd, body, extra) != extra) {
        printf("XHANDSHAKE_FAIL body\n"); kill(srv, 9); return 1;
    }
    unsigned id_base = u32(body + 4), id_mask = u32(body + 8);
    unsigned vlen = u16(body + 16), nscreens = body[20];
    if (id_mask == 0 || nscreens < 1) {
        printf("XHANDSHAKE_FAIL mask=0x%x screens=%u\n", id_mask, nscreens);
        kill(srv, 9); return 1;
    }
    unsigned off = 32 + ((vlen + 3) & ~3u) + 2 * 8;
    unsigned root = u32(body + off);

    printf("XHANDSHAKE_OK root=0x%x base=0x%x mask=0x%x screens=%u\n",
           root, id_base, id_mask, nscreens);
    fflush(stdout);
    close(fd);
    kill(srv, 9);
    waitpid(srv, 0, 0);
    return 0;
}
