#include "pci.h"
#include "../arch/i686/include/io.h"
#include "../kernel/printk.h"
#include "../lib/string.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static pci_device_t devices[PCI_MAX_DEVICES];
static int ndevices;

static uint32_t pci_config_addr(uint8_t bus, uint8_t slot, uint8_t func,
                                uint8_t offset) {
    return 0x80000000U |
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) |
           ((uint32_t)offset & 0xFCU);
}

uint32_t pci_read_config32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

static uint16_t pci_read_config16(uint8_t bus, uint8_t slot, uint8_t func,
                                  uint8_t offset) {
    uint32_t v = pci_read_config32(bus, slot, func, offset);
    return (uint16_t)(v >> ((offset & 2) * 8));
}

static uint8_t pci_read_config8(uint8_t bus, uint8_t slot, uint8_t func,
                                uint8_t offset) {
    uint32_t v = pci_read_config32(bus, slot, func, offset);
    return (uint8_t)(v >> ((offset & 3) * 8));
}

static void pci_add_device(uint8_t bus, uint8_t slot, uint8_t func) {
    if (ndevices >= PCI_MAX_DEVICES)
        return;

    uint32_t id = pci_read_config32(bus, slot, func, 0x00);
    uint16_t vendor = (uint16_t)(id & 0xFFFFU);
    if (vendor == 0xFFFFU)
        return;

    pci_device_t *d = &devices[ndevices++];
    memset(d, 0, sizeof(*d));

    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor_id = vendor;
    d->device_id = (uint16_t)(id >> 16);

    uint32_t cmd = pci_read_config32(bus, slot, func, 0x04);
    d->command = (uint16_t)(cmd & 0xFFFFU);
    d->status = (uint16_t)(cmd >> 16);

    uint32_t cls = pci_read_config32(bus, slot, func, 0x08);
    d->revision = (uint8_t)(cls & 0xFFU);
    d->prog_if = (uint8_t)((cls >> 8) & 0xFFU);
    d->subclass = (uint8_t)((cls >> 16) & 0xFFU);
    d->class_code = (uint8_t)((cls >> 24) & 0xFFU);
    d->header_type = pci_read_config8(bus, slot, func, 0x0E);

    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read_config32(bus, slot, func, (uint8_t)(0x10 + i * 4));

    uint32_t intr = pci_read_config32(bus, slot, func, 0x3C);
    d->irq_line = (uint8_t)(intr & 0xFFU);
    d->irq_pin = (uint8_t)((intr >> 8) & 0xFFU);
}

void pci_init(void) {
    ndevices = 0;
    memset(devices, 0, sizeof(devices));

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_read_config16((uint8_t)bus, slot, 0, 0x00);
            if (vendor == 0xFFFFU)
                continue;

            uint8_t header = pci_read_config8((uint8_t)bus, slot, 0, 0x0E);
            uint8_t functions = (header & 0x80U) ? 8 : 1;
            for (uint8_t func = 0; func < functions; func++)
                pci_add_device((uint8_t)bus, slot, func);
        }
    }

    printk("[PCI] found %d device(s)\n", ndevices);
}

int pci_device_count(void) {
    return ndevices;
}

const pci_device_t *pci_get_device(int index) {
    if (index < 0 || index >= ndevices)
        return 0;
    return &devices[index];
}

const pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (int i = 0; i < ndevices; i++) {
        if (devices[i].vendor_id == vendor_id &&
            devices[i].device_id == device_id)
            return &devices[i];
    }
    return 0;
}

const pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < ndevices; i++) {
        if (devices[i].class_code == class_code &&
            devices[i].subclass == subclass)
            return &devices[i];
    }
    return 0;
}

