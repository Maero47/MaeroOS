#include "../include/sys/socket.h"
#include "../include/netinet/in.h"
#include "../include/arpa/inet.h"
#include "../include/syscall.h"
#include "../include/errno.h"
#include "../include/poll.h"
#include "../include/sys/time.h"
#include <stdint.h>

static inline int chkerr(int ret) {
    if (ret < 0) { errno = -ret; return -1; }
    errno = 0;
    return ret;
}

static int socketcall(int call, uint32_t *args) {
    return chkerr(syscall2(102, call, (int)args));
}

int socket(int domain, int type, int protocol) {
    uint32_t args[3] = { (uint32_t)domain, (uint32_t)type, (uint32_t)protocol };
    return socketcall(1, args);
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    uint32_t args[3] = { (uint32_t)fd, (uint32_t)addr, (uint32_t)len };
    return socketcall(2, args);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    uint32_t args[3] = { (uint32_t)fd, (uint32_t)addr, (uint32_t)len };
    return socketcall(3, args);
}

int listen(int fd, int backlog) {
    uint32_t args[2] = { (uint32_t)fd, (uint32_t)backlog };
    return socketcall(4, args);
}

int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    uint32_t args[3] = { (uint32_t)fd, (uint32_t)addr, (uint32_t)len };
    return socketcall(5, args);
}

int send(int fd, const void *buf, size_t len, int flags) {
    uint32_t args[4] = {
        (uint32_t)fd, (uint32_t)buf, (uint32_t)len, (uint32_t)flags
    };
    return socketcall(9, args);
}

/*
 * SO_RCVTIMEO, emulated here: the kernel accepts setsockopt() and ignores it,
 * and its UDP recv never blocks, so a program that sets a receive timeout and
 * then calls recv() (toybox host, ports) would get EAGAIN at once.  A socket
 * with a timeout set waits in poll() for up to that long first.  close()
 * forgets the setting (libc/syscalls.c).
 */
#define RCVTIMEO_FDS 256
static int rcvtimeo_ms[RCVTIMEO_FDS];

void __socket_forget(int fd) {
    if (fd >= 0 && fd < RCVTIMEO_FDS) rcvtimeo_ms[fd] = 0;
}

static int wait_rcvtimeo(int fd, int flags) {
    if (fd < 0 || fd >= RCVTIMEO_FDS || !rcvtimeo_ms[fd] ||
        (flags & MSG_DONTWAIT))
        return 0;
    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll(&p, 1, rcvtimeo_ms[fd]);
    if (r == 0) {
        errno = EAGAIN;
        return -1;
    }
    return r < 0 ? -1 : 0;
}

int setsockopt(int fd, int level, int optname, const void *optval,
               socklen_t optlen) {
    if (level == SOL_SOCKET && optname == SO_RCVTIMEO && fd >= 0 &&
        fd < RCVTIMEO_FDS && optval && optlen >= sizeof(struct timeval)) {
        const struct timeval *tv = optval;
        long ms = tv->tv_sec * 1000L + (tv->tv_usec + 999) / 1000;
        rcvtimeo_ms[fd] = ms > 0x7FFFFFFF ? 0x7FFFFFFF : (int)ms;
    }
    uint32_t args[5] = {
        (uint32_t)fd, (uint32_t)level, (uint32_t)optname,
        (uint32_t)optval, (uint32_t)optlen
    };
    return socketcall(14, args);
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    uint32_t args[3] = { (uint32_t)fd, (uint32_t)addr, (uint32_t)len };
    return socketcall(6, args);
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
    uint32_t args[3] = { (uint32_t)fd, (uint32_t)addr, (uint32_t)len };
    return socketcall(7, args);
}

int recv(int fd, void *buf, size_t len, int flags) {
    if (wait_rcvtimeo(fd, flags) < 0) return -1;
    uint32_t args[4] = {
        (uint32_t)fd, (uint32_t)buf, (uint32_t)len, (uint32_t)flags
    };
    return socketcall(10, args);
}

int sendto(int fd, const void *buf, size_t len, int flags,
           const struct sockaddr *addr, socklen_t addrlen) {
    uint32_t args[6] = {
        (uint32_t)fd, (uint32_t)buf, (uint32_t)len, (uint32_t)flags,
        (uint32_t)addr, (uint32_t)addrlen
    };
    return socketcall(11, args);
}

int recvfrom(int fd, void *buf, size_t len, int flags,
             struct sockaddr *addr, socklen_t *addrlen) {
    if (wait_rcvtimeo(fd, flags) < 0) return -1;
    uint32_t args[6] = {
        (uint32_t)fd, (uint32_t)buf, (uint32_t)len, (uint32_t)flags,
        (uint32_t)addr, (uint32_t)addrlen
    };
    return socketcall(12, args);
}

int getsockopt(int fd, int level, int optname, void *optval,
               socklen_t *optlen) {
    uint32_t args[5] = {
        (uint32_t)fd, (uint32_t)level, (uint32_t)optname,
        (uint32_t)optval, (uint32_t)optlen
    };
    return socketcall(15, args);
}

int shutdown(int fd, int how) {
    uint32_t args[2] = { (uint32_t)fd, (uint32_t)how };
    return socketcall(13, args);
}

uint16_t htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

uint16_t ntohs(uint16_t v) {
    return htons(v);
}

uint32_t htonl(uint32_t v) {
    return ((v & 0x000000ffU) << 24) |
           ((v & 0x0000ff00U) << 8) |
           ((v & 0x00ff0000U) >> 8) |
           ((v & 0xff000000U) >> 24);
}

uint32_t ntohl(uint32_t v) {
    return htonl(v);
}

uint32_t inet_addr(const char *s) {
    uint32_t parts[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return 0xffffffffU;
        while (*s >= '0' && *s <= '9') {
            parts[i] = parts[i] * 10U + (uint32_t)(*s - '0');
            if (parts[i] > 255U)
                return 0xffffffffU;
            s++;
        }
        if (i < 3) {
            if (*s != '.')
                return 0xffffffffU;
            s++;
        }
    }
    if (*s)
        return 0xffffffffU;
    return htonl((parts[0] << 24) | (parts[1] << 16) |
                 (parts[2] << 8) | parts[3]);
}

