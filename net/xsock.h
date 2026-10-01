#pragma once

#include <stdint.h>

/*
 * Socket families beside lwIP's TCP/UDP that live in the same descriptor
 * type (FD_SOCKET, net/socket.c): AF_NETLINK (net/netlink.c), AF_PACKET and
 * AF_INET SOCK_RAW (net/rawsock.c).  net/socket.c owns the slot, the
 * reference count, SO_RCVTIMEO/SO_SNDTIMEO and the blocking loops; a family
 * supplies these non-blocking operations.  Addresses are the family's own
 * sockaddr, `alen` bytes long.  Every op runs under the BKL with preemption
 * disabled and must not sleep.
 */
typedef struct xsock_ops {
    int  (*bind)(void *x, const void *addr, uint32_t alen);
    int  (*connect)(void *x, const void *addr, uint32_t alen);
    /* Writes the name into addr (at most 128 bytes) and its size to *alen. */
    int  (*getname)(void *x, int peer, void *addr, uint32_t *alen);
    /* One datagram: the bytes sent, or a negative errno (-EAGAIN: try again
     * after the next network activity). */
    int  (*send)(void *x, const void *buf, uint32_t len,
                 const void *addr, uint32_t alen);
    /* The next datagram: copies up to `len` bytes, returns its FULL size
     * (so the caller can report MSG_TRUNC), -EAGAIN when there is none.
     * `peek` leaves it queued.  addr may be NULL. */
    int  (*recv)(void *x, void *buf, uint32_t len, void *addr,
                 uint32_t *alen, int peek);
    int  (*read_ready)(void *x);
    /* Options this family handles itself; 1 = not one of them. */
    int  (*setopt)(void *x, int level, int name, const void *val, uint32_t len);
    int  (*getopt)(void *x, int level, int name, void *val, uint32_t *len);
    void (*release)(void *x);
} xsock_ops_t;

int netlink_create(int type, int protocol, const xsock_ops_t **ops, void **x);
int packet_create(int type, int protocol, const xsock_ops_t **ops, void **x);
int rawip_create(int protocol, const xsock_ops_t **ops, void **x);

/* A frame received (outgoing = 0, before lwIP sees it) or sent (1) on
 * iface: a copy for every AF_PACKET socket that asked for it. */
struct maero_netif;
void packet_deliver(struct maero_netif *iface, const uint8_t *frame,
                    uint32_t len, int outgoing);

/* Interface ioctls (SIOCGIFCONF, SIOCGIFFLAGS, SIOCSIFADDR, SIOCADDRT, ...)
 * on any socket.  1 when `req` is not one of them. */
int netdev_ioctl(uint32_t req, void *uarg);
/* /proc/net/dev and /proc/net/route. */
uint32_t netdev_proc_dev(char *buf, uint32_t cap);
uint32_t netdev_proc_route(char *buf, uint32_t cap);

/* Interface index (1-based position in net.c's table) <-> interface. */
struct maero_netif;
int netdev_index(struct maero_netif *iface);
struct maero_netif *netdev_by_index(int index);
