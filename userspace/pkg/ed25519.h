#pragma once
#include <stddef.h>
#include <stdint.h>

/* Ed25519 signature check (RFC 8032) over TweetNaCl's crypto_sign_open.
 * Returns 0 if sig is a valid signature of msg under pk, -1 otherwise
 * (including a non-canonical S, and out of memory). */
int ed25519_verify(const uint8_t sig[64], const void *msg, size_t len,
                   const uint8_t pk[32]);
