#ifndef MAERO_FIREWALL_H
#define MAERO_FIREWALL_H

#include <stdint.h>

/* Direction / action / protocol selectors for a rule. */
#define FW_IN        1
#define FW_OUT       2
#define FW_ANYDIR    3
#define FW_ALLOW     0
#define FW_DROP      1
#define FW_TCP       6      /* matches IP protocol numbers */
#define FW_UDP       17
#define FW_ICMP      1
#define FW_ANYPROTO  0

/*
 * Outbound egress check, called from the socket layer.  proto = FW_TCP/FW_UDP,
 * remote_ip is network byte order, port is the remote port (host order).
 * Returns 0 to allow, -13 (-EACCES) to block.
 */
int firewall_check(int dir, int proto, uint32_t remote_ip, uint16_t port);

/* Apply one control line (from /proc/firewall writes or fwctl).  Accepts:
 *   enable | disable | flush
 *   policy in allow|drop   /   policy out allow|drop
 *   rule allow|drop in|out tcp|udp|icmp|any <ip>/<maskbits> <lo>[-<hi>]
 * Returns 0 on success, -1 on parse error. */
int firewall_ctl_line(const char *line);

/* Render the current ruleset + hit counters into buf (for /proc/firewall). */
uint32_t firewall_dump(char *buf, uint32_t cap);

#endif
