#pragma once
/* virtio-gpu 2D (virtio-gpu-pci, virtio-vga): called after pci_init, before
 * devfs; returns 1 when it took over the framebuffer. */
int virtio_gpu_init(void);
