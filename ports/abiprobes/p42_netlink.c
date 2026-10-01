/*
 * P42 rtnetlink: an RTM_GETLINK dump, and the ioctls agreeing with it.
 *
 * Linux: socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE), bind with pid 0
 * autobinds a port id getsockname reports; an NLM_F_DUMP request is answered
 * with NLM_F_MULTI RTM_NEWLINK messages carrying the request's seq and the
 * port id, ended by NLMSG_DONE; a request with NLM_F_ACK for an unknown
 * interface gets NLMSG_ERROR with -ENODEV.  SIOCGIFINDEX and SIOCGIFNAME on
 * an ordinary socket agree with the dump, and SIOCGIFCONF lists only
 * interfaces with an IPv4 address.
 *
 * MaeroOS: AF_NETLINK was EAFNOSUPPORT and the SIOC* ioctls ENOTTY, so
 * busybox ip, ifconfig and udhcpc could not see the network.
 */
#define PROBE_NAME "p42_netlink"
#include "probe.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

int main(void)
{
    probe_watchdog(60);
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) probe_fail("socket(AF_NETLINK): %s", strerror(errno));
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0)
        probe_fail("bind: %s", strerror(errno));
    socklen_t sl = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) != 0 || sl != sizeof sa ||
        sa.nl_family != AF_NETLINK || sa.nl_pid == 0)
        probe_fail("getsockname: no port id after bind");

    struct { struct nlmsghdr h; struct ifinfomsg i; } req;
    memset(&req, 0, sizeof req);
    req.h.nlmsg_len = sizeof req;
    req.h.nlmsg_type = RTM_GETLINK;
    req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.h.nlmsg_seq = 4242;
    if (send(fd, &req, sizeof req, 0) != (ssize_t)sizeof req)
        probe_fail("send dump: %s", strerror(errno));

    static char buf[65536];
    int links = 0, done = 0, first_index = 0;
    char first_name[IF_NAMESIZE] = "";
    while (!done) {
        struct sockaddr_nl from;
        struct iovec iov = { buf, sizeof buf };
        struct msghdr mh = { .msg_name = &from, .msg_namelen = sizeof from,
                             .msg_iov = &iov, .msg_iovlen = 1 };
        ssize_t n = recvmsg(fd, &mh, 0);
        if (n <= 0) probe_fail("recvmsg: %s", n ? strerror(errno) : "EOF");
        if (mh.msg_namelen != sizeof from || from.nl_pid != 0)
            probe_fail("reply not from the kernel (port 0)");
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)n);
             h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_seq != 4242 || h->nlmsg_pid != sa.nl_pid)
                probe_fail("reply seq %u pid %u, want 4242 %u",
                           h->nlmsg_seq, h->nlmsg_pid, sa.nl_pid);
            if (h->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (h->nlmsg_type != RTM_NEWLINK || !(h->nlmsg_flags & NLM_F_MULTI))
                probe_fail("dump message type %u flags %#x", h->nlmsg_type,
                           h->nlmsg_flags);
            struct ifinfomsg *ifi = NLMSG_DATA(h);
            int len = IFLA_PAYLOAD(h);
            for (struct rtattr *a = IFLA_RTA(ifi); RTA_OK(a, len); a = RTA_NEXT(a, len))
                if (a->rta_type == IFLA_IFNAME && !links) {
                    snprintf(first_name, sizeof first_name, "%s", (char *)RTA_DATA(a));
                    first_index = ifi->ifi_index;
                }
            links++;
        }
    }
    if (!links || !first_name[0])
        probe_fail("the dump listed no interface with a name");
    probe_info("%d interface(s), first %s index %d", links, first_name, first_index);

    /* An ACKed request for a missing interface: NLMSG_ERROR -ENODEV. */
    memset(&req, 0, sizeof req);
    req.h.nlmsg_len = sizeof req;
    req.h.nlmsg_type = RTM_GETLINK;
    req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.h.nlmsg_seq = 4343;
    req.i.ifi_index = 999999;
    if (send(fd, &req, sizeof req, 0) != (ssize_t)sizeof req)
        probe_fail("send get: %s", strerror(errno));
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    struct nlmsghdr *h = (struct nlmsghdr *)buf;
    if (n < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr)) ||
        h->nlmsg_type != NLMSG_ERROR || h->nlmsg_seq != 4343 ||
        ((struct nlmsgerr *)NLMSG_DATA(h))->error != -ENODEV)
        probe_fail("GETLINK of a missing index: want NLMSG_ERROR -ENODEV");
    close(fd);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) probe_fail("socket(AF_INET): %s", strerror(errno));
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", first_name);
    if (ioctl(s, SIOCGIFINDEX, &ifr) != 0 || ifr.ifr_ifindex != first_index)
        probe_fail("SIOCGIFINDEX %s: %s / %d, want %d", first_name,
                   strerror(errno), ifr.ifr_ifindex, first_index);
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_ifindex = first_index;
    if (ioctl(s, SIOCGIFNAME, &ifr) != 0 || strcmp(ifr.ifr_name, first_name) != 0)
        probe_fail("SIOCGIFNAME %d: %s", first_index, strerror(errno));
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "nosuchif0");
    if (ioctl(s, SIOCGIFINDEX, &ifr) == 0 || errno != ENODEV)
        probe_fail("SIOCGIFINDEX of a missing interface: want ENODEV");
    struct ifconf ifc = { .ifc_len = 0, .ifc_buf = NULL };
    if (ioctl(s, SIOCGIFCONF, &ifc) != 0 || ifc.ifc_len % sizeof(struct ifreq))
        probe_fail("SIOCGIFCONF size query: %s", strerror(errno));
    probe_info("SIOCGIFCONF: %d IPv4 interface(s)", ifc.ifc_len / (int)sizeof(struct ifreq));
    close(s);
    probe_pass();
}
