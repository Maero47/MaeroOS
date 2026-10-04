/*
 * Name resolution: getaddrinfo/getnameinfo, gethostby*, getservby*, the
 * inet_* conversions and the res_* subset toybox host(1) uses.
 *
 * Lookups go to /etc/hosts first (and "localhost" is always the loopback
 * address), then to DNS over UDP using /etc/resolv.conf:
 *
 *   nameserver 10.0.2.3          up to 3 lines, IPv4
 *   nameserver [10.0.2.2]:5353   OpenBSD's syntax for a non-default port
 *   search a.example b.example   (or "domain x") tried for short names
 *   options timeout:2 attempts:2 ndots:1
 *
 * With no usable nameserver line the server is 10.0.2.3, QEMU user-net's
 * DNS.  A and AAAA queries for one name go out together.  For several
 * addresses the order is RFC 6724's destination selection in part: a
 * destination this host has no route to goes last (rule 1, probed with a
 * UDP connect, which sends nothing), then by the policy table's precedence
 * (rule 6: ::1, then global IPv6, then IPv4; 6to4, ULAs and the deprecated
 * site-local fec0::/10 after IPv4), as musl and glibc do.  Replies with TC
 * set (truncated, needs TCP) are used as far as they go.
 *
 * Written for MaeroOS from RFC 1035 (messages, compression), RFC 3596
 * (AAAA, ip6.arpa), RFC 3493 (getaddrinfo/getnameinfo) and RFC 5952 (IPv6
 * text form); no code was copied.
 */
#include "../include/arpa/inet.h"
#include "../include/arpa/nameser.h"
#include "../include/errno.h"
#include "../include/fcntl.h"
#include "../include/netdb.h"
#include "../include/netinet/in.h"
#include "../include/poll.h"
#include "../include/resolv.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/strings.h"
#include "../include/sys/socket.h"
#include "../include/time.h"
#include "../include/unistd.h"

#define MAXNS        3
#define MAXADDRS     16
#define MAXSEARCH    6
#define DNS_BUFSZ    1232       /* EDNS-safe UDP payload; we never ask for EDNS */

struct resconf {
    struct sockaddr_in ns[MAXNS];
    int nns;
    int timeout_ms;             /* per attempt */
    int attempts;
    int ndots;
    char search[256];           /* space-separated */
};

struct addr {
    int family;
    unsigned char a[16];
};

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

/* ── small helpers ─────────────────────────────────────────────────────── */

static int ci_eq(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Read a whole small text file; NUL-terminated, -1 if missing. */
static int read_file(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = 0;
    while (n < cap - 1) {
        int r = read(fd, buf + n, (size_t)(cap - 1 - n));
        if (r <= 0) break;
        n += r;
    }
    close(fd);
    buf[n] = 0;
    return n;
}

/* Next line of `*p` (modified in place: NUL-terminated, '#' comment cut). */
static char *next_line(char **p) {
    char *s = *p;
    if (!s || !*s) return 0;
    char *e = s;
    while (*e && *e != '\n') e++;
    if (*e) *e++ = 0;
    *p = e;
    char *h = strchr(s, '#');
    if (h) *h = 0;
    return s;
}

/* Next whitespace-separated token of `*p`, NUL-terminated in place. */
static char *next_tok(char **p) {
    char *s = *p;
    while (*s && is_space(*s)) s++;
    if (!*s) { *p = s; return 0; }
    char *e = s;
    while (*e && !is_space(*e)) e++;
    if (*e) *e++ = 0;
    *p = e;
    return s;
}

/* ── inet_* ────────────────────────────────────────────────────────────── */

/* inet_aton: a, a.b, a.b.c or a.b.c.d, each part decimal, 0x hex or 0 octal. */
int inet_aton(const char *s, struct in_addr *out) {
    unsigned long long parts[4];
    int n = 0;
    const char *p = s;

    if (!s) return 0;
    for (;;) {
        char *end;
        if (*p < '0' || *p > '9') return 0;
        /* 64-bit and saturating: a part past 2^32 must fail the range
         * checks below, not wrap into them ("4294967423.0.0.1"). */
        parts[n] = strtoull(p, &end, 0);
        if (end == p) return 0;
        n++;
        p = end;
        if (*p == '.') {
            if (n == 4) return 0;
            p++;
            continue;
        }
        if (*p) return 0;
        break;
    }
    uint32_t v;
    switch (n) {
    case 1:
        if (parts[0] > 0xFFFFFFFFULL) return 0;
        v = (uint32_t)parts[0];
        break;
    case 2:
        if (parts[0] > 255 || parts[1] > 0xFFFFFFUL) return 0;
        v = (uint32_t)(parts[0] << 24 | parts[1]);
        break;
    case 3:
        if (parts[0] > 255 || parts[1] > 255 || parts[2] > 0xFFFFUL) return 0;
        v = (uint32_t)(parts[0] << 24 | parts[1] << 16 | parts[2]);
        break;
    default:
        for (int i = 0; i < 4; i++) if (parts[i] > 255) return 0;
        v = (uint32_t)(parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3]);
        break;
    }
    if (out) out->s_addr = htonl(v);
    return 1;
}

char *inet_ntoa(struct in_addr in) {
    static char buf[INET_ADDRSTRLEN];
    return (char *)inet_ntop(AF_INET, &in, buf, sizeof(buf));
}

static int pton4(const char *s, unsigned char *out) {
    int n = 0;
    while (n < 4) {
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (digits && v == 0) return 0;      /* no leading zeros */
            v = v * 10 + (*s++ - '0');
            if (v > 255 || ++digits > 3) return 0;
        }
        if (!digits) return 0;
        out[n++] = (unsigned char)v;
        if (n < 4) {
            if (*s++ != '.') return 0;
        }
    }
    return *s == 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int pton6(const char *s, unsigned char *out) {
    uint16_t w[8];
    int n = 0, gap = -1;

    if (s[0] == ':') {
        if (s[1] != ':') return 0;
        s++;                                    /* leading "::" */
    }
    while (*s) {
        if (*s == ':') {
            if (gap >= 0) return 0;             /* a second "::" */
            gap = n;
            s++;
            if (!*s) break;
            continue;
        }
        /* An IPv4 tail ("::ffff:1.2.3.4") takes the last two words. */
        const char *q = s;
        while (hexval(*q) >= 0) q++;
        if (*q == '.') {
            unsigned char v4[4];
            if (n > 6 || !pton4(s, v4)) return 0;
            w[n++] = (uint16_t)(v4[0] << 8 | v4[1]);
            w[n++] = (uint16_t)(v4[2] << 8 | v4[3]);
            s += strlen(s);
            break;
        }
        int v = 0, digits = 0;
        while (hexval(*s) >= 0) {
            v = v * 16 + hexval(*s++);
            if (++digits > 4) return 0;
        }
        if (!digits || n == 8) return 0;
        w[n++] = (uint16_t)v;
        if (*s == ':') {
            s++;
            if (*s == ':') {
                if (gap >= 0) return 0;
                gap = n;
                s++;
            } else if (!*s) {
                return 0;                       /* trailing single ':' */
            }
        } else if (*s) {
            return 0;
        }
    }
    if (gap >= 0) {
        if (n == 8) return 0;
        int tail = n - gap;
        for (int i = 0; i < tail; i++) w[7 - i] = w[n - 1 - i];
        for (int i = gap; i < 8 - tail; i++) w[i] = 0;
    } else if (n != 8) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        out[i * 2] = (unsigned char)(w[i] >> 8);
        out[i * 2 + 1] = (unsigned char)w[i];
    }
    return 1;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af == AF_INET) return pton4(src, dst);
    if (af == AF_INET6) return pton6(src, dst);
    errno = EAFNOSUPPORT;
    return -1;
}

static int fmt_u(char *p, unsigned v) {
    char t[12];
    int n = 0, len = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) p[len++] = t[--n];
    return len;
}

static int fmt_hex(char *p, unsigned v) {
    static const char hx[] = "0123456789abcdef";
    char t[8];
    int n = 0, len = 0;
    do { t[n++] = hx[v & 15]; v >>= 4; } while (v);
    while (n) p[len++] = t[--n];
    return len;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    const unsigned char *a = src;
    char buf[INET6_ADDRSTRLEN];
    int len = 0;

    if (af == AF_INET) {
        for (int i = 0; i < 4; i++) {
            if (i) buf[len++] = '.';
            len += fmt_u(buf + len, a[i]);
        }
    } else if (af == AF_INET6) {
        static const unsigned char v4mapped[12] =
            { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
        if (!memcmp(a, v4mapped, 12)) {
            memcpy(buf, "::ffff:", 7);
            len = 7;
            for (int i = 12; i < 16; i++) {
                if (i > 12) buf[len++] = '.';
                len += fmt_u(buf + len, a[i]);
            }
        } else {
            /* RFC 5952: the longest run of two or more zero words becomes
             * "::" (the first one on a tie). */
            unsigned w[8];
            int best = -1, bestlen = 0;
            for (int i = 0; i < 8; i++) w[i] = (unsigned)(a[i * 2] << 8 | a[i * 2 + 1]);
            for (int i = 0; i < 8;) {
                if (w[i]) { i++; continue; }
                int j = i;
                while (j < 8 && !w[j]) j++;
                if (j - i > bestlen && j - i >= 2) { best = i; bestlen = j - i; }
                i = j;
            }
            for (int i = 0; i < 8; i++) {
                if (i == best) {
                    buf[len++] = ':';
                    if (i == 0) buf[len++] = ':';
                    i += bestlen - 1;
                    continue;
                }
                len += fmt_hex(buf + len, w[i]);
                if (i < 7) buf[len++] = ':';
            }
        }
    } else {
        errno = EAFNOSUPPORT;
        return 0;
    }
    buf[len] = 0;
    if ((socklen_t)len + 1 > size) {
        errno = ENOSPC;
        return 0;
    }
    memcpy(dst, buf, (size_t)len + 1);
    return dst;
}

/* ── services ──────────────────────────────────────────────────────────── */

/* /etc/services on MaeroOS is the service manager's table, so the well-known
 * ports live here. */
static const struct { const char *name; int port; int udp; int tcp; } services[] = {
    { "echo", 7, 1, 1 },     { "discard", 9, 1, 1 },  { "daytime", 13, 1, 1 },
    { "ftp-data", 20, 0, 1 }, { "ftp", 21, 0, 1 },     { "ssh", 22, 0, 1 },
    { "telnet", 23, 0, 1 },  { "smtp", 25, 0, 1 },    { "time", 37, 1, 1 },
    { "domain", 53, 1, 1 },  { "bootps", 67, 1, 0 },  { "bootpc", 68, 1, 0 },
    { "tftp", 69, 1, 0 },    { "gopher", 70, 0, 1 },  { "finger", 79, 0, 1 },
    { "http", 80, 0, 1 },    { "www", 80, 0, 1 },     { "pop3", 110, 0, 1 },
    { "sunrpc", 111, 1, 1 }, { "nntp", 119, 0, 1 },   { "ntp", 123, 1, 0 },
    { "imap", 143, 0, 1 },   { "snmp", 161, 1, 0 },   { "irc", 194, 0, 1 },
    { "ldap", 389, 0, 1 },   { "https", 443, 0, 1 },  { "syslog", 514, 1, 0 },
    { "submission", 587, 0, 1 }, { "ldaps", 636, 0, 1 }, { "rsync", 873, 0, 1 },
    { "imaps", 993, 0, 1 },  { "pop3s", 995, 0, 1 },  { "socks", 1080, 0, 1 },
    { "mdns", 5353, 1, 0 },  { "x11", 6000, 0, 1 },   { "http-alt", 8080, 0, 1 },
};
#define NSERVICES (int)(sizeof(services) / sizeof(services[0]))

static int service_ok(int i, const char *proto) {
    if (!proto) return 1;
    if (!strcmp(proto, "tcp")) return services[i].tcp;
    if (!strcmp(proto, "udp")) return services[i].udp;
    return 0;
}

static struct servent *fill_servent(int i, const char *proto) {
    static struct servent se;
    static char *noalias[1];
    static char protobuf[8];
    se.s_name = (char *)services[i].name;
    se.s_aliases = noalias;
    se.s_port = htons((uint16_t)services[i].port);
    strncpy(protobuf, proto ? proto : (services[i].tcp ? "tcp" : "udp"),
            sizeof(protobuf) - 1);
    se.s_proto = protobuf;
    return &se;
}

struct servent *getservbyname(const char *name, const char *proto) {
    for (int i = 0; name && i < NSERVICES; i++)
        if (!strcmp(services[i].name, name) && service_ok(i, proto))
            return fill_servent(i, proto);
    return 0;
}

struct servent *getservbyport(int port, const char *proto) {
    int p = ntohs((uint16_t)port);
    for (int i = 0; i < NSERVICES; i++)
        if (services[i].port == p && service_ok(i, proto))
            return fill_servent(i, proto);
    return 0;
}

/* ── /etc/resolv.conf ──────────────────────────────────────────────────── */

static void read_resconf(struct resconf *c) {
    char buf[2048];
    memset(c, 0, sizeof(*c));
    c->timeout_ms = 2000;
    c->attempts = 2;
    c->ndots = 1;

    if (read_file("/etc/resolv.conf", buf, sizeof(buf)) >= 0) {
        char *p = buf, *line;
        while ((line = next_line(&p))) {
            char *key = next_tok(&line);
            if (!key) continue;
            if (!strcmp(key, "nameserver")) {
                char *v = next_tok(&line);
                if (!v || c->nns >= MAXNS) continue;
                int port = 53;
                if (*v == '[') {
                    char *rb = strchr(v, ']');
                    if (!rb) continue;
                    *rb = 0;
                    if (rb[1] == ':') {
                        char *pe;
                        unsigned long long pv = strtoull(rb + 2, &pe, 10);
                        port = (rb[2] >= '0' && rb[2] <= '9' && !*pe && pv <= 65535)
                               ? (int)pv : -1;
                    }
                    v++;
                }
                unsigned char a4[4];
                if (!pton4(v, a4) || port <= 0 || port > 65535) continue;
                struct sockaddr_in *sa = &c->ns[c->nns++];
                sa->sin_family = AF_INET;
                sa->sin_port = htons((uint16_t)port);
                memcpy(&sa->sin_addr, a4, 4);
            } else if (!strcmp(key, "search") || !strcmp(key, "domain")) {
                int len = 0;
                char *v;
                c->search[0] = 0;
                while ((v = next_tok(&line))) {
                    int vl = (int)strlen(v);
                    if (len + vl + 2 > (int)sizeof(c->search)) break;
                    if (len) c->search[len++] = ' ';
                    memcpy(c->search + len, v, (size_t)vl);
                    len += vl;
                    c->search[len] = 0;
                    if (!strcmp(key, "domain")) break;
                }
            } else if (!strcmp(key, "options")) {
                char *v;
                while ((v = next_tok(&line))) {
                    if (!strncmp(v, "timeout:", 8)) {
                        int t = atoi(v + 8);
                        if (t >= 1 && t <= 30) c->timeout_ms = t * 1000;
                    } else if (!strncmp(v, "attempts:", 9)) {
                        int a = atoi(v + 9);
                        if (a >= 1 && a <= 5) c->attempts = a;
                    } else if (!strncmp(v, "ndots:", 6)) {
                        int d = atoi(v + 6);
                        if (d >= 0 && d <= 15) c->ndots = d;
                    }
                }
            }
        }
    }
    if (!c->nns) {
        c->ns[0].sin_family = AF_INET;
        c->ns[0].sin_port = htons(53);
        c->ns[0].sin_addr.s_addr = htonl(0x0A000203U);   /* 10.0.2.3 */
        c->nns = 1;
    }
}

/* ── DNS messages ──────────────────────────────────────────────────────── */

/* Unpredictable bits for query IDs and source ports: an off-path attacker
 * who can guess both forges answers (Kaminsky).  getrandom() is the kernel's
 * generator; the clock/pid mix is only a fallback if it ever fails. */
static uint32_t dns_random(void) {
    static uint32_t seed;
    uint32_t r;
    if (getrandom(&r, sizeof(r), 0) == (int)sizeof(r)) return r;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    seed = seed * 1103515245U + 12345U + (uint32_t)ts.tv_nsec +
           ((uint32_t)getpid() << 16);
    return seed >> 8;
}

static uint16_t next_id(void) { return (uint16_t)dns_random(); }

/* A UDP socket bound to a random port in 49152-65535 (the IANA dynamic
 * range), so the source port adds ~14 bits an attacker must guess too.  If
 * no port is free after a few tries, the kernel picks one. */
static int dns_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    for (int i = 0; i < 8; i++) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)(49152 + dns_random() % 16384));
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) break;
    }
    return fd;
}

/* Encode `name` (dotted, optional trailing dot) as DNS labels.  Returns the
 * encoded length, or -1 for an invalid or over-long name. */
static int encode_name(const char *name, unsigned char *out, int cap) {
    int len = 0;
    if (!strcmp(name, ".")) name = "";
    while (*name) {
        const char *dot = strchr(name, '.');
        int l = dot ? (int)(dot - name) : (int)strlen(name);
        if (l < 1 || l > 63 || len + l + 2 > cap || len + l + 2 > 255) return -1;
        out[len++] = (unsigned char)l;
        memcpy(out + len, name, (size_t)l);
        len += l;
        name += l;
        if (*name == '.') name++;
    }
    if (len + 1 > cap) return -1;
    out[len++] = 0;
    return len;
}

static int build_query(const char *name, int class, int type, uint16_t id,
                       unsigned char *q, int cap) {
    if (cap < 12 + 4 + 1) return -1;
    memset(q, 0, 12);
    q[0] = (unsigned char)(id >> 8);
    q[1] = (unsigned char)id;
    q[2] = 0x01;                        /* RD */
    q[5] = 1;                           /* QDCOUNT */
    int n = encode_name(name, q + 12, cap - 12 - 4);
    if (n < 0) return -1;
    n += 12;
    q[n++] = (unsigned char)(type >> 8);
    q[n++] = (unsigned char)type;
    q[n++] = (unsigned char)(class >> 8);
    q[n++] = (unsigned char)class;
    return n;
}

int dn_expand(const unsigned char *msg, const unsigned char *eom,
              const unsigned char *src, char *dst, int dstlen) {
    const unsigned char *p = src;
    int consumed = -1, out = 0, hops = 0;

    if (!msg || !eom || !src || src < msg || src >= eom || dstlen < 1) return -1;
    for (;;) {
        if (p >= eom) return -1;
        unsigned c = *p;
        if ((c & 0xC0) == 0xC0) {
            if (p + 1 >= eom) return -1;
            if (consumed < 0) consumed = (int)(p + 2 - src);
            unsigned off = ((c & 0x3F) << 8) | p[1];
            if (msg + off >= eom || ++hops > 64) return -1;
            p = msg + off;
            continue;
        }
        if (c & 0xC0) return -1;          /* 0x40/0x80: reserved label types */
        p++;
        if (!c) break;
        if (p + c > eom) return -1;
        if (out && out + 1 >= dstlen) return -1;
        if (out) dst[out++] = '.';
        if (out + (int)c >= dstlen) return -1;
        memcpy(dst + out, p, c);
        out += (int)c;
        p += c;
    }
    if (!out) {
        if (dstlen < 2) return -1;
        dst[out++] = '.';
    }
    dst[out] = 0;
    return consumed >= 0 ? consumed : (int)(p - src);
}

int dn_comp(const char *src, unsigned char *dst, int dstlen,
            unsigned char **dnptrs, unsigned char **lastdnptr) {
    (void)dnptrs; (void)lastdnptr;      /* no compression: always valid */
    return encode_name(src, dst, dstlen);
}

/* Skip one (possibly compressed) name; returns the new offset or -1. */
static int skip_name(const unsigned char *m, int len, int off) {
    while (off < len) {
        unsigned c = m[off];
        if ((c & 0xC0) == 0xC0) return off + 2 <= len ? off + 2 : -1;
        if (c & 0xC0) return -1;
        off += 1 + (int)c;
        if (!c) return off;
    }
    return -1;
}

static int rd16(const unsigned char *p) { return p[0] << 8 | p[1]; }

/* Collect the records of type `qtype` (A, AAAA or PTR) from an answer.
 * Addresses go to `out` (up to `max`); a PTR target or the last CNAME
 * target goes to `name`.  Returns the RCODE, or -1 for a malformed reply. */
static int parse_reply(const unsigned char *m, int len, int qtype,
                       struct addr *out, int *nout, int max,
                       char *name, int namecap) {
    if (len < 12) return -1;
    int rcode = m[3] & 0x0F;
    int qd = rd16(m + 4), an = rd16(m + 6);
    int off = 12;
    for (int i = 0; i < qd; i++) {
        off = skip_name(m, len, off);
        if (off < 0 || off + 4 > len) return -1;
        off += 4;
    }
    for (int i = 0; i < an; i++) {
        off = skip_name(m, len, off);
        if (off < 0 || off + 10 > len) break;       /* truncated: keep what we have */
        int type = rd16(m + off), class = rd16(m + off + 2);
        int rdlen = rd16(m + off + 8);
        off += 10;
        if (off + rdlen > len) break;
        if (class == C_IN) {
            if (type == T_A && qtype == T_A && rdlen == 4 && *nout < max) {
                out[*nout].family = AF_INET;
                memcpy(out[*nout].a, m + off, 4);
                (*nout)++;
            } else if (type == T_AAAA && qtype == T_AAAA && rdlen == 16 &&
                       *nout < max) {
                out[*nout].family = AF_INET6;
                memcpy(out[*nout].a, m + off, 16);
                (*nout)++;
            } else if ((type == T_CNAME || (type == T_PTR && qtype == T_PTR)) &&
                       name) {
                char tmp[NS_MAXDNAME];
                if (dn_expand(m, m + len, m + off, tmp, sizeof(tmp)) > 0 &&
                    (int)strlen(tmp) < namecap) {
                    strcpy(name, tmp);
                    if (type == T_PTR) (*nout)++;
                }
            }
        }
        off += rdlen;
    }
    return rcode;
}

/*
 * Send each query to every nameserver and collect one reply per query.
 * A reply counts if it comes from a configured server, carries a query's ID
 * and echoes its question; NOERROR and NXDOMAIN are final, SERVFAIL/REFUSED
 * are kept only until a better reply arrives.  Returns 0 when every query
 * has a reply, -1 on timeout or socket failure (errno set).
 */
static int exchange(const struct resconf *c, unsigned char *const *qs,
                    const int *qlens, int nq, unsigned char *const *ans,
                    int *alens, int anscap) {
    int fd = dns_socket();
    if (fd < 0) return -1;
    int final[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < nq; i++) alens[i] = 0;

    int done = 0;
    for (int attempt = 0; attempt < c->attempts && !done; attempt++) {
        for (int i = 0; i < nq; i++) {
            if (final[i]) continue;
            for (int s = 0; s < c->nns; s++)
                sendto(fd, qs[i], (size_t)qlens[i], 0,
                       (const struct sockaddr *)&c->ns[s], sizeof(c->ns[s]));
        }
        long deadline = now_ms() + c->timeout_ms;
        for (;;) {
            done = 1;
            for (int i = 0; i < nq; i++) if (!final[i]) done = 0;
            if (done) break;
            long left = deadline - now_ms();
            if (left <= 0) break;
            struct pollfd pfd = { fd, POLLIN, 0 };
            if (poll(&pfd, 1, (int)left) <= 0) continue;

            unsigned char r[DNS_BUFSZ];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = recvfrom(fd, r, sizeof(r), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fl);
            if (n < 12 || !(r[2] & 0x80)) continue;   /* not a response */
            int known = 0;
            for (int s = 0; s < c->nns; s++)
                if (from.sin_addr.s_addr == c->ns[s].sin_addr.s_addr &&
                    from.sin_port == c->ns[s].sin_port)
                    known = 1;
            if (!known) continue;
            for (int i = 0; i < nq; i++) {
                if (final[i] || r[0] != qs[i][0] || r[1] != qs[i][1]) continue;
                /* The question must be ours (same name/type/class). */
                if (n < qlens[i] || memcmp(r + 12, qs[i] + 12, (size_t)(qlens[i] - 12)))
                    continue;
                int rc = r[3] & 0x0F;
                if (rc == 0 || rc == 3 || !alens[i]) {
                    int k = n < anscap ? n : anscap;
                    memcpy(ans[i], r, (size_t)k);
                    alens[i] = k;
                }
                if (rc == 0 || rc == 3) final[i] = 1;
                break;
            }
        }
    }
    close(fd);
    for (int i = 0; i < nq; i++)
        if (!alens[i]) { errno = ETIMEDOUT; return -1; }
    return 0;
}

/* ── res_* ─────────────────────────────────────────────────────────────── */

int res_init(void) { return 0; }

int res_mkquery(int op, const char *dname, int class, int type,
                const unsigned char *data, int datalen,
                const unsigned char *newrr, unsigned char *buf, int buflen) {
    (void)data; (void)datalen; (void)newrr;
    if (op != QUERY || !dname) return -1;
    return build_query(dname, class, type, next_id(), buf, buflen);
}

int res_send(const unsigned char *msg, int msglen, unsigned char *answer,
             int anslen) {
    struct resconf c;
    unsigned char *q = (unsigned char *)msg;
    int alen;
    if (msglen < 12) return -1;
    read_resconf(&c);
    if (exchange(&c, &q, &msglen, 1, &answer, &alen, anslen) < 0) return -1;
    return alen;
}

int res_query(const char *dname, int class, int type, unsigned char *answer,
              int anslen) {
    unsigned char q[300];
    int qlen = build_query(dname, class, type, next_id(), q, sizeof(q));
    if (qlen < 0) { h_errno = NO_RECOVERY; return -1; }
    int n = res_send(q, qlen, answer, anslen);
    if (n < 12) { h_errno = TRY_AGAIN; return -1; }
    int rc = answer[3] & 0x0F;
    if (rc == 3) { h_errno = HOST_NOT_FOUND; return -1; }
    if (rc) { h_errno = TRY_AGAIN; return -1; }
    if (!rd16(answer + 6)) { h_errno = NO_DATA; return -1; }
    return n;
}

int res_search(const char *dname, int class, int type, unsigned char *answer,
               int anslen) {
    return res_query(dname, class, type, answer, anslen);
}

/* ── name → addresses ──────────────────────────────────────────────────── */

static int want_family(int family, int af) {
    return family == AF_UNSPEC || family == af;
}

/* "localhost" and "*.localhost" are always the loopback (RFC 6761). */
static int lookup_localhost(const char *name, int family, struct addr *out) {
    int len = (int)strlen(name);
    if (len && name[len - 1] == '.') len--;
    if (!(len == 9 && !strncasecmp(name, "localhost", 9)) &&
        !(len > 10 && !strncasecmp(name + len - 10, ".localhost", 10)))
        return 0;
    int n = 0;
    if (want_family(family, AF_INET)) {
        out[n].family = AF_INET;
        memcpy(out[n].a, "\x7f\0\0\1", 4);
        n++;
    }
    if (want_family(family, AF_INET6)) {
        out[n].family = AF_INET6;
        memset(out[n].a, 0, 16);
        out[n].a[15] = 1;
        n++;
    }
    return n;
}

static int parse_addr(const char *s, struct addr *a) {
    if (pton4(s, a->a)) { a->family = AF_INET; return 1; }
    if (pton6(s, a->a)) { a->family = AF_INET6; return 1; }
    return 0;
}

/* /etc/hosts by name: every address of the wanted family on lines naming
 * `name`; `canon` gets the line's first (canonical) name. */
static int lookup_hosts(const char *name, int family, struct addr *out,
                        int max, char *canon, int canoncap) {
    static char buf[8192];
    int n = 0;
    char want[256];
    int wl = (int)strlen(name);
    if (wl >= (int)sizeof(want)) return 0;
    memcpy(want, name, (size_t)wl + 1);
    if (wl > 1 && want[wl - 1] == '.') want[wl - 1] = 0;

    if (read_file("/etc/hosts", buf, sizeof(buf)) < 0) return 0;
    char *p = buf, *line;
    while ((line = next_line(&p)) && n < max) {
        char *as = next_tok(&line), *nm, *first = 0;
        struct addr a;
        if (!as || !parse_addr(as, &a) || !want_family(family, a.family))
            continue;
        int match = 0;
        while ((nm = next_tok(&line))) {
            if (!first) first = nm;
            if (ci_eq(nm, want)) match = 1;
        }
        if (!match) continue;
        out[n++] = a;
        if (canon && first && (int)strlen(first) < canoncap && n == 1)
            strcpy(canon, first);
    }
    return n;
}

/* /etc/hosts by address: the first name on the first matching line. */
static int reverse_hosts(const struct addr *a, char *name, int cap) {
    static char buf[8192];
    if (read_file("/etc/hosts", buf, sizeof(buf)) < 0) return 0;
    char *p = buf, *line;
    while ((line = next_line(&p))) {
        char *as = next_tok(&line), *nm;
        struct addr b;
        if (!as || !parse_addr(as, &b) || b.family != a->family) continue;
        if (memcmp(a->a, b.a, a->family == AF_INET ? 4 : 16)) continue;
        if ((nm = next_tok(&line)) && (int)strlen(nm) < cap) {
            strcpy(name, nm);
            return 1;
        }
    }
    return 0;
}

/*
 * One DNS name: A and/or AAAA queried together.  Returns the number of
 * addresses (IPv4 first), 0 for "no such name / no data", or an EAI_ error
 * when no server gave a usable answer.
 */
static int dns_name(const struct resconf *c, const char *name, int family,
                    struct addr *out, int max, char *canon, int canoncap) {
    unsigned char qa[300], q6[300], ra[DNS_BUFSZ], r6[DNS_BUFSZ];
    unsigned char *qs[2], *as[2];
    int qlens[2], alens[2], types[2], nq = 0;
    if (want_family(family, AF_INET)) {
        qlens[nq] = build_query(name, C_IN, T_A, next_id(), qa, sizeof(qa));
        qs[nq] = qa; as[nq] = ra; types[nq] = T_A;
        if (qlens[nq] < 0) return EAI_NONAME;
        nq++;
    }
    if (want_family(family, AF_INET6)) {
        qlens[nq] = build_query(name, C_IN, T_AAAA, next_id(), q6, sizeof(q6));
        qs[nq] = q6; as[nq] = r6; types[nq] = T_AAAA;
        if (qlens[nq] < 0) return EAI_NONAME;
        nq++;
    }
    if (exchange(c, qs, qlens, nq, as, alens, DNS_BUFSZ) < 0) {
        /* AF_UNSPEC: an IPv4 answer is enough even if AAAA got lost. */
        if (!(nq == 2 && alens[0]))
            return EAI_AGAIN;
        nq = 1;
    }

    int n = 0, nxdomain = 0, failed = 0;
    for (int i = 0; i < nq; i++) {
        int rc = parse_reply(as[i], alens[i], types[i], out, &n, max,
                             canon, canoncap);
        if (rc == 3) nxdomain = 1;
        else if (rc != 0) failed = 1;
    }
    if (n) return n;
    if (failed && !nxdomain) return EAI_AGAIN;
    return 0;
}

/* Name → addresses: localhost, /etc/hosts, then DNS with the search list.
 * `canon` gets the canonical name (CNAME target or hosts' first name). */
static int lookup_name(const char *name, int family, struct addr *out, int max,
                       char *canon, int canoncap) {
    int n;
    if (canon) {
        strncpy(canon, name, (size_t)canoncap - 1);
        canon[canoncap - 1] = 0;
    }
    if ((n = lookup_localhost(name, family, out))) return n;
    if ((n = lookup_hosts(name, family, out, max, canon, canoncap))) return n;

    struct resconf c;
    read_resconf(&c);

    int len = (int)strlen(name), dots = 0;
    for (int i = 0; i < len; i++) if (name[i] == '.') dots++;
    int absolute = len && name[len - 1] == '.';
    int search_first = !absolute && dots < c.ndots && c.search[0];

    /* Candidates: the name as given, and name.<domain> per search domain. */
    char cand[MAXSEARCH + 1][256];
    int ncand = 0;
    if (!search_first && len < 256) strcpy(cand[ncand++], name);
    if (!absolute) {
        char *p = c.search, *d;
        char sbuf[256];
        strcpy(sbuf, c.search);
        p = sbuf;
        while ((d = next_tok(&p)) && ncand < MAXSEARCH) {
            if (len + 1 + (int)strlen(d) >= 256) continue;
            strcpy(cand[ncand], name);
            strcat(cand[ncand], ".");
            strcat(cand[ncand], d);
            ncand++;
        }
    }
    if (search_first && len < 256) strcpy(cand[ncand++], name);

    int err = EAI_NONAME;
    for (int i = 0; i < ncand; i++) {
        n = dns_name(&c, cand[i], family, out, max, canon, canoncap);
        if (n > 0) {
            if (canon && !strcmp(canon, name) && (int)strlen(cand[i]) < canoncap)
                strcpy(canon, cand[i]);
            return n;
        }
        if (n < 0) err = n;
    }
    return err;
}

/* ── destination order (RFC 6724 section 6, rules 1 and 6) ─────────────── */

/* The policy table's precedence (RFC 6724 section 2.1). */
static int precedence(const struct addr *a) {
    const unsigned char *b = a->a;
    if (a->family == AF_INET) return 35;                 /* ::ffff:0:0/96 */
    static const unsigned char lo[16] = { [15] = 1 };
    if (!memcmp(b, lo, 16)) return 50;                   /* ::1/128 */
    if (IN6_IS_ADDR_V4MAPPED(b)) return 35;
    if (b[0] == 0x20 && b[1] == 0x02) return 30;         /* 2002::/16 6to4 */
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0 && b[3] == 0) return 5;  /* Teredo */
    if ((b[0] & 0xfe) == 0xfc) return 3;                 /* fc00::/7 ULA */
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0) return 1; /* fec0::/10 */
    if (b[0] == 0x3f && b[1] == 0xfe) return 1;          /* 3ffe::/16 6bone */
    int zero12 = 1;
    for (int i = 0; i < 12; i++) if (b[i]) zero12 = 0;
    if (zero12) return 1;                                /* ::/96 v4-compatible */
    return 40;                                           /* ::/0 */
}

/* Rule 1: is there a route?  A connected UDP socket picks a source
 * address without sending anything; ENETUNREACH means none. */
static int reachable(const struct addr *a) {
    int fd = socket(a->family, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (fd < 0) return 0;
    int ok;
    if (a->family == AF_INET) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(65535);
        memcpy(&sin.sin_addr, a->a, 4);
        ok = connect(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0;
    } else {
        struct sockaddr_in6 sin6;
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(65535);
        memcpy(&sin6.sin6_addr, a->a, 16);
        ok = connect(fd, (struct sockaddr *)&sin6, sizeof(sin6)) == 0;
    }
    close(fd);
    return ok;
}

/* A stable sort on (reachable, precedence), both descending. */
static int sort_dests(const struct addr *in, int n, struct addr *out) {
    int key[MAXADDRS];
    if (n <= 1) {
        if (n == 1) out[0] = in[0];
        return n;
    }
    for (int i = 0; i < n; i++) {
        out[i] = in[i];
        key[i] = (reachable(&in[i]) ? 100 : 0) + precedence(&in[i]);
    }
    for (int i = 1; i < n; i++) {
        struct addr a = out[i];
        int k = key[i], j = i - 1;
        while (j >= 0 && key[j] < k) {
            out[j + 1] = out[j];
            key[j + 1] = key[j];
            j--;
        }
        out[j + 1] = a;
        key[j + 1] = k;
    }
    return n;
}

/* ── getaddrinfo ───────────────────────────────────────────────────────── */

static int parse_service(const char *service, int socktype, int flags,
                         int *port) {
    *port = 0;
    if (!service) return 0;
    char *end;
    /* Plain decimal digits only (no sign or blanks), range-checked before
     * any wrap: "4294967376" is not port 80. */
    unsigned long long v = strtoull(service, &end, 10);
    if (*service >= '0' && *service <= '9' && !*end) {
        if (v > 65535) return EAI_SERVICE;
        *port = (int)v;
        return 0;
    }
    if (flags & AI_NUMERICSERV) return EAI_NONAME;
    const char *proto = socktype == SOCK_DGRAM ? "udp" :
                        socktype == SOCK_STREAM ? "tcp" : 0;
    struct servent *se = getservbyname(service, proto);
    if (!se) return EAI_SERVICE;
    *port = ntohs((uint16_t)se->s_port);
    return 0;
}

static struct addrinfo *new_ai(const struct addr *a, int port, int socktype,
                               int protocol, const char *canon) {
    size_t clen = canon ? strlen(canon) + 1 : 0;
    struct addrinfo *ai = malloc(sizeof(struct addrinfo) +
                                 sizeof(struct sockaddr_in6) + clen);
    if (!ai) return 0;
    memset(ai, 0, sizeof(*ai) + sizeof(struct sockaddr_in6));
    ai->ai_family = a->family;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addr = (struct sockaddr *)(ai + 1);
    if (a->family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)ai->ai_addr;
        sin->sin_family = AF_INET;
        sin->sin_port = htons((uint16_t)port);
        memcpy(&sin->sin_addr, a->a, 4);
        ai->ai_addrlen = sizeof(struct sockaddr_in);
    } else {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ai->ai_addr;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons((uint16_t)port);
        memcpy(&sin6->sin6_addr, a->a, 16);
        ai->ai_addrlen = sizeof(struct sockaddr_in6);
    }
    if (canon) {
        ai->ai_canonname = (char *)ai->ai_addr + sizeof(struct sockaddr_in6);
        memcpy(ai->ai_canonname, canon, clen);
    }
    return ai;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res);
        res = next;
    }
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    int flags = hints ? hints->ai_flags : 0;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;

    if (!res) return EAI_FAIL;
    *res = 0;
    if (flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_V4MAPPED |
                  AI_ALL | AI_ADDRCONFIG | AI_NUMERICSERV))
        return EAI_BADFLAGS;
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6)
        return EAI_FAMILY;
    if (socktype && socktype != SOCK_STREAM && socktype != SOCK_DGRAM &&
        socktype != SOCK_RAW)
        return EAI_SOCKTYPE;
    if (!node && !service) return EAI_NONAME;
    if ((flags & AI_CANONNAME) && !node) return EAI_BADFLAGS;
    if (socktype == SOCK_RAW && service) return EAI_SERVICE;

    int port, err = parse_service(service, socktype, flags, &port);
    if (err) return err;

    struct addr addrs[MAXADDRS];
    char canon[256];
    int n = 0;
    canon[0] = 0;

    if (!node) {
        /* Wildcard for bind() or loopback for connect(); IPv4 first. */
        int passive = flags & AI_PASSIVE;
        if (want_family(family, AF_INET)) {
            addrs[n].family = AF_INET;
            memcpy(addrs[n].a, passive ? "\0\0\0\0" : "\x7f\0\0\1", 4);
            n++;
        }
        if (want_family(family, AF_INET6)) {
            addrs[n].family = AF_INET6;
            memset(addrs[n].a, 0, 16);
            if (!passive) addrs[n].a[15] = 1;
            n++;
        }
    } else {
        struct addr a;
        struct in_addr in;
        if (pton6(node, a.a)) {
            a.family = AF_INET6;
            if (family == AF_INET) return EAI_ADDRFAMILY;
            addrs[n++] = a;
        } else if (inet_aton(node, &in)) {
            a.family = AF_INET;
            memcpy(a.a, &in, 4);
            if (family == AF_INET6) {
                if (!(flags & AI_V4MAPPED)) return EAI_ADDRFAMILY;
                memset(a.a, 0, 16);
                a.a[10] = a.a[11] = 0xff;
                memcpy(a.a + 12, &in, 4);
                a.family = AF_INET6;
            }
            addrs[n++] = a;
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        } else {
            n = lookup_name(node, family, addrs, MAXADDRS, canon, sizeof(canon));
            if (n == 0 && family == AF_INET6 && (flags & AI_V4MAPPED)) {
                n = lookup_name(node, AF_INET, addrs, MAXADDRS, canon,
                                sizeof(canon));
                for (int i = 0; i < n; i++) {
                    unsigned char v4[4];
                    memcpy(v4, addrs[i].a, 4);
                    memset(addrs[i].a, 0, 16);
                    addrs[i].a[10] = addrs[i].a[11] = 0xff;
                    memcpy(addrs[i].a + 12, v4, 4);
                    addrs[i].family = AF_INET6;
                }
            }
            if (n < 0) return n;
            if (n == 0) return EAI_NONAME;
        }
        if (!canon[0]) strncpy(canon, node, sizeof(canon) - 1);
    }

    struct addr sorted[MAXADDRS];
    int ns = 0;
    if (!node) {
        for (int i = 0; i < n; i++) sorted[ns++] = addrs[i];
    } else {
        ns = sort_dests(addrs, n, sorted);
    }

    static const int types[] = { SOCK_STREAM, SOCK_DGRAM };
    struct addrinfo *head = 0, **tail = &head;
    for (int i = 0; i < ns; i++) {
        for (int t = 0; t < 2; t++) {
            int st = socktype ? socktype : types[t];
            int pr = protocol ? protocol :
                     st == SOCK_STREAM ? IPPROTO_TCP :
                     st == SOCK_DGRAM ? IPPROTO_UDP : 0;
            struct addrinfo *ai = new_ai(&sorted[i], port, st, pr,
                                         (!head && (flags & AI_CANONNAME)) ? canon : 0);
            if (!ai) {
                freeaddrinfo(head);
                return EAI_MEMORY;
            }
            ai->ai_flags = flags;
            *tail = ai;
            tail = &ai->ai_next;
            if (socktype) break;
        }
    }
    *res = head;
    return 0;
}

const char *gai_strerror(int errcode) {
    switch (errcode) {
    case 0:            return "Success";
    case EAI_BADFLAGS: return "Invalid flags";
    case EAI_NONAME:   return "Name does not resolve";
    case EAI_AGAIN:    return "Temporary failure in name resolution";
    case EAI_FAIL:     return "Non-recoverable failure in name resolution";
    case EAI_NODATA:   return "No address associated with name";
    case EAI_FAMILY:   return "Unrecognized address family";
    case EAI_SOCKTYPE: return "Unrecognized socket type";
    case EAI_SERVICE:  return "Unrecognized service";
    case EAI_MEMORY:   return "Out of memory";
    case EAI_SYSTEM:   return "System error";
    case EAI_OVERFLOW: return "Overflow";
    }
    return "Unknown error";
}

/* ── addresses → names ─────────────────────────────────────────────────── */

/* PTR lookup for `a`: in-addr.arpa / ip6.arpa. */
static int dns_reverse(const struct addr *a, char *name, int cap) {
    char q[80];
    int len = 0;
    if (a->family == AF_INET) {
        for (int i = 3; i >= 0; i--) {
            len += fmt_u(q + len, a->a[i]);
            q[len++] = '.';
        }
        memcpy(q + len, "in-addr.arpa", 13);
    } else {
        static const char hx[] = "0123456789abcdef";
        for (int i = 15; i >= 0; i--) {
            q[len++] = hx[a->a[i] & 15]; q[len++] = '.';
            q[len++] = hx[a->a[i] >> 4]; q[len++] = '.';
        }
        memcpy(q + len, "ip6.arpa", 9);
    }

    struct resconf c;
    unsigned char qb[300], rb[DNS_BUFSZ];
    unsigned char *qs = qb, *as = rb;
    int qlen, alen, n = 0;
    read_resconf(&c);
    qlen = build_query(q, C_IN, T_PTR, next_id(), qb, sizeof(qb));
    if (qlen < 0) return EAI_FAIL;
    if (exchange(&c, &qs, &qlen, 1, &as, &alen, sizeof(rb)) < 0) return EAI_AGAIN;
    int rc = parse_reply(rb, alen, T_PTR, 0, &n, 0, name, cap);
    if (rc == 0 && n) return 1;
    if (rc == 0 || rc == 3) return 0;
    return EAI_AGAIN;
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    struct addr a;
    int port;

    if (!sa) return EAI_FAIL;
    if (sa->sa_family == AF_INET) {
        if (salen < sizeof(struct sockaddr_in)) return EAI_FAMILY;
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        a.family = AF_INET;
        memcpy(a.a, &sin->sin_addr, 4);
        port = ntohs(sin->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        if (salen < sizeof(struct sockaddr_in6)) return EAI_FAMILY;
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        a.family = AF_INET6;
        memcpy(a.a, &sin6->sin6_addr, 16);
        port = ntohs(sin6->sin6_port);
    } else {
        return EAI_FAMILY;
    }

    if (host && hostlen) {
        char name[NS_MAXDNAME];
        int found = 0;
        if (!(flags & NI_NUMERICHOST)) {
            found = reverse_hosts(&a, name, sizeof(name));
            if (!found) {
                int r = dns_reverse(&a, name, sizeof(name));
                if (r > 0) found = 1;
                else if (r < 0 && (flags & NI_NAMEREQD)) return r;
            }
            if (found && (flags & NI_NOFQDN)) {
                char *dot = strchr(name, '.');
                if (dot) *dot = 0;
            }
        }
        if (!found) {
            if (flags & NI_NAMEREQD) return EAI_NONAME;
            if (!inet_ntop(a.family, a.a, name, sizeof(name))) return EAI_FAIL;
        }
        if (strlen(name) + 1 > hostlen) return EAI_OVERFLOW;
        strcpy(host, name);
    }

    if (serv && servlen) {
        char buf[NI_MAXSERV];
        struct servent *se = 0;
        if (!(flags & NI_NUMERICSERV))
            se = getservbyport(htons((uint16_t)port),
                               (flags & NI_DGRAM) ? "udp" : "tcp");
        if (se) {
            strncpy(buf, se->s_name, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
        } else {
            buf[fmt_u(buf, (unsigned)port)] = 0;
        }
        if (strlen(buf) + 1 > servlen) return EAI_OVERFLOW;
        strcpy(serv, buf);
    }
    return 0;
}

/* ── gethostby* ────────────────────────────────────────────────────────── */

/* Lay a hostent out in `buf`: aliases (none), the address list, the
 * addresses, then the name.  Returns 0, or -1 (ERANGE) if it does not fit. */
static int fill_hostent(struct hostent *h, char *buf, size_t buflen,
                        const char *name, int af, const struct addr *addrs,
                        int n) {
    int alen = af == AF_INET ? 4 : 16;
    size_t need = sizeof(char *) * (size_t)(n + 2) + (size_t)(alen * n) +
                  strlen(name) + 1 + sizeof(char *);
    if (need > buflen) return -1;
    char **aliases = (char **)buf;
    char **list = aliases + 1;
    char *data = (char *)(list + n + 1);
    aliases[0] = 0;
    for (int i = 0; i < n; i++) {
        list[i] = data;
        memcpy(data, addrs[i].a, (size_t)alen);
        data += alen;
    }
    list[n] = 0;
    strcpy(data, name);
    h->h_name = data;
    h->h_aliases = aliases;
    h->h_addrtype = af;
    h->h_length = alen;
    h->h_addr_list = list;
    return 0;
}

static int hostent_lookup(const char *name, int af, struct hostent *h,
                          char *buf, size_t buflen, int *herr) {
    struct addr addrs[MAXADDRS];
    char canon[256];
    int n;

    if (!name || (af != AF_INET && af != AF_INET6)) {
        *herr = NO_RECOVERY;
        return -1;
    }
    struct addr a;
    if (af == AF_INET && pton4(name, a.a)) {
        a.family = AF_INET;
        addrs[0] = a;
        n = 1;
        strncpy(canon, name, sizeof(canon) - 1);
        canon[sizeof(canon) - 1] = 0;
    } else if (af == AF_INET6 && pton6(name, a.a)) {
        a.family = AF_INET6;
        addrs[0] = a;
        n = 1;
        strncpy(canon, name, sizeof(canon) - 1);
        canon[sizeof(canon) - 1] = 0;
    } else {
        n = lookup_name(name, af, addrs, MAXADDRS, canon, sizeof(canon));
    }
    if (n <= 0) {
        *herr = n == EAI_AGAIN ? TRY_AGAIN : HOST_NOT_FOUND;
        return -1;
    }
    if (fill_hostent(h, buf, buflen, canon, af, addrs, n) < 0) {
        *herr = NO_RECOVERY;
        errno = ERANGE;
        return -1;
    }
    return 0;
}

static struct hostent static_he;
static char static_hebuf[1024];

struct hostent *gethostbyname2(const char *name, int af) {
    if (hostent_lookup(name, af, &static_he, static_hebuf,
                       sizeof(static_hebuf), &h_errno) < 0)
        return 0;
    return &static_he;
}

struct hostent *gethostbyname(const char *name) {
    return gethostbyname2(name, AF_INET);
}

int gethostbyname_r(const char *name, struct hostent *ret, char *buf,
                    size_t buflen, struct hostent **result, int *h_errnop) {
    int herr = 0;
    *result = 0;
    if (hostent_lookup(name, AF_INET, ret, buf, buflen, &herr) < 0) {
        if (h_errnop) *h_errnop = herr;
        return errno == ERANGE ? ERANGE : ENOENT;
    }
    *result = ret;
    return 0;
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type) {
    struct addr a;
    char name[NS_MAXDNAME];
    if (!addr || (type == AF_INET && len != 4) ||
        (type == AF_INET6 && len != 16) ||
        (type != AF_INET && type != AF_INET6)) {
        h_errno = NO_RECOVERY;
        return 0;
    }
    a.family = type;
    memcpy(a.a, addr, len);
    int r = reverse_hosts(&a, name, sizeof(name));
    if (!r) r = dns_reverse(&a, name, sizeof(name));
    if (r <= 0) {
        h_errno = r < 0 ? TRY_AGAIN : HOST_NOT_FOUND;
        return 0;
    }
    if (fill_hostent(&static_he, static_hebuf, sizeof(static_hebuf), name,
                     type, &a, 1) < 0) {
        h_errno = NO_RECOVERY;
        return 0;
    }
    return &static_he;
}

void herror(const char *s) {
    const char *m = hstrerror(h_errno);
    if (s && *s) {
        write(2, s, strlen(s));
        write(2, ": ", 2);
    }
    write(2, m, strlen(m));
    write(2, "\n", 1);
}

/* The original MaeroOS entry point (browse, httpget, pkg): one IPv4
 * address in network byte order, 0 if the name does not resolve. */
unsigned int resolve_a(const char *host) {
    struct addr addrs[MAXADDRS];
    struct in_addr in;
    if (!host || !*host) return 0;
    if (inet_aton(host, &in)) return in.s_addr;
    int n = lookup_name(host, AF_INET, addrs, MAXADDRS, 0, 0);
    if (n <= 0) return 0;
    unsigned int ip;
    memcpy(&ip, addrs[0].a, 4);
    return ip;
}
