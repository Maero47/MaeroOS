/*
 * firewall.c — a small first-match packet filter for MaeroOS.
 *
 * Inbound: hooked into lwIP at LWIP_HOOK_IP4_INPUT (after ethernet/ARP demux,
 *   before IP processing).  Returning non-zero tells lwIP we consumed the
 *   pbuf, so we pbuf_free() it ourselves (silent drop / stealth).
 * Outbound: firewall_check() is called from the socket layer on connect/
 *   sendto/bind, returning -EACCES to block (a clean, testable errno).
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
    uint16_t port_lo;    /* 0 = any */
    uint16_t port_hi;
    uint32_t hits;
} fw_rule_t;

static fw_rule_t rules[FW_MAX_RULES];
static int  fw_enabled    = 0;
static int  policy_in     = FW_ALLOW;   /* default inbound disposition */
static int  policy_out    = FW_ALLOW;

/* ── matching ───────────────────────────────────────────────────────────── */

static int rule_matches(const fw_rule_t *r, int dir, int proto,
                        uint32_t ip, uint16_t port) {
    if (!r->in_use) return 0;
    if (r->dir != FW_ANYDIR && r->dir != dir) return 0;
    if (r->proto != FW_ANYPROTO && r->proto != proto) return 0;
    if (r->mask && (ip & r->mask) != (r->ip & r->mask)) return 0;
    if (r->port_lo && (port < r->port_lo || port > r->port_hi)) return 0;
    return 1;
}

/* Returns FW_ALLOW or FW_DROP for a (dir,proto,ip,port) tuple. */
static int fw_decide(int dir, int proto, uint32_t ip, uint16_t port) {
    if (!fw_enabled) return FW_ALLOW;
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (rule_matches(&rules[i], dir, proto, ip, port)) {
            rules[i].hits++;
            return rules[i].action;
        }
    }
    return (dir == FW_IN) ? policy_in : policy_out;
}

/* ── outbound (socket layer) ────────────────────────────────────────────── */

int firewall_check(int dir, int proto, uint32_t remote_ip, uint16_t port) {
    return fw_decide(dir, proto, remote_ip, port) == FW_DROP ? -13 : 0;
}

/* ── inbound (lwIP hook) ────────────────────────────────────────────────── */
/* Parse just enough of the IPv4 header to apply rules; on short/odd packets
 * fall back to the inbound policy.  Returns 1 = consumed (dropped). */
int firewall_ip4_input_hook(struct pbuf *p, struct netif *inp) {
    (void)inp;
    if (!fw_enabled || !p || p->len < 20)
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

static uint32_t parse_u32(const char **pp) {
    const char *p = *pp;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; }
    *pp = p;
    return v;
}

/* "a.b.c.d" → network-order u32; advances *pp. */
static uint32_t parse_ip(const char **pp) {
    uint32_t b0 = parse_u32(pp); if (**pp == '.') (*pp)++;
    uint32_t b1 = parse_u32(pp); if (**pp == '.') (*pp)++;
    uint32_t b2 = parse_u32(pp); if (**pp == '.') (*pp)++;
    uint32_t b3 = parse_u32(pp);
    return (b0) | (b1 << 8) | (b2 << 16) | (b3 << 24);  /* network order */
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
                    uint32_t mask, uint16_t lo, uint16_t hi) {
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (!rules[i].in_use) {
            rules[i].in_use = 1;
            rules[i].action = (uint8_t)action;
            rules[i].dir = (uint8_t)dir;
            rules[i].proto = (uint8_t)proto;
            rules[i].ip = ip; rules[i].mask = mask;
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
        int dir = word_is(&p, "in") ? FW_IN
                : word_is(&p, "out") ? FW_OUT
                : word_is(&p, "any") ? FW_ANYDIR : FW_ANYDIR;
        int proto = word_is(&p, "tcp") ? FW_TCP
                  : word_is(&p, "udp") ? FW_UDP
                  : word_is(&p, "icmp") ? FW_ICMP
                  : word_is(&p, "any") ? FW_ANYPROTO : FW_ANYPROTO;
        p = skip_ws(p);
        uint32_t ip = 0, mask = 0;
        uint16_t lo = 0, hi = 0;
        if (*p >= '0' && *p <= '9') {
            ip = parse_ip(&p);
            int bits = 32;
            if (*p == '/') { p++; bits = (int)parse_u32(&p); }
            mask = bits >= 32 ? 0xFFFFFFFFU
                 : bits <= 0  ? 0
                 : __builtin_bswap32(0xFFFFFFFFU << (32 - bits));
        }
        p = skip_ws(p);
        if (*p >= '0' && *p <= '9') {
            lo = (uint16_t)parse_u32(&p);
            hi = lo;
            if (*p == '-') { p++; hi = (uint16_t)parse_u32(&p); }
        }
        return add_rule(act, dir, proto, ip, mask, lo, hi);
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
        n = snprintf(buf + off, cap - off,
                      "  %s %s %s %u.%u.%u.%u port %u-%u  hits=%u\n",
                      rules[i].action == FW_DROP ? "drop" : "allow",
                      rules[i].dir == FW_IN ? "in" :
                      rules[i].dir == FW_OUT ? "out" : "any",
                      rules[i].proto == FW_TCP ? "tcp" :
                      rules[i].proto == FW_UDP ? "udp" :
                      rules[i].proto == FW_ICMP ? "icmp" : "any",
                      ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF,
                      (ip >> 24) & 0xFF,
                      rules[i].port_lo, rules[i].port_hi, rules[i].hits);
        if (n > 0) off += (uint32_t)n;
    }
    return off;
}
