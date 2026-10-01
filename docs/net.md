# Networking: loopback and IPv6

The TCP/IP stack is lwIP 2.2 (`third_party/lwip`, BSD licence) in `NO_SYS`
mode, driven by `knetd` and by the socket calls themselves
(`net/socket.c`, `net/lwip_glue.c`). It runs dual-stack.

## Interfaces

| ifindex | name | addresses | notes |
|---|---|---|---|
| 1 | `lo` | `127.0.0.1/8`, `::1/128` | lwIP's loop netif; no driver |
| 2 | `eth0` | DHCPv4 lease; `fe80::/64` (EUI-64) and SLAAC `/64`s | e1000 or RTL8139 |

`lo` is registered first, so it is interface 1 and the NIC is 2, as on Linux.
Traffic to `127.0.0.0/8`, `::1` and to eth0's own addresses goes through
lwIP's loopback queues. `net_lwip_poll()` drains them (up to 8 rounds, so a
SYN, its SYN|ACK and the ACK can go through in one call) and wakes sleepers on
`io_activity`. `knetd` now always runs, because lo needs lwIP's timers even on
a machine without a NIC. `lo` shows up in rtnetlink (`ip link`, `ip addr`),
`SIOCGIFCONF`/`SIOCGIF*` and `/proc/net/dev` (with packet counters). It is not
listed in `/proc/netif`, which lists NICs only.

IPv6 on eth0 is SLAAC only. lwIP makes the link-local address and runs DAD,
sends router solicitations and configures each prefix from the router
advertisements. QEMU user networking advertises `fec0::/64` and answers at
`fec0::2` (the host) and `fec0::3` (DNS); recent QEMU versions turn this on by
default. Be careful with `-netdev user,ipv6=on` on its own: QEMU then turns
IPv4 off, so pass `ipv4=on,ipv6=on`. The NIC drivers accept all multicast
(e1000 `RCTL.MPE`, RTL8139 `MAR` all ones), so neighbour discovery reaches
lwIP. There is no DHCPv6. RDNSS is off, so the DHCPv4 lease still writes
`/etc/resolv.conf`.

Where you can see it:

- `/proc/net/if_inet6`: Linux format (address, ifindex, prefix length,
  scope, flags, name).
- rtnetlink `RTM_GETADDR` and `RTM_GETROUTE` with `AF_INET6` or `AF_UNSPEC`:
  addresses with lifetimes and `IFA_F_TENTATIVE` during DAD; routes for `::1`,
  each prefix and the default route via the RA's router.
- You can remove one of eth0's IPv6 addresses (`RTM_DELADDR`, so busybox
  `ip addr flush` works), and a later RA can bring a SLAAC address back. Adding
  one by hand (`RTM_NEWADDR` with `AF_INET6`) is `EOPNOTSUPP`.

## Sockets

- `AF_INET6` `SOCK_STREAM`/`SOCK_DGRAM` sockets are dual-stack unless
  `IPV6_V6ONLY` is set. Their pcbs are lwIP `IPADDR_TYPE_ANY`, IPv4 peers are
  named `::ffff:a.b.c.d`, and connecting or sending to such a name goes out
  over IPv4. A listener on `[::]` accepts both families. `IPV6_V6ONLY` can
  only be set before bind (`EINVAL` after it, as on Linux). With it set, a
  `[::]` listener and a `0.0.0.0` one can share a port.
- Names are 16 bytes for AF_INET and 28 bytes for AF_INET6. A 24-byte
  `sockaddr_in6` without a scope id is also accepted. A link-local address
  with no `sin6_scope_id` is taken to be on eth0.
- Routing follows Linux for IPv6. Without a router, a connect or send to a
  global IPv6 address that is not on one of eth0's prefixes fails at once with
  `ENETUNREACH`. Plain lwIP would send it out of eth0 from the link-local
  address and the connect would hang until it timed out. Failing fast is what
  lets Happy Eyeballs and the resolver's ordering move on to IPv4 without a
  delay.
- A connected UDP socket's `getsockname` reports the source address its
  datagrams would carry, as on Linux. musl's and glibc's RFC 6724 sorting read
  it.
- Ping sockets: `socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)` and
  `socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6)` work without privilege
  (Linux semantics with `ping_group_range` open). Sends must be echo requests.
  The kernel sets the identifier and the checksum. Receives are the matching
  echo replies (ICMP header plus data). The identifier is the socket's "port":
  a free one is picked from a random start, it belongs to one live socket of
  the family at a time (`bind` to a taken one is `EADDRINUSE`, as in Linux's
  `ping_get_port`), and only its owner receives the replies.
- `AF_INET6` `SOCK_RAW` (root only, like Linux): the payload after the IPv6
  header. `IPPROTO_ICMPV6` checksums are filled in by the kernel, and
  `ICMP6_FILTER` is accepted and ignored.
- `SO_LINGER` is stored and read back. With `l_onoff=1, l_linger=0`, `close()`
  resets the connection (RST, unsent data dropped). A non-zero timeout does not
  make `close()` block.
- `IPV6_UNICAST_HOPS` and `IP_TTL` read as 64. Other IPv6 options are accepted
  and ignored.

## Firewall

`net/firewall.c` also filters IPv6, inbound (`LWIP_HOOK_IP6_INPUT`) and
outbound. Rules name IPv4 networks, so a rule with an address never matches an
IPv6 packet. Rules for any address (`0.0.0.0/0`) and the policies apply to both
families. ICMPv6 counts as `icmp`. Neighbour discovery, RAs and MLD always
pass, as DHCP does for IPv4. `lo` is never filtered.

## Name resolution (`userspace/libc/netdb.c`)

A and AAAA queries for a name go out together. When there are several
results, they are sorted by part of RFC 6724's destination selection:

- Unreachable destinations go last (rule 1). Reachability is probed with a
  UDP `connect`, which sends nothing.
- Then by policy-table precedence (rule 6): `::1`, then global IPv6, then
  IPv4, then 6to4, ULAs and the deprecated site-local `fec0::/10`.

So `localhost` is `::1` then `127.0.0.1`. A dual-stack name on QEMU
user networking (`fec0::` and `10.0.2.x`) gives IPv4 first, as musl and glibc
order it. The resolver only reads IPv4 `nameserver` lines.

## Tests

`make smoke-net6` (part of `make check`, `tools/smoke_net6.py`, with
`userspace/net6probe`) checks:

- the SLAAC address and lo in the `/proc/net` files;
- TCP over `127.0.0.1` and `[::1]` between a guest server and a guest client;
- dual-stack and `IPV6_V6ONLY` listeners;
- UDP over `::1`;
- ping-socket echoes to `127.0.0.1` and `::1`;
- `toybox wget` from a host HTTP server at `http://[fec0::2]:port/`, which
  slirp hands to the host's `::1`;
- `getaddrinfo` of a dual-stack name served by `tools/dns_responder.py`;
- the firewall over IPv6.

`tools/smoke_alpine_net.py` also checks `ip -6 addr` and busybox
`ping -6 ::1` in the Alpine chroot.

## References

- RFC 4291 (IPv6 addressing, v4-mapped addresses), RFC 4861/4862 (neighbour
  discovery, SLAAC), RFC 3493 (basic socket API, `IPV6_V6ONLY`), RFC 6724
  (default address selection), RFC 4443 (ICMPv6), RFC 792 (ICMP echo),
  RFC 8200 (extension header order).
- The lwIP 2.2 documentation and sources (BSD licence) in `third_party/lwip`.
- Linux `ipv6(7)`, `raw(7)`, `socket(7)` and `icmp(7)` man pages for the ABI.
  The `/proc/net/if_inet6` format is from the procfs documentation. No GPL
  code was used.
