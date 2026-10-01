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


/* Config offset of capability `id`, or 0. */
uint8_t pci_find_cap(const pci_device_t *d, uint8_t id);
/* Enable single-message MSI to Local APIC `apic_id` on `vector` and turn
 * INTx off; -1 if the function has no MSI capability. */
int pci_enable_msi(const pci_device_t *d, uint8_t vector, uint8_t apic_id);
/* MSI-X: capability offset (0 = none), the vector table's BAR and offset;
 * and turning it on once the caller has programmed the table entries
 * (address 0xFEE00000 | apic << 12, data = vector, control 0). */
uint8_t pci_msix_table(const pci_device_t *d, uint8_t *bir, uint32_t *offset);
void    pci_msix_enable(const pci_device_t *d, uint8_t cap);
