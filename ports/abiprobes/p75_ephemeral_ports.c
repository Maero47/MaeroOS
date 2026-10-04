/*
 * P75 ephemeral ports are not handed out in sequence.
 *
 * Linux picks an unbound socket's local port at random-ish offsets (RFC 6056;
 * __inet_hash_connect / udp_lib_get_port), so a DNS reply forger cannot know
 * the next query's source port.
 *
 * MaeroOS before: lwIP's allocators counted up from an initial port seeded by
 * LWIP_RAND(), which was the constant 4: every boot used the same ports, one
 * after the other.
 *
 * Sixteen UDP sockets connected (implicitly bound) to 127.0.0.1:53 and
 * sixteen TCP connections to a local listener: no more than a few of the
 * consecutive pairs may be +1 apart.
 */
#define PROBE_NAME "p75_ephemeral_ports"
#include "probe.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define N 16

static int seq_pairs(const unsigned *p, int n)
{
    int seq = 0;
    for (int i = 1; i < n; i++)
        if (p[i] == p[i - 1] + 1) seq++;
    return seq;
}

static unsigned local_port(int fd)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &al) != 0) return 0;
    return ntohs(a.sin_port);
}

int main(void)
{
    probe_watchdog(60);
    struct sockaddr_in lo = { .sin_family = AF_INET, .sin_port = htons(53),
                              .sin_addr.s_addr = htonl(0x7f000001) };
    unsigned up[N], tp[N];
    int fds[N];
    for (int i = 0; i < N; i++) {
        fds[i] = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fds[i] < 0) probe_fail("udp socket: %s", strerror(errno));
        if (connect(fds[i], (struct sockaddr *)&lo, sizeof lo) != 0)
            probe_fail("udp connect: %s", strerror(errno));
        up[i] = local_port(fds[i]);
    }
    for (int i = 0; i < N; i++) close(fds[i]);

    int L = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in la = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001) };
    if (L < 0 || bind(L, (struct sockaddr *)&la, sizeof la) != 0 || listen(L, N) != 0)
        probe_fail("listener: %s", strerror(errno));
    socklen_t lal = sizeof la;
    getsockname(L, (struct sockaddr *)&la, &lal);
    for (int i = 0; i < N; i++) {
        fds[i] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fds[i] < 0) probe_fail("tcp socket: %s", strerror(errno));
        if (connect(fds[i], (struct sockaddr *)&la, sizeof la) != 0)
            probe_fail("tcp connect: %s", strerror(errno));
        tp[i] = local_port(fds[i]);
        int c = accept(L, NULL, NULL);
        if (c >= 0) close(c);
    }
    for (int i = 0; i < N; i++) close(fds[i]);
    close(L);

    int su = seq_pairs(up, N), st = seq_pairs(tp, N);
    probe_info("udp ports %u %u %u %u ... (%d sequential pairs), tcp %u %u %u %u ... (%d)",
               up[0], up[1], up[2], up[3], su, tp[0], tp[1], tp[2], tp[3], st);
    if (su > 3) probe_fail("UDP ephemeral ports in sequence (%d of %d pairs)", su, N - 1);
    if (st > 3) probe_fail("TCP ephemeral ports in sequence (%d of %d pairs)", st, N - 1);
    probe_pass();
}
