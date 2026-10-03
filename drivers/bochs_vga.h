#pragma once
/* Bochs/QEMU std VGA and VirtualBox VGA mode setting (DISPI).  Called after
 * pci_init, before devfs; returns 1 when it took over the framebuffer. */
int bochs_vga_init(void);
