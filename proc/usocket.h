#pragma once
#include <stdint.h>

/*
 * AF_UNIX (local) stream sockets — the IPC transport X11, Wayland and D-Bus
 * use.  A connected pair is two endpoints, each with a receive ring (the peer
 * writes it) and a transmit ring (the peer reads it).  Named sockets register
 * a filesystem-style path so connect() can find a listen()ing server.
 */
typedef struct usocket usocket_t;

/* Per-call flags for usocket_read()/usocket_write().  The socketcall layer maps
 * the user MSG_* bits onto these; USOCK_NONBLOCK is 1 so an O_NONBLOCK boolean
 * can be passed straight through. */
#define USOCK_NONBLOCK  0x1   /* do not sleep: -EAGAIN instead        */
#define USOCK_PEEK      0x2   /* MSG_PEEK: leave the data queued      */
#define USOCK_WANTFDS   0x4   /* recvmsg with room for ancillary fds  */
#define USOCK_NOSIGNAL  0x8   /* MSG_NOSIGNAL: EPIPE without SIGPIPE  */

usocket_t *usocket_create(int type);
int  usocket_socketpair(usocket_t **a, usocket_t **b);
int  usocket_bind(usocket_t *s, const char *path);
int  usocket_listen(usocket_t *s, int backlog);
int  usocket_connect(usocket_t *s, const char *path);
usocket_t *usocket_accept(usocket_t *s, int nonblock, int *err);
int  usocket_read(usocket_t *s, void *buf, int len, int flags);
int  usocket_write(usocket_t *s, const void *buf, int len, int flags);
int  usocket_read_ready(usocket_t *s);   /* data available / EOF / accept ready */
int  usocket_write_ready(usocket_t *s);  /* space to write and peer alive */
int  usocket_fds_ready(usocket_t *s);    /* a SCM_RIGHTS batch is deliverable */
int  usocket_hup(usocket_t *s);          /* peer closed → POLLHUP */
const char *usocket_path(usocket_t *s);  /* bound path ("" if none) */
void *usocket_rx_id(usocket_t *s);       /* diag: shared rx buffer id */
void *usocket_tx_id(usocket_t *s);       /* diag: shared tx buffer id */
/* diag: bytes queued in each direction, the ring capacity, and whether the
 * peer has closed its end.  A wedge with tx full and the peer blocked writing
 * its own tx is the classic AF_UNIX flow-control deadlock, and this is what
 * makes it visible from the watchdog. */
void usocket_stat(usocket_t *s, uint32_t *rx, uint32_t *tx, uint32_t *cap,
                  int *peer_gone);
void usocket_retain(usocket_t *s);
void usocket_release(usocket_t *s);
