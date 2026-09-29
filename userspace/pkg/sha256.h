#pragma once
#include <stddef.h>
#include <stdint.h>

/* SHA-256, used by pkg to check downloads against the repo index. */
typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t  buf[64];
    size_t   used;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t out[32]);
/* One-shot digest of data, written as 64 lowercase hex chars + NUL. */
void sha256_hex(const void *data, size_t len, char out[65]);
