/*
 * firewall.c — a small first-match packet filter for MaeroOS.
 *
 * Inbound: hooked into lwIP at LWIP_HOOK_IP4_INPUT (after ethernet/ARP demux,
 *   before IP processing).  Returning non-zero tells lwIP we consumed the
 *   pbuf, so we pbuf_free() it ourselves (silent drop / stealth).
 * Outbound: firewall_check() is called from the socket layer on connect/
 *   sendto/bind, returning -EACCES to block (a clean, testable errno).
 *
 * IPv6 (LWIP_HOOK_IP6_INPUT, firewall_check6): rules name IPv4 networks, so
 * a rule with an address never matches an IPv6 packet, while rules for any
 * address and the policies apply to both families.  ICMPv6 counts as
 * "icmp"; neighbour discovery, router advertisements and MLD always pass,
 * as DHCP does for IPv4, or SLAAC would stop working under "policy in drop".
 *
 * Disabled by default — zero behaviour change until `fwctl enable`.
 */
#include "firewall.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"

#define FW_MAX_RULES 32

typedef struct {
    uint8_t  in_use;
    uint8_t  action;     /* FW_ALLOW / FW_DROP */
    uint8_t  dir;        /* FW_IN / FW_OUT / FW_ANYDIR */
    uint8_t  proto;      /* FW_TCP / FW_UDP / FW_ICMP / FW_ANYPROTO */
    uint32_t ip;         /* remote address, network order; 0 = any */
    uint32_t mask;       /* network mask, network order; 0 = any */
    uint8_t  any_port;   /* no port given: every port matches */
    uint16_t port_lo;    /* inclusive range; 0 is a port like any other */
    uint16_t port_hi;
    uint32_t hits;
} fw_rule_t;

static fw_rule_t rules[FW_MAX_RULES];
static int  fw_enabled    = 0;
static int  policy_in     = FW_ALLOW;   /* default inbound disposition */
static int  policy_out    = FW_ALLOW;

/* ── matching ───────────────────────────────────────────────────────────── */

static int rule_matches(const fw_rule_t *r, int dir, int proto,
                        uint32_t ip, uint16_t port, int v6) {
    if (!r->in_use) return 0;
    if (v6 && r->mask) return 0;          /* an IPv4 network */
    if (r->dir != FW_ANYDIR && r->dir != dir) return 0;
    if (r->proto != FW_ANYPROTO && r->proto != proto) return 0;
    if (r->mask && (ip & r->mask) != (r->ip & r->mask)) return 0;
    if (!r->any_port && (port < r->port_lo || port > r->port_hi)) return 0;
    return 1;
}

/* Returns FW_ALLOW or FW_DROP for a (dir,proto,ip,port) tuple. */
static int fw_decide_af(int dir, int proto, uint32_t ip, uint16_t port, int v6) {
    if (!fw_enabled) return FW_ALLOW;
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (rule_matches(&rules[i], dir, proto, ip, port, v6)) {
            rules[i].hits++;
            return rules[i].action;
        }
    }
    return (dir == FW_IN) ? policy_in : policy_out;
}

static int fw_decide(int dir, int proto, uint32_t ip, uint16_t port) {
    return fw_decide_af(dir, proto, ip, port, 0);
}

/* ── outbound (socket layer) ────────────────────────────────────────────── */

int firewall_check(int dir, int proto, uint32_t remote_ip, uint16_t port) {
    return fw_decide(dir, proto, remote_ip, port) == FW_DROP ? -13 : 0;
}

int firewall_check6(int dir, int proto, uint16_t port) {
    return fw_decide_af(dir, proto, 0, port, 1) == FW_DROP ? -13 : 0;
}

/* IPv6 inbound: the upper-layer protocol after the hop-by-hop, routing and
 * destination-options headers (RFC 8200 4.1); a fragment or anything else
 * is judged by the policy with port 0.  Returns 1 = consumed (dropped). */
int firewall_ip6_input_hook(struct pbuf *p, struct netif *inp) {
    if (!fw_enabled || !p || p->len < 40)
        return 0;
    if (inp && inp->name[0] == 'l' && inp->name[1] == 'o')
        return 0;
    const uint8_t *ip = (const uint8_t *)p->payload;
    uint8_t nh = ip[6];
    uint32_t off = 40;
    for (int i = 0; i < 8 && (nh == 0 || nh == 43 || nh == 60); i++) {
        if (p->len < off + 8) return 0;
        nh = ip[off];
        off += 8u + 8u * ip[off + 1];
    }
    int proto = nh == 58 ? FW_ICMP : nh;
    uint16_t dport = 0;
    if (nh == 58 && p->len >= off + 1) {
        uint8_t t = ip[off];
        /* MLD (130-132, 143), RS/RA/NS/NA/redirect (133-137). */
        if ((t >= 130 && t <= 137) || t == 143) return 0;
    } else if ((nh == FW_TCP || nh == FW_UDP) && p->len >= off + 4) {
        dport = (uint16_t)((ip[off + 2] << 8) | ip[off + 3]);
    }
    if (fw_decide_af(FW_IN, proto, 0, dport, 1) == FW_DROP) {
        pbuf_free(p);
        return 1;
    }
    return 0;
}

/* ── inbound (lwIP hook) ────────────────────────────────────────────────── */
/* Parse just enough of the IPv4 header to apply rules; on short/odd packets
 * fall back to the inbound policy.  Returns 1 = consumed (dropped). */
int firewall_ip4_input_hook(struct pbuf *p, struct netif *inp) {
    if (!fw_enabled || !p || p->len < 20)
        return 0;
    /* lo is always open (127.0.0.0/8 and our own addresses looped back). */
    if (inp && inp->name[0] == 'l' && inp->name[1] == 'o')
        return 0;

    const uint8_t *ip = (const uint8_t *)p->payload;
    uint8_t proto = ip[9];
    uint32_t src;
    memcpy(&src, ip + 12, 4);          /* source addr, network order */

    /* Always allow DHCP (UDP 67/68) so addressing keeps working. */
    if (proto == FW_UDP) {
        uint32_t ihl = (ip[0] & 0x0F) * 4;
        if (p->len >= ihl + 4) {
            const uint8_t *udp = ip + ihl;
            uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
            if (dport == 67 || dport == 68) return 0;
        }
    }

    uint16_t dport = 0;
    if (proto == FW_TCP || proto == FW_UDP) {
        uint32_t ihl = (ip[0] & 0x0F) * 4;
        if (p->len >= ihl + 4) {
            const uint8_t *l4 = ip + ihl;
            dport = (uint16_t)((l4[2] << 8) | l4[3]);   /* dest (local) port */
        }
    }

    if (fw_decide(FW_IN, proto, src, dport) == FW_DROP) {
        pbuf_free(p);                  /* stealth drop: lwIP sends no RST/ICMP */
        return 1;
    }
    return 0;
}

/* ── control plane ──────────────────────────────────────────────────────── */

/*
 * The control-plane parsers are strict: a rule that does not parse exactly is
 * rejected, never widened.  A mask or port that silently fell back to "any"
 * (an overflowing /bits wrapping to /0, a range starting at port 0, a typo in
 * the protocol word) turned an intended narrow allow into allow-everything.
 */

/* Decimal number no larger than `max`; -1 when there are no digits or it is
 * too big.  Advances *pp past the digits. */
static int parse_num(const char **pp, uint32_t max, uint32_t *out) {
    const char *p = *pp;
    uint32_t v = 0;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint32_t)(*p - '0');
        if (v > max) return -1;
        p++;
    }
    *pp = p;
    *out = v;
    return 0;
}

/* "a.b.c.d" → network-order u32; advances *pp.  -1 unless it is exactly four
 * dotted octets. */
static int parse_ip(const char **pp, uint32_t *out) {
    uint32_t b[4];
    for (int i = 0; i < 4; i++) {
        if (i && *(*pp)++ != '.') return -1;
        if (parse_num(pp, 255, &b[i]) < 0) return -1;
    }
    *out = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);  /* network order */
    return 0;
}

static int at_token_end(const char *p) {
    return *p == ' ' || *p == '\t' || *p == '\0' || *p == '\n';
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int word_is(const char **pp, const char *w) {
    const char *p = skip_ws(*pp);
    int n = (int)strlen(w);
    if (strncmp(p, w, (size_t)n) == 0 &&
        (p[n] == ' ' || p[n] == '\t' || p[n] == '\0' || p[n] == '\n')) {
        *pp = p + n;
        return 1;
    }
    return 0;
}

static int add_rule(int action, int dir, int proto, uint32_t ip,
                    uint32_t mask, int any_port, uint16_t lo, uint16_t hi) {
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (!rules[i].in_use) {
            rules[i].in_use = 1;
            rules[i].action = (uint8_t)action;
            rules[i].dir = (uint8_t)dir;
            rules[i].proto = (uint8_t)proto;
            rules[i].ip = ip; rules[i].mask = mask;
            rules[i].any_port = (uint8_t)any_port;
            rules[i].port_lo = lo; rules[i].port_hi = hi;
            rules[i].hits = 0;
            return 0;
        }
    }
    return -1;
}

int firewall_ctl_line(const char *line) {
    const char *p = skip_ws(line);

    if (*p == '#' || *p == '\0' || *p == '\n') return 0;
    if (word_is(&p, "enable"))  { fw_enabled = 1; return 0; }
    if (word_is(&p, "disable")) { fw_enabled = 0; return 0; }
    if (word_is(&p, "flush")) {
        memset(rules, 0, sizeof(rules));
        return 0;
    }
    if (word_is(&p, "policy")) {
        int dir = word_is(&p, "in") ? FW_IN : word_is(&p, "out") ? FW_OUT : -1;
        if (dir < 0) return -1;
        int act = word_is(&p, "drop") ? FW_DROP
                : word_is(&p, "allow") ? FW_ALLOW : -1;
        if (act < 0) return -1;
        if (dir == FW_IN) policy_in = act; else policy_out = act;
        return 0;
    }
    if (word_is(&p, "rule")) {
        int act = word_is(&p, "allow") ? FW_ALLOW
                : word_is(&p, "drop") ? FW_DROP : -1;
        if (act < 0) return -1;
        /* Direction and protocol words are optional (default any), but
         * anything else in their place is an error, not a default. */
        int dir = word_is(&p, "in") ? FW_IN
                : word_is(&p, "out") ? FW_OUT
                : word_is(&p, "any") ? FW_ANYDIR : FW_ANYDIR;
        int proto = word_is(&p, "tcp") ? FW_TCP
                  : word_is(&p, "udp") ? FW_UDP
                  : word_is(&p, "icmp") ? FW_ICMP
                  : word_is(&p, "any") ? FW_ANYPROTO : FW_ANYPROTO;
        p = skip_ws(p);
        uint32_t ip = 0, mask = 0;
        uint32_t lo = 0, hi = 0;
        int any_port = 1;
        if (*p >= '0' && *p <= '9') {
            uint32_t bits = 32;
            if (parse_ip(&p, &ip) < 0) return -1;
            if (*p == '/') {
                p++;
                if (parse_num(&p, 32, &bits) < 0) return -1;
            }
            if (!at_token_end(p)) return -1;
            mask = bits == 32 ? 0xFFFFFFFFU
                 : bits == 0  ? 0
                 : __builtin_bswap32(0xFFFFFFFFU << (32 - bits));
        }
        p = skip_ws(p);
        if (*p >= '0' && *p <= '9') {
            if (parse_num(&p, 65535, &lo) < 0) return -1;
            hi = lo;
            if (*p == '-') {
                p++;
                if (parse_num(&p, 65535, &hi) < 0 || hi < lo) return -1;
            }
            any_port = 0;
        }
        p = skip_ws(p);
        if (*p != '\0' && *p != '\n') return -1;   /* trailing junk */
        return add_rule(act, dir, proto, ip, mask, any_port,
                        (uint16_t)lo, (uint16_t)hi);
    }
    return -1;
}

uint32_t firewall_dump(char *buf, uint32_t cap) {
    uint32_t off = 0;
    int n;

    n = snprintf(buf + off, cap - off,
                  "firewall: %s  policy in=%s out=%s\n",
                  fw_enabled ? "enabled" : "disabled",
                  policy_in == FW_DROP ? "drop" : "allow",
                  policy_out == FW_DROP ? "drop" : "allow");
    if (n > 0) off += (uint32_t)n;
    for (int i = 0; i < FW_MAX_RULES && off < cap; i++) {
        if (!rules[i].in_use) continue;
        uint32_t ip = rules[i].ip;
        uint32_t m = __builtin_bswap32(rules[i].mask);
        int bits = 0;
        while (m & 0x80000000U) { bits++; m <<= 1; }
        char ports[16];
        if (rules[i].any_port)
            snprintf(ports, sizeof(ports), "any");
        else
            snprintf(ports, sizeof(ports), "%u-%u",
                     rules[i].port_lo, rules[i].port_hi);
        n = snprintf(buf + off, cap - off,
                      "  %s %s %s %u.%u.%u.%u/%d port %s  hits=%u\n",
                      rules[i].action == FW_DROP ? "drop" : "allow",
                      rules[i].dir == FW_IN ? "in" :
                      rules[i].dir == FW_OUT ? "out" : "any",
                      rules[i].proto == FW_TCP ? "tcp" :
                      rules[i].proto == FW_UDP ? "udp" :
                      rules[i].proto == FW_ICMP ? "icmp" : "any",
                      ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF,
                      (ip >> 24) & 0xFF, bits, ports, rules[i].hits);
        if (n > 0) off += (uint32_t)n;
    }
    /* snprintf returns the untruncated length; report only what fit (it
     * NUL-terminates at cap-1), or callers index/copy past buf. */
    if (cap && off >= cap) off = cap - 1;
    return off;
}
