#pragma once

#include <stdint.h>

typedef struct r8169_info {
    int present;
    uint16_t vendor_id, device_id;
    uint32_t txcfg;        /* TxConfig at probe: the XID names the chip */
    const char *chip;      /* "RTL8168H/8111H" etc. */
    uint32_t flags;        /* R8169_F_* (drivers/r8169_hw.h) */
    uint32_t bar_phys;     /* MMIO base, or 0 when on I/O ports */
    uint16_t io_base;      /* I/O base when not MMIO */
    uint8_t irq;
    uint8_t mac[6];
    int mac_generated;     /* IDR was blank: a locally administered one */
    int phy_ok;            /* PHY answered on MDIO at attach */
    uint32_t resets;       /* chip resets after attach (TX stall, SERR, ...) */
    uint32_t rdu, fovw;    /* RX ring-full / FIFO-overflow interrupts */
} r8169_info_t;

/* Probe and bring up the first RTL8169/8168/8111/810x found; prints
 * "[R8169] not present" and does nothing when there is none. */
void r8169_init(void);
const r8169_info_t *r8169_get_info(void);
int r8169_send(const void *data, uint32_t len);
int r8169_poll(void);
/* "chip=... link=..." for /proc/netif; returns the number of bytes written. */
int r8169_describe(char *buf, uint32_t cap);
