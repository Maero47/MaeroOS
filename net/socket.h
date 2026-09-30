#pragma once

#include <stdint.h>

typedef struct net_socket net_socket_t;

typedef struct net_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
} __attribute__((packed)) net_sockaddr_in_t;

void net_sockets_init(void);
int net_socket_create(int domain, int type, int protocol, net_socket_t **out);
void net_socket_retain(net_socket_t *s);
void net_socket_release(net_socket_t *s);
int net_socket_bind(net_socket_t *s, const net_sockaddr_in_t *addr);
/* Per-call flags of sendto/recvfrom (Linux MSG_* values).  NET_MSG_DONTWAIT
 * is also how the caller passes the descriptor's O_NONBLOCK. */
#define NET_MSG_PEEK      0x0002
#define NET_MSG_DONTWAIT  0x0040
#define NET_MSG_WAITALL   0x0100

/* A blocking TCP connect waits for the handshake; a non-blocking one starts it
 * and returns -EINPROGRESS, then -EALREADY while it runs and -EISCONN once it
 * is up; the outcome is read with net_socket_take_error (SO_ERROR). */
int net_socket_connect(net_socket_t *s, const net_sockaddr_in_t *addr,
                       int nonblock);
/* -EPIPE means the caller should raise SIGPIPE unless MSG_NOSIGNAL. */
int net_socket_sendto(net_socket_t *s, const void *buf, uint32_t len,
                      const net_sockaddr_in_t *addr, int flags);
int net_socket_recvfrom(net_socket_t *s, void *buf, uint32_t len,
                        net_sockaddr_in_t *addr, int flags);
/* SO_ERROR: the pending error (a negative errno, or 0), cleared by reading. */
int net_socket_take_error(net_socket_t *s);
int net_socket_shutdown(net_socket_t *s, int how);
int net_socket_read_ready(net_socket_t *s);
int net_socket_write_ready(net_socket_t *s);
