#pragma once

/* xHCI (USB 3.x host controller, PCI class 0C/03/30) driver.
 *
 * xhci_init() runs at boot after pci_init(): it finds the controller and maps
 * its registers.  xhci_start_thread() runs once kernel threads can be made:
 * the "kusbd" thread resets the controller, enumerates the root-hub ports and
 * then polls the event ring, handing HID reports to usb_hid.c, which feeds
 * the same /dev/input/event0 (keys) and event1 (pointer) rings as the PS/2
 * drivers. */
void xhci_init(void);
void xhci_start_thread(void);
