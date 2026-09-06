#pragma once
#include <stdint.h>

/*
 * AF_UNIX (local) stream sockets — the IPC transport X11, Wayland and D-Bus
 * use.  A connected pair is two endpoints, each with a receive ring (the peer
 * writes it) and a transmit ring (the peer reads it).  Named sockets register
 * a filesystem-style path so connect() can find a listen()ing server.
 */
typedef struct usocket usocket_t;

usocket_t *usocket_create(int type);
int  usocket_socketpair(usocket_t **a, usocket_t **b);
int  usocket_bind(usocket_t *s, const char *path);
int  usocket_listen(usocket_t *s, int backlog);
int  usocket_connect(usocket_t *s, const char *path);
usocket_t *usocket_accept(usocket_t *s, int nonblock, int *err);
int  usocket_read(usocket_t *s, void *buf, int len, int nonblock);
int  usocket_write(usocket_t *s, const void *buf, int len, int nonblock);
int  usocket_read_ready(usocket_t *s);   /* data available / EOF / accept ready */
int  usocket_write_ready(usocket_t *s);  /* space to write and peer alive */
const char *usocket_path(usocket_t *s);  /* bound path ("" if none) */
void *usocket_rx_id(usocket_t *s);       /* diag: shared rx buffer id */
void *usocket_tx_id(usocket_t *s);       /* diag: shared tx buffer id */
void usocket_retain(usocket_t *s);
void usocket_release(usocket_t *s);
