#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10

#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX   AF_UNIX
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

#define SOL_SOCKET 1
#define SO_REUSEADDR 2

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int send(int fd, const void *buf, size_t len, int flags);
int recv(int fd, void *buf, size_t len, int flags);
int sendto(int fd, const void *buf, size_t len, int flags,
           const struct sockaddr *addr, socklen_t addrlen);
int recvfrom(int fd, void *buf, size_t len, int flags,
             struct sockaddr *addr, socklen_t *addrlen);
int shutdown(int fd, int how);
int setsockopt(int fd, int level, int optname, const void *optval,
               socklen_t optlen);
