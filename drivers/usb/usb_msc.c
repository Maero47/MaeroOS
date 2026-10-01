/*
 * USB mass storage: bulk-only transport (BOT) with the SCSI transparent
 * command set, exposed as the raw block device /dev/usbdisk0.
 *
 * Written from the USB Mass Storage Class Bulk-Only Transport specification
 * 1.0 (CBW/CSW, section 5; reset recovery, 5.3.4) and SCSI SPC/SBC (INQUIRY,
 * TEST UNIT READY, REQUEST SENSE, READ CAPACITY(10), READ(10), WRITE(10)).
 *
 * One device at a time (the first one attached), LUN 0, block sizes up to
 * 4 KiB.  Transfers go through one 32 KiB bounce buffer in .bss; callers
 * hold the USB lock for the whole command, so the buffer needs no more.
 *
 * Besides the raw node, an attached disk joins drivers/blkdev.c's table as
 * the next SCSI disk name after the AHCI ones ("sdb" next to one AHCI
 * disk), and its partitions are scanned, so it can be mounted
 * (`mount -t vfat /dev/sdb1 /mnt`).  That registration does disk I/O, which
 * needs the USB lock, so kusbd does it after attach returns
 * (usb_msc_service()); unplugging removes the disk and its partitions from
 * the table at once.
 */
#include "usb.h"
#include "../../fs/vfs.h"
#include "../../kernel/printk.h"
#include "../blkdev.h"
#include "../blkpart.h"
#include "../../lib/string.h"
#include <stdint.h>

#define MSC_SUBCLASS_SCSI  0x06
#define MSC_PROTO_BOT      0x50
#define MSC_REQ_RESET      0xFF
#define MSC_REQ_MAX_LUN    0xFE

#define CBW_SIGNATURE      0x43425355U
#define CSW_SIGNATURE      0x53425355U

#define BOUNCE_SIZE        32768U

typedef struct __attribute__((packed)) {
    uint32_t sig, tag, data_len;
    uint8_t  flags, lun, cb_len;
    uint8_t  cb[16];
} cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t sig, tag, residue;
    uint8_t  status;
} csw_t;

/* Page alignment only: an alignment above 4 KiB raises the kernel's data
 * segment alignment, which QEMU's multiboot loader then gets wrong (.data
 * arrives zeroed).  usb_bulk() splits the buffer at a 64 KiB boundary. */
static uint8_t bounce[BOUNCE_SIZE] __attribute__((aligned(4096)));
static cbw_t cbw_buf __attribute__((aligned(64)));
static csw_t csw_buf __attribute__((aligned(64)));

static struct usb_device *msc_dev;   /* NULL = no disk */
static int ep_in, ep_out;            /* endpoint indices for usb_bulk() */
static uint8_t ifnum;
static uint32_t tag_seq;
static uint32_t block_size, block_count;
static vfs_node_t disk_node;
static volatile int blk_pending;     /* attached, not yet in the disk table */
static int blk_index = -1;           /* index in blkdev's table, -1 = none */

/* Find the first SCSI/BOT interface and its two bulk endpoints. */
static int find_bot(const uint8_t *cfg, uint32_t len, uint8_t *ifn,
                    const usb_endpoint_desc_t **in,
                    const usb_endpoint_desc_t **out) {
    int inside = 0;
    *in = *out = 0;
    for (uint32_t off = 0; off + 2 <= len;) {
        uint8_t blen = cfg[off], type = cfg[off + 1];
        if (blen < 2 || off + blen > len) break;
        if (type == USB_DT_INTERFACE && blen >= 9) {
            if (*in && *out) return 0;
            const usb_interface_desc_t *i =
                (const usb_interface_desc_t *)(cfg + off);
            inside = i->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                     i->bInterfaceSubClass == MSC_SUBCLASS_SCSI &&
                     i->bInterfaceProtocol == MSC_PROTO_BOT &&
                     i->bAlternateSetting == 0;
            *in = *out = 0;
            if (inside) *ifn = i->bInterfaceNumber;
        } else if (type == USB_DT_ENDPOINT && blen >= 7 && inside) {
            const usb_endpoint_desc_t *e =
                (const usb_endpoint_desc_t *)(cfg + off);
            if ((e->bmAttributes & 3) == 2) {
                if (e->bEndpointAddress & 0x80) { if (!*in) *in = e; }
                else if (!*out) *out = e;
            }
        }
        off += blen;
    }
    return (*in && *out) ? 0 : -1;
}

int usb_msc_match(const uint8_t *cfg, uint32_t len) {
    uint8_t ifn;
    const usb_endpoint_desc_t *in, *out;
    return find_bot(cfg, len, &ifn, &in, &out) == 0;
}

/* Bulk-Only Mass Storage Reset, then clear both halts (BOT 5.3.4). */
static void reset_recovery(struct usb_device *d) {
    usb_setup_t s = { USB_TYPE_CLASS | USB_RECIP_INTERFACE, MSC_REQ_RESET,
                      0, ifnum, 0 };
    usb_control(d, &s, 0);
    usb_clear_halt(d, ep_in);
    usb_clear_halt(d, ep_out);
}

/* One SCSI command.  Data (`len` bytes, at most BOUNCE_SIZE) moves through
 * `bounce`.  Returns the CSW status (0 passed, 1 failed) or -1 on a
 * transport error (after reset recovery). */
static int scsi(struct usb_device *d, const uint8_t *cb, uint8_t cb_len,
                int in, uint32_t len) {
    memset(&cbw_buf, 0, sizeof(cbw_buf));
    cbw_buf.sig = CBW_SIGNATURE;
    cbw_buf.tag = ++tag_seq;
    cbw_buf.data_len = len;
    cbw_buf.flags = in ? 0x80 : 0x00;
    cbw_buf.lun = 0;
    cbw_buf.cb_len = cb_len;
    memcpy(cbw_buf.cb, cb, cb_len);

    uint32_t got = 0;
    if (usb_bulk(d, ep_out, usb_phys(&cbw_buf), 31, &got, 2000) != 0 ||
        got != 31) {
        reset_recovery(d);
        return -1;
    }
    if (len) {
        int r = usb_bulk(d, in ? ep_in : ep_out, usb_phys(bounce), len, &got,
                         10000);
        if (r == USB_STALL) {
            usb_clear_halt(d, in ? ep_in : ep_out);   /* then read the CSW */
        } else if (r != 0) {
            reset_recovery(d);
            return -1;
        }
    }
    int r = usb_bulk(d, ep_in, usb_phys(&csw_buf), 13, &got, 2000);
    if (r == USB_STALL) {
        usb_clear_halt(d, ep_in);
        r = usb_bulk(d, ep_in, usb_phys(&csw_buf), 13, &got, 2000);
    }
    if (r != 0 || got != 13 || csw_buf.sig != CSW_SIGNATURE ||
        csw_buf.tag != cbw_buf.tag || csw_buf.status > 1) {
        reset_recovery(d);
        return -1;
    }
    return csw_buf.status;
}

static void request_sense(struct usb_device *d) {
    uint8_t cb[6] = { 0x03, 0, 0, 0, 18, 0 };
    if (scsi(d, cb, 6, 1, 18) == 0)
        printk("[USB-MSC] sense key %x asc %02x ascq %02x\n",
               bounce[2] & 0xF, bounce[12], bounce[13]);
}

/* READ(10) / WRITE(10) of `count` blocks through the bounce buffer. */
static int rw10(struct usb_device *d, int write, uint32_t lba, uint32_t count) {
    uint8_t cb[10] = { write ? 0x2A : 0x28, 0,
                       (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                       (uint8_t)(lba >> 8), (uint8_t)lba, 0,
                       (uint8_t)(count >> 8), (uint8_t)count, 0 };
    int r = scsi(d, cb, 10, !write, count * block_size);
    if (r == 1) request_sense(d);
    return r == 0 ? 0 : -1;
}

/* ── /dev/usbdisk0 ───────────────────────────────────────────────────────── */

/* Read or write `len` bytes at byte offset `off`; partial blocks at either
 * end are read, patched and written back. */
static uint32_t disk_io(uint64_t off, uint32_t len, uint8_t *buf,
                        const uint8_t *wbuf) {
    uint32_t done = 0;
    usb_lock();
    if (!msc_dev || !block_size) {
        usb_unlock();
        return 0;
    }
    uint64_t total = (uint64_t)block_count * block_size;
    if (off >= total) {
        usb_unlock();
        return 0;
    }
    if (off + len > total) len = (uint32_t)(total - off);
    uint32_t per = BOUNCE_SIZE / block_size;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t lba = (uint32_t)(pos / block_size);
        uint32_t skip = (uint32_t)(pos % block_size);
        uint32_t want = len - done;
        uint32_t nblk = (skip + want + block_size - 1) / block_size;
        if (nblk > per) nblk = per;
        uint32_t span = nblk * block_size - skip;
        if (span > want) span = want;
        if (wbuf) {
            /* Whole blocks are overwritten; a partial one is read first. */
            int partial = skip || (span % block_size);
            if (partial && rw10(msc_dev, 0, lba, nblk) != 0) break;
            memcpy(bounce + skip, wbuf + done, span);
            if (rw10(msc_dev, 1, lba, nblk) != 0) break;
        } else {
            if (rw10(msc_dev, 0, lba, nblk) != 0) break;
            memcpy(buf + done, bounce + skip, span);
        }
        done += span;
    }
    usb_unlock();
    return done;
}

static uint32_t disk_read(vfs_node_t *n, uint32_t off, uint32_t len,
                          uint8_t *buf) {
    (void)n;
    return disk_io(off, len, buf, 0);
}

static uint32_t disk_write(vfs_node_t *n, uint32_t off, uint32_t len,
                           const uint8_t *buf) {
    (void)n;
    /* The same stick as /dev/sdX: never written behind a mounted
     * filesystem's back (vfs_write_user passes a negative errno through). */
    if (blk_index >= 0 && blkpart_disk_in_use(blk_index))
        return (uint32_t)-16;                              /* -EBUSY */
    return disk_io(off, len, 0, buf);
}

static int disk_ready(vfs_node_t *n) {
    (void)n;
    return 1;
}

vfs_node_t *usb_msc_node(void) {
    return msc_dev ? &disk_node : 0;
}

/* ── the disk table (drivers/blkdev.c) ───────────────────────────────────── */

uint32_t usb_msc_sectors(void) {
    uint64_t n = (uint64_t)block_count * block_size / 512u;
    return n > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)n;
}

int usb_msc_read(uint32_t lba, uint32_t count, void *buf) {
    uint32_t len = count * 512u;
    return disk_io((uint64_t)lba * 512u, len, (uint8_t *)buf, 0) == len ? 0 : -1;
}

int usb_msc_write(uint32_t lba, uint32_t count, const void *buf) {
    uint32_t len = count * 512u;
    return disk_io((uint64_t)lba * 512u, len, 0, (const uint8_t *)buf) == len ? 0 : -1;
}

void usb_msc_service(void) {
    if (!blk_pending) return;
    blk_pending = 0;
    if (!msc_dev) return;
    int disk = blk_usb_attach(0);
    if (disk < 0) return;
    blk_index = disk;
    printk("[USB-MSC] /dev/usbdisk0 is /dev/%s\n", blk_disk_devname(disk));
    blkpart_scan(disk);
}

/* ── attach / detach (kusbd, lock held) ──────────────────────────────────── */

int usb_msc_attach(struct usb_device *d, const uint8_t *cfg, uint32_t len) {
    const usb_endpoint_desc_t *in, *out;
    if (msc_dev) {
        printk("[USB-MSC] only one disk is supported; slot %d ignored\n",
               usb_device_slot(d));
        return -1;
    }
    if (find_bot(cfg, len, &ifnum, &in, &out) != 0) return -1;
    const usb_endpoint_desc_t *eps[2] = { in, out };
    if (usb_configure_eps(d, eps, 2) != 0) return -1;
    ep_in = 0;
    ep_out = 1;

    /* Devices with one LUN may stall GET MAX LUN (BOT 3.2); either way we
     * use LUN 0. */
    uint8_t maxlun = 0;
    usb_setup_t s = { USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                      MSC_REQ_MAX_LUN, 0, ifnum, 1 };
    usb_control(d, &s, &maxlun);

    uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (scsi(d, inquiry, 6, 1, 36) != 0) {
        printk("[USB-MSC] slot %d: INQUIRY failed\n", usb_device_slot(d));
        return -1;
    }
    char vendor[9], product[17];
    memcpy(vendor, bounce + 8, 8);
    vendor[8] = 0;
    memcpy(product, bounce + 16, 16);
    product[16] = 0;
    if ((bounce[0] & 0x1F) != 0) {
        printk("[USB-MSC] slot %d: peripheral type %u is not a disk\n",
               usb_device_slot(d), bounce[0] & 0x1F);
        return -1;
    }

    /* The medium may need a moment (and a sense read) after power-on. */
    int ready = 0;
    for (int tries = 0; tries < 5 && !ready; tries++) {
        uint8_t tur[6] = { 0 };
        int r = scsi(d, tur, 6, 0, 0);
        if (r == 0) ready = 1;
        else if (r == 1) request_sense(d);
    }
    uint8_t cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!ready || scsi(d, cap, 10, 1, 8) != 0) {
        printk("[USB-MSC] slot %d: no medium\n", usb_device_slot(d));
        return -1;
    }
    uint32_t last = ((uint32_t)bounce[0] << 24) | ((uint32_t)bounce[1] << 16) |
                    ((uint32_t)bounce[2] << 8) | bounce[3];
    uint32_t bs = ((uint32_t)bounce[4] << 24) | ((uint32_t)bounce[5] << 16) |
                  ((uint32_t)bounce[6] << 8) | bounce[7];
    if (bs < 512 || bs > 4096 || (bs & (bs - 1)) || last == 0xFFFFFFFFU) {
        printk("[USB-MSC] slot %d: unsupported geometry (%u-byte blocks)\n",
               usb_device_slot(d), (unsigned)bs);
        return -1;
    }
    block_size = bs;
    block_count = last + 1;

    memset(&disk_node, 0, sizeof(disk_node));
    strncpy(disk_node.name, "usbdisk0", 255);
    disk_node.flags = VFS_FLAG_BLKDEV;
    disk_node.inode = 0x5B0;
    uint64_t bytes = (uint64_t)block_count * block_size;
    disk_node.size = bytes > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)bytes;
    disk_node.mask = 0660;
    disk_node.read_fn = disk_read;
    disk_node.write_fn = disk_write;
    disk_node.read_ready_fn = disk_ready;
    disk_node.write_ready_fn = disk_ready;
    msc_dev = d;
    blk_pending = 1;
    printk("[USB-MSC] /dev/usbdisk0: %s %s, %u blocks of %u bytes (%u MiB)\n",
           vendor, product, (unsigned)block_count, (unsigned)block_size,
           (unsigned)(bytes >> 20));
    return 0;
}

void usb_msc_detach(struct usb_device *d) {
    if (d != msc_dev) return;
    msc_dev = 0;
    block_size = block_count = 0;
    blk_pending = 0;
    if (blk_index >= 0) {
        blkpart_drop(blk_index);
        blk_usb_detach(blk_index);
        blk_index = -1;
    }
    printk("[USB-MSC] /dev/usbdisk0 removed\n");
}
