/*
 * Host test for the kernel random generator (kernel/random.c); run by
 * tools/test_random.py.
 *
 * The ChaCha20 block function against RFC 8439's test vector (2.3.2); then
 * the generator itself: different outputs per call, no short cycle, byte
 * frequencies close to uniform, every request length filled, and TCP ISNs
 * that differ per connection but advance with time for one connection.
 * The file is compiled in with its lock and the PIT stubbed out.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <kernel/klock.h>

uint32_t kspin_lock_irqsave(kspinlock_t *l) { (void)l; return 0; }
void kspin_unlock_irqrestore(kspinlock_t *l, uint32_t f) { (void)l; (void)f; }
uint32_t pit_ticks(void) { return 0; }

#include "../kernel/random.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    /* RFC 8439 2.3.2 */
    uint32_t k[8], n[3] = { 0x09000000, 0x4a000000, 0x00000000 }, out[16];
    for (int i = 0; i < 8; i++)
        k[i] = (uint32_t)(4 * i) | (uint32_t)(4 * i + 1) << 8 |
               (uint32_t)(4 * i + 2) << 16 | (uint32_t)(4 * i + 3) << 24;
    random_chacha20_block(k, 1, n, out);
    static const uint32_t want[16] = {
        0xe4e7f110, 0x15593bd1, 0x1fdd0f50, 0xc47120a3,
        0xc7f4d1c7, 0x0368c033, 0x9aaa2204, 0x4e6cd4c3,
        0x466482d2, 0x09aa9f07, 0x05d7c214, 0xa2028bd9,
        0xd19c12b5, 0xb94e16de, 0xe883d0cb, 0x4e3c50a2,
    };
    CHECK(memcmp(out, want, sizeof(want)) == 0, "ChaCha20 block differs from RFC 8439 2.3.2");

    random_init(0x12345678, 0x9abcdef0);
    uint32_t a = random_u32(), b = random_u32(), c = random_u32();
    CHECK(a != b && b != c && a != c, "three equal words in a row");

    /* every length is filled (nothing left at a canary value) */
    for (uint32_t len = 0; len < 700; len += 13) {
        static uint8_t buf[1024];
        memset(buf, 0xA5, sizeof(buf));
        random_get_bytes(buf, len);
        uint32_t same = 0;
        for (uint32_t i = 0; i < len; i++) same += buf[i] == 0xA5;
        CHECK(same <= len / 32 + 3, "length %u: %u bytes left as 0xA5", len, same);
        CHECK(buf[len] == 0xA5, "length %u: wrote past the end", len);
    }

    /* byte frequencies over 4 MiB: each within 5% of the mean */
    static uint32_t hist[256];
    static uint8_t big[4096];
    for (int r = 0; r < 1024; r++) {
        random_get_bytes(big, sizeof(big));
        for (int i = 0; i < 4096; i++) hist[big[i]]++;
    }
    for (int v = 0; v < 256; v++)
        CHECK(hist[v] > 16384 * 95 / 100 && hist[v] < 16384 * 105 / 100,
              "byte %d seen %u times of 16384", v, hist[v]);

    /* the 16-bit port draws are not sequential */
    uint32_t seq = 0, prev = random_u32() & 0x3FFF;
    for (int i = 0; i < 1000; i++) {
        uint32_t p = random_u32() & 0x3FFF;
        if (p == prev + 1) seq++;
        prev = p;
    }
    CHECK(seq < 5, "%u sequential port draws of 1000", seq);

    /* ISNs: two tuples unrelated; one tuple advances with time */
    uint32_t t1[3] = { (1234u << 16) | 80, 0x0100007f, 0x0200007f };
    uint32_t t2[3] = { (1235u << 16) | 80, 0x0100007f, 0x0200007f };
    uint32_t i1 = random_tcp_isn(t1, 3, 1000000), i2 = random_tcp_isn(t2, 3, 1000000);
    uint32_t i1b = random_tcp_isn(t1, 3, 2000000);
    CHECK(i1 != i2 && i1 - i2 != 1 && i2 - i1 != 1, "ISNs of neighbouring ports related");
    CHECK(i1b - i1 == (2000000u >> 12) - (1000000u >> 12), "ISN does not advance with time");

    if (fails) {
        printf("test_random: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_random: ok\n");
    return 0;
}
