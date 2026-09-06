#include "auth.h"
#include "../include/stdint.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/sys/types.h"
#include "../include/unistd.h"

#define PBKDF2_ITERS 100000
#define SHA256_BLOCK 64
#define SHA256_DIGEST 32

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t buf[SHA256_BLOCK];
    int used;
} sha256_ctx;

static const uint32_t k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void store_be64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = v;
        v >>= 8;
    }
}

static void sha256_transform(sha256_ctx *ctx, const uint8_t block[SHA256_BLOCK]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    for (int i = 0; i < 16; i++)
        w[i] = load_be32(block + i * 4);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k256[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bits = 0;
    ctx->used = 0;
}

static void sha256_update(sha256_ctx *ctx, const void *data, int len) {
    const uint8_t *p = data;
    ctx->bits += (uint64_t)len * 8;

    while (len > 0) {
        int n = SHA256_BLOCK - ctx->used;
        if (n > len) n = len;
        memcpy(ctx->buf + ctx->used, p, n);
        ctx->used += n;
        p += n;
        len -= n;
        if (ctx->used == SHA256_BLOCK) {
            sha256_transform(ctx, ctx->buf);
            ctx->used = 0;
        }
    }
}

static void sha256_final(sha256_ctx *ctx, uint8_t out[SHA256_DIGEST]) {
    uint64_t bits = ctx->bits;
    ctx->buf[ctx->used++] = 0x80;

    if (ctx->used > 56) {
        while (ctx->used < SHA256_BLOCK)
            ctx->buf[ctx->used++] = 0;
        sha256_transform(ctx, ctx->buf);
        ctx->used = 0;
    }

    while (ctx->used < 56)
        ctx->buf[ctx->used++] = 0;
    store_be64(ctx->buf + 56, bits);
    sha256_transform(ctx, ctx->buf);

    for (int i = 0; i < 8; i++)
        store_be32(out + i * 4, ctx->state[i]);
}

static void hmac_sha256(const uint8_t *key, int key_len,
                        const uint8_t *data, int data_len,
                        uint8_t out[SHA256_DIGEST]) {
    uint8_t k0[SHA256_BLOCK];
    uint8_t inner[SHA256_DIGEST];
    sha256_ctx ctx;

    memset(k0, 0, sizeof(k0));
    if (key_len > SHA256_BLOCK) {
        sha256_init(&ctx);
        sha256_update(&ctx, key, key_len);
        sha256_final(&ctx, k0);
    } else {
        memcpy(k0, key, key_len);
    }

    for (int i = 0; i < SHA256_BLOCK; i++)
        k0[i] ^= 0x36;
    sha256_init(&ctx);
    sha256_update(&ctx, k0, SHA256_BLOCK);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, inner);

    for (int i = 0; i < SHA256_BLOCK; i++)
        k0[i] ^= 0x36 ^ 0x5c;
    sha256_init(&ctx);
    sha256_update(&ctx, k0, SHA256_BLOCK);
    sha256_update(&ctx, inner, SHA256_DIGEST);
    sha256_final(&ctx, out);
}

static void pbkdf2_sha256(const char *password, const uint8_t *salt, int salt_len,
                          int iterations, uint8_t out[SHA256_DIGEST]) {
    uint8_t block[64];
    uint8_t u[SHA256_DIGEST];

    memcpy(block, salt, salt_len);
    block[salt_len] = 0;
    block[salt_len + 1] = 0;
    block[salt_len + 2] = 0;
    block[salt_len + 3] = 1;

    hmac_sha256((const uint8_t *)password, strlen(password), block, salt_len + 4, u);
    memcpy(out, u, SHA256_DIGEST);

    for (int i = 1; i < iterations; i++) {
        hmac_sha256((const uint8_t *)password, strlen(password), u, SHA256_DIGEST, u);
        for (int j = 0; j < SHA256_DIGEST; j++)
            out[j] ^= u[j];
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, int out_cap) {
    int len = strlen(hex);
    if ((len & 1) || len / 2 > out_cap)
        return -1;
    for (int i = 0; i < len / 2; i++) {
        int hi = hex_value(hex[i * 2]);
        int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (hi << 4) | lo;
    }
    return len / 2;
}

static void hex_encode(const uint8_t *in, int len, char *out, int out_cap) {
    static const char hex[] = "0123456789abcdef";
    int pos = 0;
    for (int i = 0; i < len && pos + 2 < out_cap; i++) {
        out[pos++] = hex[in[i] >> 4];
        out[pos++] = hex[in[i] & 0xf];
    }
    out[pos] = '\0';
}

static int parse_uint(const char *s, int *value) {
    int n = 0;
    if (!s || !s[0]) return 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        n = n * 10 + (s[i] - '0');
    }
    *value = n;
    return 1;
}

static int constant_time_equal(const char *a, const char *b) {
    int la = strlen(a), lb = strlen(b);
    unsigned char diff = la ^ lb;
    int max = la > lb ? la : lb;
    for (int i = 0; i < max; i++) {
        unsigned char ca = i < la ? a[i] : 0;
        unsigned char cb = i < lb ? b[i] : 0;
        diff |= ca ^ cb;
    }
    return diff == 0;
}

static int legacy_fnv_verify(const char *stored, const char *password) {
    unsigned long long h = 1469598103934665603ULL;
    const char *salt = stored + 7;
    const char *hash = strchr(salt, '$');
    char hex[17];

    if (!hash)
        return 0;
    for (const char *p = salt; p < hash; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    h ^= ':';
    h *= 1099511628211ULL;
    for (int i = 0; password[i]; i++) {
        h ^= (unsigned char)password[i];
        h *= 1099511628211ULL;
    }
    hex_encode((const uint8_t *)&h, 0, hex, sizeof(hex));
    static const char chars[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        hex[i] = chars[h & 0xf];
        h >>= 4;
    }
    hex[16] = '\0';
    return strcmp(hash + 1, hex) == 0;
}

int maero_password_hash(const char *salt_hex, const char *password, char *out, int out_cap) {
    uint8_t salt[32];
    uint8_t digest[SHA256_DIGEST];
    char digest_hex[SHA256_DIGEST * 2 + 1];
    int salt_len = hex_decode(salt_hex, salt, sizeof(salt));

    if (salt_len < 16 || !password || out_cap < MAERO_HASH_MAX)
        return 0;

    pbkdf2_sha256(password, salt, salt_len, PBKDF2_ITERS, digest);
    hex_encode(digest, sizeof(digest), digest_hex, sizeof(digest_hex));
    snprintf(out, out_cap, "$maero-pbkdf2-sha256$%d$%s$%s",
             PBKDF2_ITERS, salt_hex, digest_hex);
    return 1;
}

int maero_password_verify(const char *stored, const char *password) {
    char copy[MAERO_HASH_MAX];
    char computed[MAERO_HASH_MAX];
    char *save = (char *)0;
    char *alg, *iters_s, *salt, *hash;
    int iterations;

    if (!stored || !password)
        return 0;

    if (strncmp(stored, "$maero-pbkdf2-sha256$", 21) != 0) {
        if (strncmp(stored, "$maero$", 7) == 0)
            return legacy_fnv_verify(stored, password);
        return strcmp(stored, password) == 0;
    }

    if ((int)strlen(stored) >= (int)sizeof(copy))
        return 0;
    strcpy(copy, stored);

    alg = strtok_r(copy, "$", &save);
    iters_s = strtok_r((char *)0, "$", &save);
    salt = strtok_r((char *)0, "$", &save);
    hash = strtok_r((char *)0, "$", &save);
    if (!alg || !iters_s || !salt || !hash)
        return 0;
    if (strcmp(alg, "maero-pbkdf2-sha256") != 0)
        return 0;
    if (!parse_uint(iters_s, &iterations) || iterations < 10000)
        return 0;
    if (iterations != PBKDF2_ITERS)
        return 0;

    if (!maero_password_hash(salt, password, computed, sizeof(computed)))
        return 0;
    return constant_time_equal(computed, stored);
}

int maero_password_make_salt(const char *user, char *out, int out_cap) {
    struct timeval tv;
    char seed[96];
    uint8_t salt[16];
    uint8_t digest[SHA256_DIGEST];
    sha256_ctx ctx;

    if (!user || !out || out_cap < 33)
        return 0;

    if (getrandom(salt, sizeof(salt), 0) == (int)sizeof(salt)) {
        hex_encode(salt, sizeof(salt), out, out_cap);
        return 1;
    }

    gettimeofday(&tv, (void *)0);
    snprintf(seed, sizeof(seed), "%s:%d:%ld:%ld", user, getpid(), tv.tv_sec, tv.tv_usec);
    sha256_init(&ctx);
    sha256_update(&ctx, seed, strlen(seed));
    sha256_final(&ctx, digest);
    hex_encode(digest, 16, out, out_cap);
    return 1;
}
