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
int net_socket_connect(net_socket_t *s, const net_sockaddr_in_t *addr);
int net_socket_sendto(net_socket_t *s, const void *buf, uint32_t len,
                      const net_sockaddr_in_t *addr);
int net_socket_recvfrom(net_socket_t *s, void *buf, uint32_t len,
                        net_sockaddr_in_t *addr);
int net_socket_shutdown(net_socket_t *s, int how);
int net_socket_read_ready(net_socket_t *s);
int net_socket_write_ready(net_socket_t *s);
