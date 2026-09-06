#pragma once

#include <stdint.h>

typedef struct rtl8139_info {
    int present;
    uint16_t io_base;
    uint8_t irq;
    uint8_t mac[6];
    uint32_t rx_config;
    uint32_t tx_config;
    uint32_t rx_offset;
    uint32_t tx_index;
} rtl8139_info_t;

void rtl8139_init(void);
const rtl8139_info_t *rtl8139_get_info(void);
int rtl8139_send(const void *data, uint32_t len);
int rtl8139_poll(void);
