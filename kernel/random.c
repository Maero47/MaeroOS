#include "random.h"
#include "../arch/i686/cpu/pit.h"
#include <stdint.h>

static uint32_t rng_state[4];
static uint32_t rng_counter;

static uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint32_t rotl32(uint32_t x, uint32_t r) {
    return (x << r) | (x >> (32 - r));
}

static uint32_t splitmix32(uint32_t *x) {
    uint32_t z = (*x += 0x9e3779b9U);
    z = (z ^ (z >> 16)) * 0x85ebca6bU;
    z = (z ^ (z >> 13)) * 0xc2b2ae35U;
    return z ^ (z >> 16);
}

static uint32_t next_u32(void) {
    uint32_t result = rotl32(rng_state[0] + rng_state[3], 7) + rng_state[0];
    uint32_t t = rng_state[1] << 9;

    rng_state[2] ^= rng_state[0];
    rng_state[3] ^= rng_state[1];
    rng_state[1] ^= rng_state[2];
    rng_state[0] ^= rng_state[3];
    rng_state[2] ^= t;
    rng_state[3] = rotl32(rng_state[3], 11);
    return result;
}

void random_mix_u32(uint32_t value) {
    uint64_t t = rdtsc64();
    uint32_t x = value ^ (uint32_t)t ^ (uint32_t)(t >> 32) ^
                 pit_ticks() ^ (++rng_counter * 0x9e3779b9U);
    rng_state[rng_counter & 3] ^= splitmix32(&x);

    for (int i = 0; i < 4; i++)
        (void)next_u32();
}

void random_init(uint32_t seed0, uint32_t seed1) {
    uint64_t t = rdtsc64();
    uint32_t seed = seed0 ^ rotl32(seed1, 13) ^
                    (uint32_t)t ^ (uint32_t)(t >> 32) ^ 0xa5a5f00dU;

    for (int i = 0; i < 4; i++)
        rng_state[i] = splitmix32(&seed);
    rng_counter = seed;

    for (int i = 0; i < 16; i++)
        (void)next_u32();
}

void random_get_bytes(void *buf, uint32_t len) {
    uint8_t *out = (uint8_t *)buf;
    uint32_t word = 0;

    random_mix_u32((uint32_t)(uintptr_t)buf ^ len);
    for (uint32_t i = 0; i < len; i++) {
        if ((i & 3) == 0)
            word = next_u32();
        out[i] = (uint8_t)(word >> ((i & 3) * 8));
    }
}

