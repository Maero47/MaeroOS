#pragma once
#include <stdint.h>

/* USB core definitions shared by the xHCI host driver (xhci.c) and the class
 * drivers (usb_hid.c).  Written from the USB 2.0 specification (chapter 9)
 * and the HID 1.11 specification. */

/* bmRequestType */
#define USB_DIR_IN          0x80
#define USB_TYPE_STANDARD   0x00
#define USB_TYPE_CLASS      0x20
#define USB_RECIP_DEVICE    0x00
#define USB_RECIP_INTERFACE 0x01

/* bRequest */
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_SET_CONFIGURATION 9
#define HID_REQ_SET_IDLE          0x0A
#define HID_REQ_SET_PROTOCOL      0x0B

/* descriptor types */
#define USB_DT_DEVICE        1
#define USB_DT_CONFIG        2
#define USB_DT_INTERFACE     4
#define USB_DT_ENDPOINT      5
#define USB_DT_HID           0x21
#define USB_DT_REPORT        0x22

#define USB_CLASS_HID        3
#define USB_CLASS_HUB        9
#define USB_CLASS_MASS_STORAGE 8

/* xHCI PORTSC speed IDs (the default Protocol Speed ID mapping) */
#define USB_SPEED_FULL  1
#define USB_SPEED_LOW   2
#define USB_SPEED_HIGH  3
#define USB_SPEED_SUPER 4

typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} usb_device_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces, bConfigurationValue, iConfiguration;
    uint8_t  bmAttributes, bMaxPower;
} usb_config_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength, bDescriptorType;
    uint8_t bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol;
    uint8_t iInterface;
} usb_interface_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength, bDescriptorType;
    uint8_t  bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_desc_t;

/* What a class driver gets to see of a device (owned by xhci.c). */
struct usb_device;

#define USB_MAX_EPS  3       /* endpoints per device besides EP0 */
#define USB_CLS_HID  1
#define USB_CLS_MSC  2
#define USB_CLS_HUB  3
#define USB_STALL    (-2)
/* what an endpoint carries (xhci.c) */
#define EP_BULK      0
#define EP_HID       1       /* interrupt-IN HID reports */
#define EP_HUB       2       /* interrupt-IN hub status changes */

/* Physical address of a buffer in the kernel image (.data/.bss), which is
 * where every DMA buffer of the USB stack lives. */
#define usb_phys(p)  ((uint32_t)((uintptr_t)(p) - 0xC0000000U))

/* Every call below needs the controller lock, which kusbd holds while it
 * enumerates and class drivers' attach/detach run under; a process doing
 * I/O takes it with usb_lock().  It is a sleeping lock. */
void usb_lock(void);
void usb_unlock(void);
/* Ask kusbd to look around now (keyboard.c: the lock keys changed, so the
 * keyboard LEDs need updating).  Safe from any context. */
void usb_kick(void);
int  usb_device_slot(const struct usb_device *dev);

/* Control transfer on endpoint 0.  `data` is a kernel buffer of `setup->
 * wLength` bytes (IN: filled; OUT: sent).  Returns the number of bytes
 * transferred, or a negative value on error.  May sleep. */
int usb_control(struct usb_device *dev, const usb_setup_t *setup, void *data);

/* Set up endpoints (bulk or interrupt); eps[i] becomes index i below. */
int usb_configure_eps(struct usb_device *dev,
                      const usb_endpoint_desc_t *const *eps, int n);
/* Bulk transfer on endpoint index i: 0 (bytes moved in *actual), USB_STALL,
 * or -1.  `phys` must not cross a 64 KiB boundary. */
int usb_bulk(struct usb_device *dev, int i, uint32_t phys, uint32_t len,
             uint32_t *actual, uint32_t timeout_ms);
/* Reset a halted endpoint on both sides. */
int usb_clear_halt(struct usb_device *dev, int i);

/* ── mass storage (usb_msc.c) ─────────────────────────────────────────────── */

/* Disks (one per LUN) attached at once; disk `unit` is /dev/usbdisk<unit>
 * and unit <unit> in drivers/blkdev.c's table. */
#define USB_MSC_MAX_DISKS 8

/* Does this configuration have a SCSI / bulk-only interface? */
int  usb_msc_match(const uint8_t *cfg, uint32_t len);
/* Called by kusbd (lock held) after SET_CONFIGURATION. 0 = attached. */
int  usb_msc_attach(struct usb_device *dev, const uint8_t *cfg, uint32_t len);
void usb_msc_detach(struct usb_device *dev);
/* devfs: the /dev/usbdisk<unit> node while that disk is attached, else
 * NULL; by name ("usbdisk1"), or by unit for listing. */
struct vfs_node *usb_msc_node(const char *name);
struct vfs_node *usb_msc_node_at(int unit);
/* For drivers/blkdev.c: size in 512-byte sectors (saturated), and sector
 * I/O (0 or -1); they take the USB lock, so never call them with it held. */
uint32_t usb_msc_sectors(int unit);
int usb_msc_read(int unit, uint32_t lba, uint32_t count, void *buf);
int usb_msc_write(int unit, uint32_t lba, uint32_t count, const void *buf);
/* kusbd, without the USB lock: put a newly attached disk into the disk
 * table and scan its partitions. */
void usb_msc_service(void);

/* ── HID class driver (usb_hid.c) ─────────────────────────────────────────── */

/* Kinds of HID function the driver handles. */
#define HID_KIND_NONE     0
#define HID_KIND_KEYBOARD 1
#define HID_KIND_MOUSE    2   /* boot protocol, relative */
#define HID_KIND_POINTER  3   /* report protocol: parsed (tablet, mice) */
#define HID_KIND_CONSUMER 4   /* report protocol: media keys (volume ...) */

#define HID_CC_MAX 16

typedef struct {
    uint16_t offset;   /* bit offset within the report (after any report ID) */
    uint8_t  size;     /* bits; 0 = field not present */
    uint8_t  count;    /* buttons: number of 1-bit fields */
    uint8_t  is_signed;
    uint8_t  relative;
    int32_t  lmin, lmax;
} hid_field_t;

typedef struct {
    int kind;
    uint8_t  report_id;     /* 0 = reports carry no ID byte */
    hid_field_t buttons, x, y, wheel;
    /* keyboard state; repeat_key is the key held down for typematic repeat
     * (USB keyboards, unlike PS/2 ones, do not repeat by themselves) */
    uint8_t  prev[8];
    uint16_t repeat_key;
    uint32_t repeat_tick;
    /* absolute pointer state */
    int      have_abs;
    int32_t  last_x, last_y;
    /* consumer control (page 0x0C): either an array field whose values
     * index usages cc_umin..cc_umax, or cc_n one-bit variable fields with
     * their usages; and the evdev keys held down after the last report */
    hid_field_t cc;
    uint8_t  cc_array;
    uint8_t  cc_n;
    uint16_t cc_umin, cc_umax;
    uint16_t cc_usage[HID_CC_MAX];
    uint16_t cc_down[HID_CC_MAX];
    /* interface number, and the LED byte last sent (keyboards; 0xFF: none
     * yet) */
    uint8_t  ifnum;
    uint8_t  leds;
} hid_state_t;

/* Choose a HID function for an interface: fills st->kind (HID_KIND_NONE when
 * the interface is not one the driver handles).  `report_desc`/`len` is the
 * interface's report descriptor (may be NULL for boot devices). */
void hid_setup(hid_state_t *st, const usb_interface_desc_t *intf,
               const uint8_t *report_desc, uint32_t len);
/* One interrupt-IN report arrived. */
void hid_report(hid_state_t *st, const uint8_t *data, uint32_t len);
/* Called by kusbd every tick: key repeat for keyboards. */
void hid_tick(hid_state_t *st);
const char *hid_kind_name(int kind);
/* Boot-time check of the report descriptor parser and the media-key path
 * on canned descriptors and reports; 0 = passed. */
int hid_selftest(void);
