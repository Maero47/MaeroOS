/* Host test of drivers/alsa_resample.h (the kernel's 44.1 -> 48 kHz path).
 *
 *     cc -O2 -o build/test_alsa_resample tools/test_alsa_resample.c && build/test_alsa_resample
 *
 * Feeds a full-scale square wave (+-32767, the worst case for the
 * interpolation product) and a sine at 44.1 kHz and checks that every output
 * frame is within 1 LSB of the exact interpolation between the two input
 * samples around it (no sign flips, no full-scale clicks) and that
 * 48000/44100 as many frames come out.
 *
 * It also reports how the old `(b - a) * (int32_t)pos` would have done.  That
 * product overflows 32 bits on full-scale steps, which is undefined behaviour
 * in C (the reason for the 64-bit product now); with the wrapping multiply
 * i686 actually performs, though, only the low 32 bits of the product reach
 * the int16 result, so the old code happened to produce the same samples. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../drivers/alsa_resample.h"

/* The old code, `(b - a) * (int32_t)pos` in 32 bits; written with an
 * explicit wrap (what i686 imul does) since the signed overflow is UB. */
static int16_t lerp_int32(int32_t a, int32_t b, uint32_t pos) {
    return (int16_t)(a + ((int32_t)((uint32_t)(b - a) * pos) >> 16));
}

static int run(const char *name, int square) {
    struct alsa_rs rs = { (uint32_t)((44100ULL << 16) / 48000), 0, 0, 0, 0 };
    int16_t out[2 * 64];
    long in_frames = 44100 * 2, out_frames = 0, bad = 0, old_bad = 0;

    for (long i = 0; i < in_frames; i++) {
        int32_t v = square ? ((i / 50) % 2 ? -32767 : 32767)
                           : (int32_t)lrint(32767 * sin(2 * M_PI * 441 * i / 44100.0));
        int32_t a = rs.pl, have = rs.have_prev;
        uint32_t pos0 = rs.pos;
        uint32_t n = alsa_rs_frame(&rs, v, -v, out, 64);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t pos = pos0 + k * rs.step;
            if (!have) continue;
            double exact = a + (double)(v - a) * pos / 65536.0;
            if (fabs(out[2 * k] - exact) > 1.0 || fabs(out[2 * k + 1] + exact) > 1.0)
                bad++;
            if (fabs(lerp_int32(a, v, pos) - exact) > 1.0)
                old_bad++;
        }
        out_frames += n;
    }
    double ratio = (double)out_frames / in_frames;
    int ok = bad == 0 && ratio > 48000.0 / 44100 - 0.001 && ratio < 48000.0 / 44100 + 0.001;
    printf("%-7s %ld in -> %ld out (ratio %.5f), %ld frames off by > 1 LSB "
           "(old 32-bit product, wrapping: %ld): %s\n",
           name, in_frames, out_frames, ratio, bad, old_bad, ok ? "ok" : "FAIL");
    return !ok;
}

int main(void) {
    int fails = run("square", 1) + run("sine", 0);
    printf("test_alsa_resample: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
