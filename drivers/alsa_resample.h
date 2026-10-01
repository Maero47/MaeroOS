#pragma once
#include <stdint.h>

/*
 * Linear-interpolation resampler of drivers/alsa.c, on its own so that
 * tools/test_alsa_resample.c can run it on the host.
 *
 * `pos` is the 16.16 position of the next output frame between the previous
 * input frame (pl, pr) and the one being fed; `step` is in_rate / out_rate in
 * 16.16.  The difference of two 16-bit samples times a 16-bit fraction needs
 * 33 bits, hence the 64-bit product.
 */
struct alsa_rs {
    uint32_t step, pos;
    int32_t pl, pr;
    int have_prev;
};

static inline int16_t alsa_rs_lerp(int32_t a, int32_t b, uint32_t pos) {
    return (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)pos) >> 16));
}

/* Feed one input frame (l, r); writes up to `cap` stereo frames to `out` and
 * returns how many. */
static inline uint32_t alsa_rs_frame(struct alsa_rs *rs, int32_t l, int32_t r,
                                     int16_t *out, uint32_t cap) {
    uint32_t n = 0;

    if (!rs->have_prev) {
        rs->pl = l; rs->pr = r; rs->pos = 0; rs->have_prev = 1;
        return 0;
    }
    while (rs->pos < 65536 && n < cap) {
        out[2 * n]     = alsa_rs_lerp(rs->pl, l, rs->pos);
        out[2 * n + 1] = alsa_rs_lerp(rs->pr, r, rs->pos);
        n++;
        rs->pos += rs->step;
    }
    rs->pos -= 65536;
    rs->pl = l; rs->pr = r;
    return n;
}
