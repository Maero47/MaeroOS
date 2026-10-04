#pragma once

#include <stdint.h>

void random_init(uint32_t seed0, uint32_t seed1);
void random_mix_u32(uint32_t value);
void random_get_bytes(void *buf, uint32_t len);
uint32_t random_u32(void);
/* TCP initial sequence number (RFC 6528) for a connection identified by
 * nids words (addresses, ports), at monotonic time now_ns. */
uint32_t random_tcp_isn(const uint32_t *ids, uint32_t nids, uint64_t now_ns);
/* The ChaCha20 block function (RFC 8439 2.3), for the host test. */
void random_chacha20_block(const uint32_t k[8], uint32_t counter,
                           const uint32_t nonce[3], uint32_t out[16]);
