/*
 * Host test of the isochronous TD fields (drivers/usb/xhci_iso.h): the
 * endpoint values taken from the descriptors (the SuperSpeed companion only
 * at SuperSpeed, reserved values refused) and the Transfer Burst Count /
 * Last Burst Packet Count of a TD, with hostile descriptors: whatever the
 * device claims, an accepted TD's counts fit their 2- and 4-bit fields, so
 * `tbc << 7` and `tlbpc << 16` never reach BEI (bit 9) or the TRB type
 * (bits 10-15).  Exhaustive over packet sizes, bursts, mults and lengths.
 */
#include <stdio.h>
#include <stdint.h>
#include "usb/xhci_iso.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void) {
    uint32_t mps, burst, mult, tbc, tlbpc;
    uint8_t comp[6] = { 6, 0x30, 255, 3, 0, 0 };

    /* A full-speed device with a bogus companion (bMaxBurst 255, Mult 3):
     * the companion is ignored below SuperSpeed. */
    CHECK(xhci_ep_burst(0, 0, 1, 8, comp, &mps, &burst, &mult) == 0 &&
          mps == 8 && burst == 0 && mult == 0, "FS ignores the companion");
    /* ... so a 512-byte TD on its 8-byte endpoint is refused. */
    CHECK(xhci_iso_td_fields(512, 8, burst, mult, &tbc, &tlbpc) == -1,
          "FS 512-byte TD on an 8-byte endpoint accepted");
    /* At SuperSpeed the same companion is reserved/out of range. */
    CHECK(xhci_ep_burst(1, 0, 1, 1024, comp, &mps, &burst, &mult) == -1,
          "bMaxBurst 255 accepted");
    comp[2] = 15;
    CHECK(xhci_ep_burst(1, 0, 1, 1024, comp, &mps, &burst, &mult) == -1,
          "Mult 3 accepted");
    comp[3] = 2;
    CHECK(xhci_ep_burst(1, 0, 1, 1024, comp, &mps, &burst, &mult) == 0 &&
          burst == 15 && mult == 2, "valid SS companion refused");
    /* the largest SS isoch TD: 48 packets = 3 bursts of 16 */
    CHECK(xhci_iso_td_fields(48 * 1024, 1024, 15, 2, &tbc, &tlbpc) == 0 &&
          tbc == 2 && tlbpc == 15, "48-packet TD: tbc %u tlbpc %u", tbc, tlbpc);
    CHECK(xhci_iso_td_fields(48 * 1024 + 1, 1024, 15, 2, &tbc, &tlbpc) == -1,
          "49-packet TD accepted");
    /* high speed: bits 12:11 are extra transactions, 3 is reserved */
    CHECK(xhci_ep_burst(0, 1, 1, (uint16_t)(1024 | (2 << 11)), 0, &mps, &burst,
                        &mult) == 0 && burst == 2 && mps == 1024, "HS x3");
    CHECK(xhci_ep_burst(0, 1, 1, (uint16_t)(1024 | (3 << 11)), 0, &mps, &burst,
                        &mult) == -1, "HS transactions 3 accepted");
    CHECK(xhci_ep_burst(0, 0, 1, 0x7FF, 0, &mps, &burst, &mult) == -1,
          "2047-byte packets accepted");
    /* QEMU's usb-audio: 192-byte packets at full speed, one per TD */
    CHECK(xhci_iso_td_fields(192, 192, 0, 0, &tbc, &tlbpc) == 0 && tbc == 0 &&
          tlbpc == 0, "192-byte FS TD");
    CHECK(xhci_iso_td_fields(193, 192, 0, 0, &tbc, &tlbpc) == -1,
          "two packets on a one-packet FS interval accepted");
    CHECK(xhci_iso_td_fields(0, 192, 0, 0, &tbc, &tlbpc) == -1, "empty TD");

    /* Exhaustive: any burst/mult/mps (hostile ones included) and length;
     * an accepted TD has counts inside their fields and no more packets
     * than one interval holds. */
    long accepted = 0;
    for (uint32_t b = 0; b < 40; b++)
        for (uint32_t m = 0; m < 6; m++)
            for (uint32_t p = 0; p <= 1100; p += (p < 64 ? 1 : 37))
                for (uint32_t len = 0; len <= 70000; len += (len < 4096 ? 7 : 997)) {
                    tbc = tlbpc = 0xFFFFFFFFU;
                    if (xhci_iso_td_fields(len, p, b, m, &tbc, &tlbpc) != 0)
                        continue;
                    accepted++;
                    uint32_t pk = (len + p - 1) / p;
                    uint32_t ctl = (tbc << 7) | (tlbpc << 16);
                    CHECK(tbc <= 3 && tlbpc <= 15 && b <= 15 && m <= 2 &&
                          p <= 1024 && pk <= (b + 1) * (m + 1) &&
                          !(ctl & ((1U << 9) | (0x3FU << 10))),
                          "len %u mps %u burst %u mult %u: tbc %u tlbpc %u",
                          len, p, b, m, tbc, tlbpc);
                    if (fails > 10) return 1;
                }
    CHECK(accepted > 1000, "only %ld TDs accepted", accepted);
    printf(fails ? "test_xhci_iso: FAILED\n" : "test_xhci_iso: ok (%ld TDs)\n",
           accepted);
    return fails != 0;
}
