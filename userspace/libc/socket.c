#include "../include/sys/socket.h"
#include "../include/netinet/in.h"
#include "../include/arpa/inet.h"
#include "../include/syscall.h"
#include "../include/errno.h"
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

int recv(int fd, void *buf, size_t len, int flags) {
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
    uint32_t args[6] = {
        (uint32_t)fd, (uint32_t)buf, (uint32_t)len, (uint32_t)flags,
        (uint32_t)addr, (uint32_t)addrlen
    };
    return socketcall(12, args);
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

