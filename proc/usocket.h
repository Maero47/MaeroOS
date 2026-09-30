#pragma once
#include <stdint.h>
#include "process.h"   /* proc_file_t */

struct vfs_node;

/*
 * AF_UNIX (local) sockets — the IPC transport X11, Wayland and D-Bus use.
 *
 * SOCK_STREAM, SOCK_SEQPACKET and SOCK_DGRAM.  A connected pair is two
 * endpoints, each with a receive ring (the peer writes it) and a transmit ring
 * (the peer reads it).  SEQPACKET and DGRAM rings hold whole records, so
 * message boundaries survive (net/unix/af_unix.c unix_dgram_sendmsg).  A DGRAM
 * socket owns its receive ring from creation; connect() or sendto() points a
 * sender at the receiver's ring.
 *
 * Names follow Linux: a filesystem name is a socket inode (S_IFSOCK) created
 * by bind() — the syscall layer does the VFS work and hands the node in — and
 * connect() finds the listener through that inode; an abstract name (leading
 * NUL) is a separate namespace compared as bytes.
 *
 * Lifetime: `refs` counts every holder (descriptors, SCM_RIGHTS batches in
 * flight, and syscalls in progress, which pin the socket for their whole
 * duration).  Once only pins remain — the last descriptor was closed, maybe
 * by another thread while one of these syscalls sleeps — the socket is shut
 * down: the peer sees EOF/EPIPE, the name is released, and every sleeper
 * wakes and returns -EBADF.  The memory goes with the last pin.  (Linux keeps
 * the file open until such a syscall returns, so its sleeper stays asleep;
 * waking it with -EBADF is deliberate here.)
 */
typedef struct usocket usocket_t;

#define USOCK_STREAM     1
#define USOCK_DGRAM      2
#define USOCK_SEQPACKET  5

/* Per-call flags for the read/write/sendmsg/recvmsg entry points.  The
 * socketcall layer maps the user MSG_* bits onto these; USOCK_NONBLOCK is 1 so
 * an O_NONBLOCK boolean can be passed straight through. */
#define USOCK_NONBLOCK  0x1   /* do not sleep: -EAGAIN instead        */
#define USOCK_PEEK      0x2   /* MSG_PEEK: leave the data queued      */
#define USOCK_WANTFDS   0x4   /* recvmsg with room for ancillary fds  */
#define USOCK_NOSIGNAL  0x8   /* MSG_NOSIGNAL: EPIPE without SIGPIPE  */
#define USOCK_TRUNC     0x10  /* MSG_TRUNC on a record socket: return the
                               * record's full length                 */

/* Output flag of usocket_recvmsg (Linux MSG_TRUNC). */
#define USOCK_MSG_TRUNC 0x20

/* A user buffer segment (struct iovec, user pointers). */
typedef struct { void *base; uint32_t len; } usock_iov_t;

/* A sockaddr_un name: `len` bytes of sun_path (0 = unnamed), NUL-terminated
 * in `path` as well.  A filesystem name is the path as given to bind (its
 * NUL not counted; the syscall layer reports it with the NUL, as Linux does);
 * an abstract name starts with NUL and is `len` raw bytes. */
#define USOCK_PATH_MAX 108
typedef struct {
    uint32_t len;
    char     path[USOCK_PATH_MAX + 1];
} usock_name_t;

/* struct ucred as SO_PEERCRED returns it. */
typedef struct { int32_t pid; uint32_t uid, gid; } usock_cred_t;

usocket_t *usocket_create(int type);
int  usocket_socketpair(int type, usocket_t **a, usocket_t **b);
int  usocket_type(usocket_t *s);
int  usocket_is_record(usocket_t *s);    /* SEQPACKET or DGRAM */
int  usocket_listening(usocket_t *s);

/* node != NULL: filesystem name, the freshly created socket inode (the socket
 * takes its own reference); NULL: abstract name. */
int  usocket_bind(usocket_t *s, const usock_name_t *name, struct vfs_node *node);
int  usocket_autobind(usocket_t *s);
int  usocket_listen(usocket_t *s, int backlog);
/* connect() to a name (node as for bind; the caller checked the permission).
 * SOCK_DGRAM sets the default destination; name NULL dissolves it. */
int  usocket_connect(usocket_t *s, const usock_name_t *name,
                     struct vfs_node *node, int nonblock);
usocket_t *usocket_accept(usocket_t *s, int nonblock, int *err);

int  usocket_read(usocket_t *s, void *buf, int len, int flags);
int  usocket_write(usocket_t *s, const void *buf, int len, int flags);

/* Record sockets: send one record gathered from iov, with `nfds` already
 * retained file refs (ownership moves into the queue on success only) to the
 * connected peer, or to the named DGRAM socket when `to` is given. */
int  usocket_send_record(usocket_t *s, const usock_iov_t *iov, int niov,
                         proc_file_t *fds, int nfds,
                         const usock_name_t *to, struct vfs_node *tonode,
                         int flags);
/* Receive one record scattered into iov; *oflags gets USOCK_MSG_TRUNC, *from
 * the sender's name (DGRAM) or the peer's. */
int  usocket_recv_record(usocket_t *s, const usock_iov_t *iov, int niov,
                         int flags, int *oflags, usock_name_t *from);

int  usocket_shutdown(usocket_t *s, int how);
/* getsockname (peer = 0) / getpeername (peer = 1); -ENOTCONN if no peer. */
int  usocket_getname(usocket_t *s, int peer, usock_name_t *out);
void usocket_peercred(usocket_t *s, usock_cred_t *out);

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
/* A syscall working on s holds a pin from start to finish (Linux fdget). */
void usocket_pin(usocket_t *s);
void usocket_unpin(usocket_t *s);
