/*
 * USB mass storage: bulk-only transport (BOT) with the SCSI transparent
 * command set, every LUN of every stick exposed as a raw block device
 * /dev/usbdisk<N>.
 *
 * Written from the USB Mass Storage Class Bulk-Only Transport specification
 * 1.0 (CBW/CSW, section 5; reset recovery, 5.3.4) and SCSI SPC/SBC (INQUIRY,
 * TEST UNIT READY, REQUEST SENSE, READ CAPACITY(10), READ(10), WRITE(10)).
 *
 * Up to USB_MSC_MAX_DISKS disks at once (a disk is one LUN of a device;
 * GET MAX LUN says how many a device has), block sizes up to 4 KiB.  A disk
 * takes the lowest free number, so a stick unplugged and plugged back in
 * gets its old number and name back.  Transfers go through one 32 KiB
 * bounce buffer in .bss; callers hold the USB lock for the whole command, so
 * the buffer needs no more.
 *
 * Besides the raw node, an attached disk joins drivers/blkdev.c's table as
 * unit N, named after the AHCI disks ("sdb" for usbdisk0 next to one AHCI
 * disk, "sdc" for usbdisk1), and its partitions are scanned, so it can be
 * mounted
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

#define MAX_LUNS 4

typedef struct {
    struct usb_device *dev;          /* NULL = free */
    uint8_t lun;
    uint8_t ifnum;
    uint32_t block_size, block_count;
    vfs_node_t node;
    char name[12];
    volatile int blk_pending;        /* attached, not yet in the disk table */
    int blk_index;                   /* index in blkdev's table, -1 = none */
} msc_disk_t;

static msc_disk_t disks[USB_MSC_MAX_DISKS];
/* Every device uses endpoint indices 0 (bulk IN) and 1 (bulk OUT). */
#define EP_IN  0
#define EP_OUT 1
static uint32_t tag_seq;

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
static void reset_recovery(struct usb_device *d, uint8_t ifnum) {
    usb_setup_t s = { USB_TYPE_CLASS | USB_RECIP_INTERFACE, MSC_REQ_RESET,
                      0, ifnum, 0 };
    if (usb_control(d, &s, 0) < 0) return;    /* gone, most likely */
    usb_clear_halt(d, EP_IN);
    usb_clear_halt(d, EP_OUT);
}

/* One SCSI command to `lun` of `d`.  Data (`len` bytes, at most
 * BOUNCE_SIZE) moves through `bounce`.  Returns the CSW status (0 passed,
 * 1 failed) or -1 on a transport error (after reset recovery). */
static int scsi(struct usb_device *d, uint8_t ifnum, uint8_t lun,
                const uint8_t *cb, uint8_t cb_len, int in, uint32_t len) {
    memset(&cbw_buf, 0, sizeof(cbw_buf));
    cbw_buf.sig = CBW_SIGNATURE;
    cbw_buf.tag = ++tag_seq;
    cbw_buf.data_len = len;
    cbw_buf.flags = in ? 0x80 : 0x00;
    cbw_buf.lun = lun;
    cbw_buf.cb_len = cb_len;
    memcpy(cbw_buf.cb, cb, cb_len);

    uint32_t got = 0;
    if (usb_bulk(d, EP_OUT, usb_phys(&cbw_buf), 31, &got, 2000) != 0 ||
        got != 31) {
        reset_recovery(d, ifnum);
        return -1;
    }
    if (len) {
        int r = usb_bulk(d, in ? EP_IN : EP_OUT, usb_phys(bounce), len, &got,
                         10000);
        if (r == USB_STALL) {
            usb_clear_halt(d, in ? EP_IN : EP_OUT);   /* then read the CSW */
        } else if (r != 0) {
            reset_recovery(d, ifnum);
            return -1;
        }
    }
    int r = usb_bulk(d, EP_IN, usb_phys(&csw_buf), 13, &got, 2000);
    if (r == USB_STALL) {
        usb_clear_halt(d, EP_IN);
        r = usb_bulk(d, EP_IN, usb_phys(&csw_buf), 13, &got, 2000);
    }
    if (r != 0 || got != 13 || csw_buf.sig != CSW_SIGNATURE ||
        csw_buf.tag != cbw_buf.tag || csw_buf.status > 1) {
        reset_recovery(d, ifnum);
        return -1;
    }
    return csw_buf.status;
}

static int disk_scsi(msc_disk_t *k, const uint8_t *cb, uint8_t cb_len,
                     int in, uint32_t len) {
    return scsi(k->dev, k->ifnum, k->lun, cb, cb_len, in, len);
}

static void request_sense(msc_disk_t *k) {
    uint8_t cb[6] = { 0x03, 0, 0, 0, 18, 0 };
    if (disk_scsi(k, cb, 6, 1, 18) == 0)
        printk("[USB-MSC] %s: sense key %x asc %02x ascq %02x\n", k->name,
               bounce[2] & 0xF, bounce[12], bounce[13]);
}

/* READ(10) / WRITE(10) of `count` blocks through the bounce buffer. */
static int rw10(msc_disk_t *k, int write, uint32_t lba, uint32_t count) {
    uint8_t cb[10] = { write ? 0x2A : 0x28, 0,
                       (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                       (uint8_t)(lba >> 8), (uint8_t)lba, 0,
                       (uint8_t)(count >> 8), (uint8_t)count, 0 };
    int r = disk_scsi(k, cb, 10, !write, count * k->block_size);
    if (r == 1) request_sense(k);
    return r == 0 ? 0 : -1;
}

/* ── /dev/usbdisk<N> ─────────────────────────────────────────────────────── */

static msc_disk_t *unit_disk(int unit) {
    return unit >= 0 && unit < USB_MSC_MAX_DISKS ? &disks[unit] : 0;
}

/* Read or write `len` bytes at byte offset `off` of disk `unit`; partial
 * blocks at either end are read, patched and written back. */
static uint32_t disk_io(int unit, uint64_t off, uint32_t len, uint8_t *buf,
                        const uint8_t *wbuf) {
    uint32_t done = 0;
    msc_disk_t *k = unit_disk(unit);
    if (!k) return 0;
    usb_lock();
    if (!k->dev || !k->block_size) {
        usb_unlock();
        return 0;
    }
    uint32_t bs = k->block_size;
    uint64_t total = (uint64_t)k->block_count * bs;
    if (off >= total) {
        usb_unlock();
        return 0;
    }
    if (off + len > total) len = (uint32_t)(total - off);
    uint32_t per = BOUNCE_SIZE / bs;
    while (done < len && k->dev) {
        uint64_t pos = off + done;
        uint32_t lba = (uint32_t)(pos / bs);
        uint32_t skip = (uint32_t)(pos % bs);
        uint32_t want = len - done;
        uint32_t nblk = (skip + want + bs - 1) / bs;
        if (nblk > per) nblk = per;
        uint32_t span = nblk * bs - skip;
        if (span > want) span = want;
        if (wbuf) {
            /* Whole blocks are overwritten; a partial one is read first. */
            int partial = skip || (span % bs);
            if (partial && rw10(k, 0, lba, nblk) != 0) break;
            memcpy(bounce + skip, wbuf + done, span);
            if (rw10(k, 1, lba, nblk) != 0) break;
        } else {
            if (rw10(k, 0, lba, nblk) != 0) break;
            memcpy(buf + done, bounce + skip, span);
        }
        done += span;
    }
    usb_unlock();
    return done;
}

/* The node's inode number carries its unit. */
#define NODE_INODE_BASE 0x5B0U
static int node_unit(const vfs_node_t *n) {
    return (int)(n->inode - NODE_INODE_BASE);
}

static uint32_t disk_read(vfs_node_t *n, uint64_t off, uint32_t len,
                          uint8_t *buf) {
    return disk_io(node_unit(n), off, len, buf, 0);
}

static uint32_t disk_write(vfs_node_t *n, uint64_t off, uint32_t len,
                           const uint8_t *buf) {
    msc_disk_t *k = unit_disk(node_unit(n));
    /* The same stick as /dev/sdX: never written behind a mounted
     * filesystem's back (vfs_write_user passes a negative errno through). */
    if (k && k->blk_index >= 0 && blkpart_disk_in_use(k->blk_index))
        return (uint32_t)-16;                              /* -EBUSY */
    return disk_io(node_unit(n), off, len, 0, buf);
}

static int disk_ready(vfs_node_t *n) {
    (void)n;
    return 1;
}

/* A disk being attached (dev set for its SCSI commands) is not listed until
 * its node is filled in, which is the last step. */
static int disk_listed(const msc_disk_t *k) {
    return k->dev && k->node.read_fn;
}

vfs_node_t *usb_msc_node_at(int unit) {
    msc_disk_t *k = unit_disk(unit);
    return k && disk_listed(k) ? &k->node : 0;
}

vfs_node_t *usb_msc_node(const char *name) {
    for (int i = 0; i < USB_MSC_MAX_DISKS; i++)
        if (disk_listed(&disks[i]) && strcmp(disks[i].name, name) == 0)
            return &disks[i].node;
    return 0;
}

/* ── the disk table (drivers/blkdev.c) ───────────────────────────────────── */

uint32_t usb_msc_sectors(int unit) {
    msc_disk_t *k = unit_disk(unit);
    if (!k || !k->dev) return 0;
    uint64_t n = (uint64_t)k->block_count * k->block_size / 512u;
    return n > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)n;
}

int usb_msc_read(int unit, uint32_t lba, uint32_t count, void *buf) {
    uint32_t len = count * 512u;
    return disk_io(unit, (uint64_t)lba * 512u, len, (uint8_t *)buf, 0) == len
           ? 0 : -1;
}

int usb_msc_write(int unit, uint32_t lba, uint32_t count, const void *buf) {
    uint32_t len = count * 512u;
    return disk_io(unit, (uint64_t)lba * 512u, len, 0, (const uint8_t *)buf) ==
           len ? 0 : -1;
}

void usb_msc_service(void) {
    for (int i = 0; i < USB_MSC_MAX_DISKS; i++) {
        msc_disk_t *k = &disks[i];
        if (!k->blk_pending) continue;
        k->blk_pending = 0;
        if (!k->dev) continue;
        int disk = blk_usb_attach(i);
        if (disk < 0) continue;
        k->blk_index = disk;
        printk("[USB-MSC] /dev/%s is /dev/%s\n", k->name,
               blk_disk_devname(disk));
        blkpart_scan(disk);
    }
}

/* ── attach / detach (kusbd, lock held) ──────────────────────────────────── */

/* Bring up LUN `lun` of `d` as a free disk number; 0 on success. */
static int attach_lun(struct usb_device *d, uint8_t ifnum, uint8_t lun) {
    msc_disk_t *k = 0;
    int unit = 0;
    for (; unit < USB_MSC_MAX_DISKS; unit++)
        if (!disks[unit].dev && !disks[unit].blk_pending) {
            k = &disks[unit];
            break;
        }
    if (!k) {
        printk("[USB-MSC] slot %d LUN %u: too many disks, ignored\n",
               usb_device_slot(d), lun);
        return -1;
    }
    memset(k, 0, sizeof(*k));
    k->blk_index = -1;
    k->ifnum = ifnum;
    k->lun = lun;
    k->name[0] = 'u'; k->name[1] = 's'; k->name[2] = 'b';
    k->name[3] = 'd'; k->name[4] = 'i'; k->name[5] = 's'; k->name[6] = 'k';
    k->name[7] = (char)('0' + unit);
    k->name[8] = 0;
    k->dev = d;                    /* for disk_scsi(); undone on failure */

    uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (disk_scsi(k, inquiry, 6, 1, 36) != 0) {
        printk("[USB-MSC] slot %d LUN %u: INQUIRY failed\n",
               usb_device_slot(d), lun);
        k->dev = 0;
        return -1;
    }
    char vendor[9], product[17];
    memcpy(vendor, bounce + 8, 8);
    vendor[8] = 0;
    memcpy(product, bounce + 16, 16);
    product[16] = 0;
    if ((bounce[0] & 0x1F) != 0) {
        printk("[USB-MSC] slot %d LUN %u: peripheral type %u is not a disk\n",
               usb_device_slot(d), lun, bounce[0] & 0x1F);
        k->dev = 0;
        return -1;
    }

    /* The medium may need a moment (and a sense read) after power-on. */
    int ready = 0;
    for (int tries = 0; tries < 5 && !ready; tries++) {
        uint8_t tur[6] = { 0 };
        int r = disk_scsi(k, tur, 6, 0, 0);
        if (r == 0) ready = 1;
        else if (r == 1) request_sense(k);
    }
    uint8_t cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!ready || disk_scsi(k, cap, 10, 1, 8) != 0) {
        printk("[USB-MSC] slot %d LUN %u: no medium\n", usb_device_slot(d),
               lun);
        k->dev = 0;
        return -1;
    }
    uint32_t last = ((uint32_t)bounce[0] << 24) | ((uint32_t)bounce[1] << 16) |
                    ((uint32_t)bounce[2] << 8) | bounce[3];
    uint32_t bs = ((uint32_t)bounce[4] << 24) | ((uint32_t)bounce[5] << 16) |
                  ((uint32_t)bounce[6] << 8) | bounce[7];
    if (bs < 512 || bs > 4096 || (bs & (bs - 1)) || last == 0xFFFFFFFFU) {
        printk("[USB-MSC] slot %d LUN %u: unsupported geometry (%u-byte "
               "blocks)\n", usb_device_slot(d), lun, (unsigned)bs);
        k->dev = 0;
        return -1;
    }
    k->block_size = bs;
    k->block_count = last + 1;

    vfs_node_t *n = &k->node;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, k->name, 255);
    n->flags = VFS_FLAG_BLKDEV;
    n->inode = NODE_INODE_BASE + (uint32_t)unit;
    uint64_t bytes = (uint64_t)k->block_count * bs;
    n->size = bytes;
    n->mask = 0660;
    n->gid = DEV_GID_DISK;
    n->read_fn = disk_read;
    n->write_fn = disk_write;
    n->read_ready_fn = disk_ready;
    n->write_ready_fn = disk_ready;
    k->blk_pending = 1;
    printk("[USB-MSC] /dev/%s: %s %s, %u blocks of %u bytes (%u MiB), "
           "slot %d LUN %u\n", k->name, vendor, product,
           (unsigned)k->block_count, (unsigned)bs, (unsigned)(bytes >> 20),
           usb_device_slot(d), lun);
    return 0;
}

int usb_msc_attach(struct usb_device *d, const uint8_t *cfg, uint32_t len) {
    const usb_endpoint_desc_t *in, *out;
    uint8_t ifnum;
    if (find_bot(cfg, len, &ifnum, &in, &out) != 0) return -1;
    const usb_endpoint_desc_t *eps[2] = { in, out };
    if (usb_configure_eps(d, eps, 2) != 0) return -1;

    /* Devices with one LUN may stall GET MAX LUN (BOT 3.2): LUN 0 only. */
    uint8_t maxlun = 0;
    usb_setup_t s = { USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                      MSC_REQ_MAX_LUN, 0, ifnum, 1 };
    if (usb_control(d, &s, &maxlun) != 1 || maxlun > 15) maxlun = 0;
    if (maxlun >= MAX_LUNS) maxlun = MAX_LUNS - 1;

    int attached = 0;
    for (uint8_t lun = 0; lun <= maxlun; lun++)
        if (attach_lun(d, ifnum, lun) == 0) attached++;
    return attached ? 0 : -1;
}

void usb_msc_detach(struct usb_device *d) {
    for (int i = 0; i < USB_MSC_MAX_DISKS; i++) {
        msc_disk_t *k = &disks[i];
        if (k->dev != d) continue;
        k->dev = 0;
        k->block_size = k->block_count = 0;
        k->blk_pending = 0;
        if (k->blk_index >= 0) {
            blkpart_drop(k->blk_index);
            blk_usb_detach(k->blk_index);
            k->blk_index = -1;
        }
        printk("[USB-MSC] /dev/%s removed\n", k->name);
    }
}
