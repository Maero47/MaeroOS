/*
 * P74 raw sockets: messages too short for their headers are refused, not
 * sent (and do not stop the machine).
 *
 * Linux: an ICMPv6 raw socket (the kernel fills the checksum at offset 2)
 * refuses a message of fewer than 4 bytes with EINVAL; a 4-byte one goes out.
 * An IPv4 raw socket with IP_HDRINCL refuses a message shorter than an IP
 * header with EINVAL.
 *
 * MaeroOS before: the ICMPv6 checksum was zeroed past the end of the packet
 * buffer, then lwIP's raw_sendto asserted ("Checksum must fit into first
 * pbuf") and LWIP_ASSERT halted the machine.
 *
 * Needs root (raw sockets); SKIP otherwise.
 */
#define PROBE_NAME "p74_raw_short_send"
#include "probe.h"
#include <netinet/in.h>
#include <sys/socket.h>

int main(void)
{
    probe_watchdog(60);
    int fd = socket(AF_INET6, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMPV6);
    if (fd < 0 && (errno == EPERM || errno == EACCES))
        probe_skip("raw sockets need root");
    if (fd < 0) probe_fail("socket(AF_INET6, SOCK_RAW, ICMPV6): %s", strerror(errno));
    struct sockaddr_in6 to = { .sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT };
    unsigned char msg[8] = { 128, 0, 0, 0, 0x12, 0x34, 0, 1 };   /* echo request */
    for (int len = 0; len < 4; len++) {
        ssize_t n = sendto(fd, msg, (size_t)len, 0, (struct sockaddr *)&to, sizeof to);
        if (n >= 0 || errno != EINVAL)
            probe_fail("ICMPv6 sendto of %d bytes: %zd (%s), want EINVAL", len, n,
                       n < 0 ? strerror(errno) : "sent");
    }
    ssize_t n = sendto(fd, msg, sizeof msg, 0, (struct sockaddr *)&to, sizeof to);
    if (n != (ssize_t)sizeof msg)
        probe_fail("ICMPv6 echo request of 8 bytes: %zd (%s)", n, strerror(errno));
    close(fd);

    fd = socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_RAW);   /* IP_HDRINCL */
    if (fd < 0) probe_fail("socket(AF_INET, SOCK_RAW, IPPROTO_RAW): %s", strerror(errno));
    struct sockaddr_in to4 = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001) };
    unsigned char ip[20] = { 0x45 };
    for (int len = 0; len < 20; len += 7) {
        n = sendto(fd, ip, (size_t)len, 0, (struct sockaddr *)&to4, sizeof to4);
        if (n >= 0 || errno != EINVAL)
            probe_fail("IP_HDRINCL sendto of %d bytes: %zd (%s), want EINVAL", len, n,
                       n < 0 ? strerror(errno) : "sent");
    }
    close(fd);
    probe_pass();
}
