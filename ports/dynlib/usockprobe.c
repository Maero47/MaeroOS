/* usockprobe — exercises AF_UNIX local sockets (the IPC transport X11/Wayland/
 * D-Bus use).  Tests socketpair() and a named bind/listen/connect/accept with a
 * request/reply exchange across a fork.  Prints UNIX_SOCK_OK on success. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static int test_socketpair(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    char buf[16] = {0};
    if (write(sv[0], "ping", 4) != 4) return -2;
    if (read(sv[1], buf, sizeof(buf)) != 4 || memcmp(buf, "ping", 4)) return -3;
    if (write(sv[1], "pong", 4) != 4) return -4;
    if (read(sv[0], buf, sizeof(buf)) != 4 || memcmp(buf, "pong", 4)) return -5;
    close(sv[0]); close(sv[1]);
    return 0;
}

static int test_named(void) {
    const char *path = "/tmp/usockprobe.sock";
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    socklen_t alen = sizeof(addr.sun_family) + strlen(path);

    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) return -10;
    if (bind(ls, (struct sockaddr *)&addr, alen) != 0) return -11;
    if (listen(ls, 4) != 0) return -12;

    pid_t pid = fork();
    if (pid == 0) {                      /* child = client */
        int cs = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cs < 0) _exit(21);
        if (connect(cs, (struct sockaddr *)&addr, alen) != 0) _exit(22);
        char b[16] = {0};
        if (write(cs, "HELLO", 5) != 5) _exit(23);
        if (read(cs, b, sizeof(b)) != 5 || memcmp(b, "WORLD", 5)) _exit(24);
        close(cs);
        _exit(0);
    }
    int as = accept(ls, 0, 0);           /* parent = server */
    if (as < 0) return -13;
    char b[16] = {0};
    if (read(as, b, sizeof(b)) != 5 || memcmp(b, "HELLO", 5)) return -14;
    if (write(as, "WORLD", 5) != 5) return -15;
    close(as); close(ls);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return -16;
    return 0;
}

/* Abstract namespace (Linux): sun_path[0]==0 then the name.  libxcb uses this
 * for "@/tmp/.X11-unix/X0", so it is on the real-Xlib path. */
static int test_abstract(void) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const char *name = "maeroX-abstract-test";       /* after the leading NUL */
    memcpy(addr.sun_path + 1, name, strlen(name));    /* sun_path[0] stays NUL */
    socklen_t alen = sizeof(addr.sun_family) + 1 + strlen(name);

    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) return -30;
    if (bind(ls, (struct sockaddr *)&addr, alen) != 0) return -31;
    if (listen(ls, 4) != 0) return -32;

    pid_t pid = fork();
    if (pid == 0) {
        int cs = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cs < 0) _exit(41);
        if (connect(cs, (struct sockaddr *)&addr, alen) != 0) _exit(42);
        char b[8] = {0};
        if (read(cs, b, sizeof(b)) != 2 || memcmp(b, "AB", 2)) _exit(43);
        close(cs);
        _exit(0);
    }
    int as = accept(ls, 0, 0);
    if (as < 0) return -33;
    if (write(as, "AB", 2) != 2) return -34;
    close(as); close(ls);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return -35;
    return 0;
}

int main(void) {
    int r = test_socketpair();
    if (r) { printf("UNIX_SOCK_FAIL socketpair=%d\n", r); return 1; }
    r = test_named();
    if (r) { printf("UNIX_SOCK_FAIL named=%d\n", r); return 1; }
    r = test_abstract();
    if (r) { printf("UNIX_SOCK_FAIL abstract=%d\n", r); return 1; }
    printf("UNIX_SOCK_OK socketpair+named+abstract\n");
    fflush(stdout);
    return 0;
}
