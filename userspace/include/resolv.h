#pragma once

/* The res_* subset toybox host(1) and simple ports use (libc/netdb.c). */
#include <netinet/in.h>
#include <arpa/nameser.h>

int res_init(void);
int res_mkquery(int op, const char *dname, int class, int type,
                const unsigned char *data, int datalen,
                const unsigned char *newrr, unsigned char *buf, int buflen);
int res_send(const unsigned char *msg, int msglen, unsigned char *answer,
             int anslen);
int res_query(const char *dname, int class, int type, unsigned char *answer,
              int anslen);
int res_search(const char *dname, int class, int type, unsigned char *answer,
               int anslen);
int dn_expand(const unsigned char *msg, const unsigned char *eom,
              const unsigned char *src, char *dst, int dstlen);
int dn_comp(const char *src, unsigned char *dst, int dstlen,
            unsigned char **dnptrs, unsigned char **lastdnptr);
