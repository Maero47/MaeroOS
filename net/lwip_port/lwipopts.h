#pragma once

#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0

#define LWIP_IPV4                       1
/* Dual stack: IPv6 with SLAAC from router advertisements (QEMU slirp
 * -netdev user,ipv6=on announces fec0::/64), MLD for the solicited-node
 * groups, and ICMPv6.  No DHCPv6; RDNSS stays off so the DHCPv4 lease keeps
 * owning /etc/resolv.conf (lwIP would otherwise overwrite DNS server 0). */
#define LWIP_IPV6                       1
#define LWIP_IPV6_AUTOCONFIG            1
#define LWIP_IPV6_MLD                   1
#define LWIP_ND6_RDNSS_MAX_DNS_SERVERS  0
#define LWIP_IPV6_DHCP6                 0
#define LWIP_IPV6_NUM_ADDRESSES         4
#define MEMP_NUM_MLD6_GROUP             8
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DHCP                       1
#define LWIP_DNS                        1
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0

#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0

#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (256 * 1024)
/* Pool sizing follows the socket table (net/socket.c, 128 slots): Firefox
 * opens UDP sockets for DNS (musl's resolver, one per lookup, several lookups
 * in flight) and QUIC/connectivity probes next to its TCP connections; with 16
 * UDP pcbs socket() failed ("[NET] socket: lwIP has no free UDP pcb") ~33
 * times per smoke-firefox boot.  A udp_pcb is ~40 bytes, so 64 costs ~2.5 KiB.
 * Per-pool .bss is in the memp_memory_*_base symbols of kernel.elf. */
#define MEMP_NUM_PBUF                   64
#define MEMP_NUM_RAW_PCB                8
#define MEMP_NUM_UDP_PCB                64
/* Firefox keeps background TLS sessions to Mozilla services open while it
 * loads a page, and closed connections linger in FIN_WAIT/TIME_WAIT; with 16
 * pcbs tcp_new() failed ("[NET] socket: lwIP has no free TCP pcb") and an
 * image on the page was never requested, in 4 of 20 smoke-firefox --web boots. */
#define MEMP_NUM_TCP_PCB                64
#define MEMP_NUM_TCP_PCB_LISTEN         8
/* Queued TX segments across all pcbs (a tcp_seg is 20 bytes; the data sits
 * in MEM_SIZE).  One full TCP_SND_BUF is TCP_SND_QUEUELEN (64) segments, so
 * 512 lets 8 connections have a full send buffer queued at once. */
#define MEMP_NUM_TCP_SEG                512
/* NIC RX and out-of-order segments: 128 x 1.5 KiB = 192 KiB (96 was 144). */
#define PBUF_POOL_SIZE                  128
#define PBUF_POOL_BUFSIZE               1536

/* lo: lwIP's loop netif (127.0.0.1/8 and ::1), and packets an interface
 * sends to one of its own addresses come back through its loop queue.
 * Both queues are drained by netif_poll_all() in net_lwip_poll (NO_SYS). */
#define LWIP_NETIF_LOOPBACK             1
#define LWIP_HAVE_LOOPIF                1
#define LWIP_LOOPIF_MULTICAST           1
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 0
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_CHECKSUM_CTRL_PER_NETIF    0
#define LWIP_TIMERS                     1

#define LWIP_DHCP_DOES_ACD_CHECK        0
#define LWIP_DNS_SUPPORT_MDNS_QUERIES   0

/* Server sockets (net/socket.c listen/accept): SO_REUSEADDR lets a server
 * rebind its port while old connections sit in TIME_WAIT; the backlog makes
 * a listener with a full accept queue ignore new SYNs (the client retries),
 * as Linux does; keepalive takes TCP_KEEPIDLE/KEEPINTVL/KEEPCNT per pcb. */
#define SO_REUSE                        1
#define TCP_LISTEN_BACKLOG              1
#define LWIP_TCP_KEEPALIVE              1

#define TCP_MSS                         1460
#define TCP_SND_BUF                     (16 * TCP_MSS)
/* 16*MSS ≈ 23 KB receive window: comfortably under the 64 KB per-socket RX
 * ring (so the flow-control path never refuses) and the 32 KB NIC ring. */
#define TCP_WND                         (16 * TCP_MSS)


/* ── MaeroOS firewall: filter inbound IPv4/IPv6 after the ethernet demux ── */
struct pbuf;
struct netif;
int firewall_ip4_input_hook(struct pbuf *p, struct netif *inp);
#define LWIP_HOOK_IP4_INPUT(pbuf, input_netif) \
    firewall_ip4_input_hook((pbuf), (input_netif))
int firewall_ip6_input_hook(struct pbuf *p, struct netif *inp);
#define LWIP_HOOK_IP6_INPUT(pbuf, input_netif) \
    firewall_ip6_input_hook((pbuf), (input_netif))
