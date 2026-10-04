#pragma once
#include <stdint.h>

/* Isochronous TD fields (xHCI 4.11.2.3, 6.4.1.3), apart so the host test
 * (tools/test_xhci_iso.c) runs the same code on hostile values.
 *
 * A TD of `len` bytes on an endpoint with max packet size `mps`, Max Burst
 * `burst` (packets per burst - 1: SuperSpeed 0..15, a high-speed
 * high-bandwidth endpoint's extra transactions 0..2) and Mult `mult`
 * (SuperSpeed bursts per interval - 1, 0..2): Transfer Burst Count (2 bits)
 * and Last Burst Packet Count (4 bits).  0, or -1 when the endpoint values
 * are out of range or the TD is more than one service interval carries
 * ((burst + 1) x (mult + 1) packets), which is also what keeps both counts
 * inside their fields. */
static inline int xhci_iso_td_fields(uint32_t len, uint32_t mps,
                                     uint32_t burst, uint32_t mult,
                                     uint32_t *tbc, uint32_t *tlbpc) {
    uint32_t pk, t, l;
    if (!len || !mps || mps > 1024 || burst > 15 || mult > 2) return -1;
    pk = (len + mps - 1) / mps;
    if (pk > (burst + 1) * (mult + 1)) return -1;
    t = (pk + burst) / (burst + 1) - 1;
    l = (pk - 1) % (burst + 1);
    if (t > 3 || l > 15) return -1;
    *tbc = t;
    *tlbpc = l;
    return 0;
}

/* The endpoint values from its descriptors (xHCI 6.2.3.4, USB 3.2
 * 9.6.7): `comp` is the SuperSpeed companion, used only at SuperSpeed
 * (`ss`); below it a high-speed periodic endpoint's wMaxPacketSize bits
 * 12:11 are its extra transactions.  -1 for reserved or impossible values
 * (bMaxBurst > 15, Mult 3, transactions 3, a packet size above 1024). */
static inline int xhci_ep_burst(int ss, int hs, int xfer, uint16_t wmps,
                                const uint8_t *comp, uint32_t *mps,
                                uint32_t *burst, uint32_t *mult) {
    *mps = wmps & 0x7FF;
    *burst = 0;
    *mult = 0;
    if (*mps == 0 || *mps > 1024) return -1;
    if (ss && comp) {
        if (comp[2] > 15) return -1;
        *burst = comp[2];
        if (xfer == 1) {
            if ((comp[3] & 3) == 3) return -1;
            *mult = comp[3] & 3;
        }
    } else if (hs && xfer != 2) {
        *burst = (wmps >> 11) & 3;
        if (*burst == 3) return -1;
    }
    return 0;
}
