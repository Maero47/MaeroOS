#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ed25519.h"
#include "tweetnacl.h"

/* TweetNaCl references randombytes() from its key generation functions;
 * pkg only verifies, so nothing ever calls this. */
void randombytes(unsigned char *buf, unsigned long long len) {
    (void)buf;
    (void)len;
    _exit(127);
}

/* Group order L, little endian.  TweetNaCl reduces S mod L without
 * checking it, which accepts a second encoding of every valid signature;
 * RFC 8032 section 5.1.7 requires S < L. */
static const uint8_t order_l[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
};

static int s_canonical(const uint8_t s[32]) {
    for (int i = 31; i >= 0; i--) {
        if (s[i] < order_l[i]) return 1;
        if (s[i] > order_l[i]) return 0;
    }
    return 0;   /* S == L */
}

int ed25519_verify(const uint8_t sig[64], const void *msg, size_t len,
                   const uint8_t pk[32]) {
    unsigned char *sm, *m;
    unsigned long long mlen;
    int rc;

    if (!s_canonical(sig + 32) || len > (size_t)-1 - 64) return -1;
    sm = (unsigned char *)malloc(len + 64);
    m = (unsigned char *)malloc(len + 64);
    if (!sm || !m) {
        free(sm);
        free(m);
        return -1;
    }
    memcpy(sm, sig, 64);
    memcpy(sm + 64, msg, len);
    rc = crypto_sign_open(m, &mlen, sm, len + 64, pk);
    free(sm);
    free(m);
    return rc == 0 && mlen == len ? 0 : -1;
}
