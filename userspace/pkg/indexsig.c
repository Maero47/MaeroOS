#include <string.h>
#include "ed25519.h"
#include "indexsig.h"

#define HDR    "#maeros-index v1 serial="
#define SIGTAG "#sig ed25519 "

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int index_verify(const char *buf, int n, const uint8_t pk[32],
                 unsigned long *serial, int *signed_len, const char **why) {
    int hl = (int)strlen(HDR), tl = (int)strlen(SIGTAG), end = n, start, i;
    unsigned long s = 0;
    uint8_t sig[64];

    if (n < hl || memcmp(buf, HDR, (size_t)hl)) {
        *why = "index is not signed (rebuild the repo with `make repo`)";
        return -1;
    }
    /* The signature is the last line, optionally newline-terminated. */
    if (buf[end - 1] == '\n') end--;
    start = end;
    while (start > 0 && buf[start - 1] != '\n') start--;
    if (start == 0 || end - start != tl + 128 ||
        memcmp(buf + start, SIGTAG, (size_t)tl)) {
        *why = "index signature is missing";
        return -1;
    }
    for (i = 0; i < 64; i++) {
        int hi = hexval(buf[start + tl + 2 * i]);
        int lo = hexval(buf[start + tl + 2 * i + 1]);
        if (hi < 0 || lo < 0) {
            *why = "index signature is malformed";
            return -1;
        }
        sig[i] = (uint8_t)(hi << 4 | lo);
    }
    if (ed25519_verify(sig, buf, (size_t)start, pk)) {
        *why = "index signature is invalid (tampered, or signed with another key)";
        return -1;
    }

    /* Only now is the content trusted: parse the serial it vouches for. */
    for (i = 0; i < start; i++)
        if (!buf[i]) {
            *why = "index contains a NUL byte";
            return -1;
        }
    for (i = hl; i < start && buf[i] >= '0' && buf[i] <= '9'; i++) {
        if (s > (0xFFFFFFFFUL - 9) / 10) break;
        s = s * 10 + (unsigned long)(buf[i] - '0');
    }
    if (i == hl || buf[i] != '\n') {
        *why = "index header is malformed";
        return -1;
    }
    *serial = s;
    *signed_len = start;
    return 0;
}
