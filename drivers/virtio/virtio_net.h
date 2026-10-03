#pragma once

#include <stdint.h>

/* Probe and bring up the first virtio network device (QEMU's
 * "-device virtio-net-pci", "-nic model=virtio-net-pci", most cloud VMs);
 * prints "[VNET] not present" and does nothing when there is none.
 * Registers it as the next free ethN after the NICs probed before it. */
void virtio_net_init(void);
int virtio_net_present(void);
