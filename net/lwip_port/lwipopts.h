#pragma once

#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
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
#define MEMP_NUM_PBUF                   32
#define MEMP_NUM_RAW_PCB                8
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_TCP_PCB                16
#define MEMP_NUM_TCP_PCB_LISTEN         8
#define MEMP_NUM_TCP_SEG                64
#define PBUF_POOL_SIZE                  96
#define PBUF_POOL_BUFSIZE               1536

#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_CHECKSUM_CTRL_PER_NETIF    0
#define LWIP_TIMERS                     1

#define LWIP_DHCP_DOES_ACD_CHECK        0
#define LWIP_DNS_SUPPORT_MDNS_QUERIES   0

#define TCP_MSS                         1460
#define TCP_SND_BUF                     (16 * TCP_MSS)
/* 16*MSS ≈ 23 KB receive window: comfortably under the 64 KB per-socket RX
 * ring (so the flow-control path never refuses) and the 32 KB NIC ring. */
#define TCP_WND                         (16 * TCP_MSS)


/* ── MaeroOS firewall: filter inbound IPv4 after ethernet/ARP demux ────── */
struct pbuf;
struct netif;
int firewall_ip4_input_hook(struct pbuf *p, struct netif *inp);
#define LWIP_HOOK_IP4_INPUT(pbuf, input_netif) \
    firewall_ip4_input_hook((pbuf), (input_netif))
