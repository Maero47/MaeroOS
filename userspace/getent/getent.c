/*
 * getent - query the name databases through the libc resolver.
 *
 *   getent hosts NAME|ADDR...    first address (getaddrinfo) or name
 *                                (getnameinfo) per key, glibc's format
 *   getent ahosts NAME...        every address getaddrinfo returns
 *   getent ahostsv4 NAME...      the same, AF_INET only
 *   getent ahostsv6 NAME...      the same, AF_INET6 only
 *   getent services NAME|PORT... the built-in service table
 *
 * Exit status as glibc's: 0 found, 1 bad arguments, 2 a key not found.
 */
#include "../include/arpa/inet.h"
#include "../include/netdb.h"
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"

static const char *ntop(const struct sockaddr *sa, char *buf, int cap) {
    if (sa->sa_family == AF_INET)
        return inet_ntop(AF_INET, &((const struct sockaddr_in *)sa)->sin_addr,
                         buf, (socklen_t)cap);
    return inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)sa)->sin6_addr,
                     buf, (socklen_t)cap);
}

static int is_numeric(const char *s, struct sockaddr_in6 *ss, socklen_t *len) {
    struct sockaddr_in *sin = (struct sockaddr_in *)ss;
    memset(ss, 0, sizeof(*ss));
    if (inet_pton(AF_INET, s, &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        *len = sizeof(*sin);
        return 1;
    }
    if (inet_pton(AF_INET6, s, &ss->sin6_addr) == 1) {
        ss->sin6_family = AF_INET6;
        *len = sizeof(*ss);
        return 1;
    }
    return 0;
}

static int hosts(const char *key) {
    struct sockaddr_in6 ss;
    socklen_t sl;
    char buf[INET6_ADDRSTRLEN], name[NI_MAXHOST];

    if (is_numeric(key, &ss, &sl)) {
        int rc = getnameinfo((struct sockaddr *)&ss, sl, name, sizeof(name),
                             0, 0, NI_NAMEREQD);
        if (rc) return 2;
        printf("%-15s %s\n", key, name);
        return 0;
    }
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    int rc = getaddrinfo(key, 0, &hints, &res);
    if (rc) {
        fprintf(stderr, "getent: %s: %s\n", key, gai_strerror(rc));
        return 2;
    }
    ntop(res->ai_addr, buf, sizeof(buf));
    if (res->ai_canonname && strcmp(res->ai_canonname, key))
        printf("%-15s %s %s\n", buf, res->ai_canonname, key);
    else
        printf("%-15s %s\n", buf, key);
    freeaddrinfo(res);
    return 0;
}

static int ahosts(const char *key, int family) {
    struct addrinfo hints, *res, *ai;
    char buf[INET6_ADDRSTRLEN];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_flags = AI_CANONNAME | (family == AF_INET6 ? AI_V4MAPPED : 0);
    int rc = getaddrinfo(key, 0, &hints, &res);
    if (rc) {
        fprintf(stderr, "getent: %s: %s\n", key, gai_strerror(rc));
        return 2;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        const char *type = ai->ai_socktype == SOCK_STREAM ? "STREAM" :
                           ai->ai_socktype == SOCK_DGRAM ? "DGRAM" : "RAW";
        printf("%-15s %-6s %s\n", ntop(ai->ai_addr, buf, sizeof(buf)), type,
               ai == res && ai->ai_canonname ? ai->ai_canonname : "");
    }
    freeaddrinfo(res);
    return 0;
}

static int services(const char *key) {
    char *end;
    long port = strtol(key, &end, 10);
    struct servent *se = (*key && !*end) ? getservbyport(htons((uint16_t)port), 0)
                                         : getservbyname(key, 0);
    if (!se) return 2;
    printf("%-15s %d/%s\n", se->s_name, ntohs((uint16_t)se->s_port), se->s_proto);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: getent hosts|ahosts|ahostsv4|ahostsv6|services KEY...\n");
        return 1;
    }
    int status = 0;
    for (int i = 2; i < argc; i++) {
        int rc;
        if (!strcmp(argv[1], "hosts")) rc = hosts(argv[i]);
        else if (!strcmp(argv[1], "ahosts")) rc = ahosts(argv[i], AF_UNSPEC);
        else if (!strcmp(argv[1], "ahostsv4")) rc = ahosts(argv[i], AF_INET);
        else if (!strcmp(argv[1], "ahostsv6")) rc = ahosts(argv[i], AF_INET6);
        else if (!strcmp(argv[1], "services")) rc = services(argv[i]);
        else {
            fprintf(stderr, "getent: unknown database: %s\n", argv[1]);
            return 1;
        }
        if (rc) status = rc;
    }
    return status;
}
