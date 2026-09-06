#pragma once

#include <stdint.h>

#define PCI_MAX_DEVICES 64

typedef struct pci_device {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;
    uint8_t irq_line;
    uint8_t irq_pin;
    uint32_t bar[6];
} pci_device_t;

void pci_init(void);
uint32_t pci_read_config32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset);
void pci_write_config32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset, uint32_t value);
int pci_device_count(void);
const pci_device_t *pci_get_device(int index);
const pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);
const pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);

