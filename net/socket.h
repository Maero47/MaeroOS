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
int net_socket_is_stream(net_socket_t *s);   /* SOCK_STREAM (TCP) vs UDP */
int net_socket_shutdown(net_socket_t *s, int how);
int net_socket_getname(net_socket_t *s, int peer, net_sockaddr_in_t *out);
/* listen(): -EOPNOTSUPP for UDP, -EINVAL once connected; again on a
 * listener it only changes the backlog (clamped to 1..32). */
int net_socket_listen(net_socket_t *s, int backlog);
int net_socket_is_listening(net_socket_t *s);
/* accept(): the next established connection, as a new socket holding one
 * reference for the caller.  Blocks unless nonblock (-EAGAIN), bounded by
 * SO_RCVTIMEO; -EINTR on a signal, -EINVAL when not listening. */
int net_socket_accept(net_socket_t *s, net_socket_t **out, int nonblock);
/* setsockopt/getsockopt (Linux level/optname values).  `val` is a kernel
 * copy.  getopt returns 1 for an option it does not know. */
int net_socket_setopt(net_socket_t *s, int level, int name,
                      const void *val, uint32_t len);
int net_socket_getopt(net_socket_t *s, int level, int name,
                      void *val, uint32_t *len);
int net_socket_read_ready(net_socket_t *s);
int net_socket_write_ready(net_socket_t *s);
int net_socket_poll_err(net_socket_t *s);

/* AF_NETLINK, AF_PACKET and AF_INET SOCK_RAW sockets (net/xsock.h) share
 * the slot table but take their own sockaddr: the syscall layer passes the
 * raw bytes.  xrecvfrom returns the datagram's full size (MSG_TRUNC). */
int net_socket_is_x(net_socket_t *s);
int net_socket_xbind(net_socket_t *s, const void *addr, uint32_t alen);
int net_socket_xconnect(net_socket_t *s, const void *addr, uint32_t alen);
int net_socket_xgetname(net_socket_t *s, int peer, void *addr, uint32_t *alen);
int net_socket_xsendto(net_socket_t *s, const void *buf, uint32_t len,
                       const void *addr, uint32_t alen, int flags);
int net_socket_xrecvfrom(net_socket_t *s, void *buf, uint32_t len,
                         void *addr, uint32_t *alen, int flags);
