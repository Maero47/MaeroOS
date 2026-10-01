#pragma once

#include <stdint.h>
#include <sys/socket.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

struct in6_addr {
    unsigned char s6_addr[16];
};

struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

#define IPPROTO_IP  0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#define IPPROTO_ICMPV6 58
#define IPPROTO_RAW 255

/* IPPROTO_IP options (Linux values) */
#define IP_TOS      1
#define IP_TTL      2
#define IP_RECVTTL  12

/* IPPROTO_IPV6 options (Linux values) */
#define IPV6_CHECKSUM        7
#define IPV6_UNICAST_HOPS    16
#define IPV6_MULTICAST_IF    17
#define IPV6_MULTICAST_HOPS  18
#define IPV6_MULTICAST_LOOP  19
#define IPV6_V6ONLY          26
#define IPV6_RECVPKTINFO     49
#define IPV6_RECVHOPLIMIT    51
#define IPV6_TCLASS          67

#define IN6ADDR_ANY_INIT      { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } }
#define IN6ADDR_LOOPBACK_INIT { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } }
extern const struct in6_addr in6addr_any, in6addr_loopback;

#define IN6_IS_ADDR_UNSPECIFIED(a) \
    (((const uint32_t *)(a))[0] == 0 && ((const uint32_t *)(a))[1] == 0 && \
     ((const uint32_t *)(a))[2] == 0 && ((const uint32_t *)(a))[3] == 0)
#define IN6_IS_ADDR_LOOPBACK(a) \
    (((const uint32_t *)(a))[0] == 0 && ((const uint32_t *)(a))[1] == 0 && \
     ((const uint32_t *)(a))[2] == 0 && \
     ((const unsigned char *)(a))[12] == 0 && ((const unsigned char *)(a))[13] == 0 && \
     ((const unsigned char *)(a))[14] == 0 && ((const unsigned char *)(a))[15] == 1)
#define IN6_IS_ADDR_MULTICAST(a) (((const unsigned char *)(a))[0] == 0xff)
#define IN6_IS_ADDR_LINKLOCAL(a) \
    (((const unsigned char *)(a))[0] == 0xfe && (((const unsigned char *)(a))[1] & 0xc0) == 0x80)
#define IN6_IS_ADDR_SITELOCAL(a) \
    (((const unsigned char *)(a))[0] == 0xfe && (((const unsigned char *)(a))[1] & 0xc0) == 0xc0)
#define IN6_IS_ADDR_V4MAPPED(a) \
    (((const uint32_t *)(a))[0] == 0 && ((const uint32_t *)(a))[1] == 0 && \
     ((const unsigned char *)(a))[8] == 0 && ((const unsigned char *)(a))[9] == 0 && \
     ((const unsigned char *)(a))[10] == 0xff && ((const unsigned char *)(a))[11] == 0xff)

#define INADDR_ANY 0x00000000U
#define INADDR_LOOPBACK 0x7f000001U

uint16_t htons(uint16_t v);
uint16_t ntohs(uint16_t v);
uint32_t htonl(uint32_t v);
uint32_t ntohl(uint32_t v);

/* Minimal stub resolver (libc/resolve.c): A record lookup.
 * Returns IPv4 in network byte order, 0 on failure. */
unsigned int resolve_a(const char *host);
