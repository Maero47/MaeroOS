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
#define USB_STALL    (-2)

/* Physical address of a buffer in the kernel image (.data/.bss), which is
 * where every DMA buffer of the USB stack lives. */
#define usb_phys(p)  ((uint32_t)((uintptr_t)(p) - 0xC0000000U))

/* Every call below needs the controller lock, which kusbd holds while it
 * enumerates and class drivers' attach/detach run under; a process doing
 * I/O takes it with usb_lock().  It is a sleeping lock. */
void usb_lock(void);
void usb_unlock(void);
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

/* Does this configuration have a SCSI / bulk-only interface? */
int  usb_msc_match(const uint8_t *cfg, uint32_t len);
/* Called by kusbd (lock held) after SET_CONFIGURATION. 0 = attached. */
int  usb_msc_attach(struct usb_device *dev, const uint8_t *cfg, uint32_t len);
void usb_msc_detach(struct usb_device *dev);
/* devfs: the /dev/usbdisk0 node while a disk is attached, else NULL. */
struct vfs_node *usb_msc_node(void);

/* ── HID class driver (usb_hid.c) ─────────────────────────────────────────── */

/* Kinds of HID function the driver handles. */
#define HID_KIND_NONE     0
#define HID_KIND_KEYBOARD 1
#define HID_KIND_MOUSE    2   /* boot protocol, relative */
#define HID_KIND_POINTER  3   /* report protocol: parsed (tablet, mice) */

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
    /* keyboard state */
    uint8_t  prev[8];
    /* absolute pointer state */
    int      have_abs;
    int32_t  last_x, last_y;
} hid_state_t;

/* Choose a HID function for an interface: fills st->kind (HID_KIND_NONE when
 * the interface is not one the driver handles).  `report_desc`/`len` is the
 * interface's report descriptor (may be NULL for boot devices). */
void hid_setup(hid_state_t *st, const usb_interface_desc_t *intf,
               const uint8_t *report_desc, uint32_t len);
/* One interrupt-IN report arrived. */
void hid_report(hid_state_t *st, const uint8_t *data, uint32_t len);
const char *hid_kind_name(int kind);
