#pragma once

#include <stdint.h>

typedef struct e1000_info {
    int present;
    uint16_t device_id;
    uint32_t mmio_phys;
    uint8_t irq;
    uint8_t mac[6];
    int mac_from_eeprom;   /* 1: EEPROM words 0-2, 0: RAL0/RAH0 fallback */
    uint32_t rx_next;      /* next RX descriptor to look at */
    uint32_t tx_next;      /* next free TX descriptor */
} e1000_info_t;

void e1000_init(void);
const e1000_info_t *e1000_get_info(void);
int e1000_send(const void *data, uint32_t len);
int e1000_poll(void);
/* " status=..." etc. for /proc/netif; returns the number of bytes written. */
int e1000_describe(char *buf, uint32_t cap);
