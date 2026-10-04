/*
 * Kernel random numbers: /dev/urandom, getrandom(2), AT_RANDOM, lwIP's
 * LWIP_RAND() (DNS ids, initial ports), ephemeral ports and TCP initial
 * sequence numbers.
 *
 * A ChaCha20 generator (RFC 8439 block function) with fast key erasure: every
 * request is produced from the current 256-bit key and then a fresh key is
 * taken from the same keystream, so the output never reveals the key and a
 * captured key does not reveal earlier output.  (The xoshiro128+ generator
 * this replaces could be recovered from four outputs and then predicted.)
 *
 * Entropy: RDSEED/RDRAND when the CPU has them, the TSC (several jittered
 * samples), the boot loader's addresses at random_init(); afterwards every
 * timer tick and any random_mix_u32() caller adds the event and the TSC to a
 * 16-word pool, which is folded into the key before the next request.  The
 * pool is cheap to add to (interrupt context) and only hashed on demand.
 *
 * ChaCha20 written from RFC 8439 section 2.3; no code copied.
 */
#include "random.h"
#include "../arch/i686/cpu/pit.h"
#include "../lib/string.h"
#include <kernel/klock.h>
#include <stdint.h>

static uint32_t key[8];
static uint32_t isn_key[8];          /* TCP ISN secret (RFC 6528) */
static uint32_t pool[16];
static uint32_t pool_pos, pool_new;
static uint32_t block_ctr, block_epoch;
static kspinlock_t rng_lock = KSPINLOCK_INIT("random");

static uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint32_t rotl32(uint32_t x, uint32_t r) {
    return (x << r) | (x >> ((32 - r) & 31));
}

/* ── ChaCha20 block (RFC 8439 2.3) ───────────────────────────────────────── */

#define QR(a, b, c, d) do {                      \
    a += b; d ^= a; d = rotl32(d, 16);           \
    c += d; b ^= c; b = rotl32(b, 12);           \
    a += b; d ^= a; d = rotl32(d, 8);            \
    c += d; b ^= c; b = rotl32(b, 7);            \
} while (0)

void random_chacha20_block(const uint32_t k[8], uint32_t counter,
                           const uint32_t nonce[3], uint32_t out[16]) {
    uint32_t in[16] = {
        0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U,
        k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7],
        counter, nonce[0], nonce[1], nonce[2],
    };
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

/* ── hardware sources ────────────────────────────────────────────────────── */

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                     : "a"(leaf), "c"(sub));
}

static int have_rdrand, have_rdseed;

static void detect_hw(void) {
    uint32_t r[4];
    cpuid(0, 0, r);
    uint32_t max = r[0];
    cpuid(1, 0, r);
    have_rdrand = (r[2] >> 30) & 1;
    if (max >= 7) {
        cpuid(7, 0, r);
        have_rdseed = (r[1] >> 18) & 1;
    }
}

/* One word from RDSEED, else RDRAND; 0 (and *ok = 0) when neither works. */
static uint32_t hw_word(int *ok) {
    uint32_t v;
    uint8_t good;
    for (int tries = 0; have_rdseed && tries < 16; tries++) {
        __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(good));
        if (good) { *ok = 1; return v; }
    }
    for (int tries = 0; have_rdrand && tries < 16; tries++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(good));
        if (good) { *ok = 1; return v; }
    }
    *ok = 0;
    return 0;
}

/* ── the pool ────────────────────────────────────────────────────────────── */

static void pool_add_locked(uint32_t v) {
    uint64_t t = rdtsc64();
    uint32_t i = pool_pos++ & 15;
    pool[i] = rotl32(pool[i], 7) ^ v ^ (uint32_t)t ^ rotl32((uint32_t)(t >> 32), 13);
    pool[(i + 5) & 15] += rotl32(pool[i], 11) ^ pool_pos;
    pool_new++;
}

/* Fold the pool into the key: the next key is a ChaCha20 block of the old
 * key XOR the pool, so it depends on every bit of both and reveals neither. */
static void reseed_locked(void) {
    uint32_t blk[16];
    uint32_t k[8], nonce[3];
    for (int i = 0; i < 8; i++) k[i] = key[i] ^ pool[i];
    nonce[0] = pool[8] ^ pool[11] ^ pool[14];
    nonce[1] = pool[9] ^ pool[12] ^ pool[15];
    nonce[2] = pool[10] ^ pool[13] ^ 0x72657365U;
    random_chacha20_block(k, 0xFFFFFFFFU, nonce, blk);
    for (int i = 0; i < 8; i++) key[i] = blk[i];
    for (int i = 0; i < 16; i++) pool[i] = blk[i] ^ pool[i];
    pool_new = 0;
    block_ctr = 0;
    block_epoch++;
    memset(blk, 0, sizeof(blk));
    memset(k, 0, sizeof(k));
}

/* Next 64 bytes of keystream under the current key. */
static void keystream_locked(uint32_t out[16]) {
    uint32_t nonce[3] = { block_epoch, 0x4d616572U, 0x6f4f5321U };
    random_chacha20_block(key, block_ctr++, nonce, out);
    if (block_ctr == 0) block_epoch++;
}

/* Fast key erasure: the key that produced this request's bytes is gone. */
static void rekey_locked(void) {
    uint32_t blk[16];
    keystream_locked(blk);
    for (int i = 0; i < 8; i++) key[i] = blk[i];
    memset(blk, 0, sizeof(blk));
}

void random_mix_u32(uint32_t value) {
    uint32_t fl = kspin_lock_irqsave(&rng_lock);
    pool_add_locked(value);
    kspin_unlock_irqrestore(&rng_lock, fl);
}

/* Early boot, one CPU, before the lock machinery is up: no lock. */
void random_init(uint32_t seed0, uint32_t seed1) {
    detect_hw();
    pool_add_locked(seed0);
    pool_add_locked(seed1);
    /* Hardware words when there are any; TSC samples around cpuid (whose
     * latency varies under a hypervisor) either way. */
    for (int i = 0; i < 32; i++) {
        int ok;
        uint32_t w = hw_word(&ok);
        uint32_t r[4];
        cpuid(0, 0, r);
        pool_add_locked(w ^ (uint32_t)rdtsc64() ^ ((uint32_t)i << 24));
    }
    reseed_locked();
    for (int i = 0; i < 16; i++) pool_add_locked((uint32_t)rdtsc64());
    reseed_locked();
    uint32_t blk[16];
    keystream_locked(blk);
    for (int i = 0; i < 8; i++) isn_key[i] = blk[i];
    rekey_locked();
    memset(blk, 0, sizeof(blk));
}

void random_get_bytes(void *buf, uint32_t len) {
    uint8_t *out = (uint8_t *)buf;
    /* At most 256 bytes per lock hold: interrupts stay off only briefly. */
    while (len) {
        uint32_t chunk = len < 256 ? len : 256;
        uint32_t fl = kspin_lock_irqsave(&rng_lock);
        int ok;
        uint32_t hw = hw_word(&ok);
        if (ok) pool_add_locked(hw);
        pool_add_locked(chunk);
        reseed_locked();
        for (uint32_t done = 0; done < chunk; done += 64) {
            uint32_t blk[16];
            keystream_locked(blk);
            uint32_t n = chunk - done < 64 ? chunk - done : 64;
            memcpy(out + done, blk, n);
            memset(blk, 0, sizeof(blk));
        }
        rekey_locked();
        kspin_unlock_irqrestore(&rng_lock, fl);
        out += chunk;
        len -= chunk;
    }
}

uint32_t random_u32(void) {
    uint32_t v;
    random_get_bytes(&v, sizeof(v));
    return v;
}

/* RFC 6528: ISN = M + F(localip, localport, remoteip, remoteport, secret),
 * M a 4-microsecond clock (the TSC-based monotonic clock here), F a keyed
 * function of the connection's identity: ChaCha20 under a boot-time secret.
 * Successive connections of one 4-tuple still get increasing ISNs; another
 * tuple's are unrelated and unguessable. */
uint32_t random_tcp_isn(const uint32_t *ids, uint32_t nids, uint64_t now_ns) {
    uint32_t k[8], blk[16], nonce[3] = { 0, 0, 0 };
    for (int i = 0; i < 8; i++) k[i] = isn_key[i];
    for (uint32_t i = 0; i < nids; i++) {
        if (i < 3) nonce[i] ^= ids[i];
        else k[i % 8] ^= rotl32(ids[i], i & 31);
    }
    random_chacha20_block(k, 0x6528U, nonce, blk);
    uint32_t f = blk[0];
    memset(k, 0, sizeof(k));
    memset(blk, 0, sizeof(blk));
    return f + (uint32_t)(now_ns >> 12);          /* ~4.1 us ticks */
}
