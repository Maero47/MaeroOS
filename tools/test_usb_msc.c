/*
 * Host test for the USB mass-storage bulk-only transport (drivers/usb/
 * usb_msc.c); run by tools/test_usb_msc.py.
 *
 * The driver file is compiled in with the xHCI calls replaced by a simulated
 * stick: CBW in, data phase, CSW out (BOT 1.0, section 5).  The stick can
 * end its data phase early, report a residue, or both, and lie about either.
 * A read must never hand out bytes the stick did not send: the shared bounce
 * buffer still holds the previous command's data (another disk's blocks), so
 * a short READ(10) that "passed" must fail, not succeed with stale data.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "usb/usb.h"

/* Bus addresses are cookies for the three DMA buffers the driver uses. */
static void *dma_ptr[8];
static int dma_n;
static uint32_t test_phys(void *p) {
    for (int i = 0; i < dma_n; i++)
        if (dma_ptr[i] == p) return (uint32_t)(i + 1) << 20;
    dma_ptr[dma_n++] = p;
    return (uint32_t)dma_n << 20;
}
static uint8_t *dma_at(uint32_t phys) {
    return (uint8_t *)dma_ptr[(phys >> 20) - 1] + (phys & 0xFFFFF);
}
#undef usb_phys
#define usb_phys(p) test_phys((void *)(p))

#include "usb/usb_msc.c"

/* ── kernel stubs ────────────────────────────────────────────────────────── */
void printk(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  [k] ");
    vprintf(fmt, ap);
    va_end(ap);
}
void usb_lock(void) {}
void usb_unlock(void) {}
int usb_device_slot(const struct usb_device *dev) { (void)dev; return 1; }
int usb_control(struct usb_device *dev, const usb_setup_t *s, void *data) {
    (void)dev; (void)s; (void)data;
    return 0;
}
int usb_configure_eps(struct usb_device *dev,
                      const usb_endpoint_desc_t *const *eps, int n) {
    (void)dev; (void)eps; (void)n;
    return 0;
}
int usb_clear_halt(struct usb_device *dev, int i) { (void)dev; (void)i; return 0; }
const char *blk_disk_devname(int disk) { (void)disk; return "sdz"; }
int blk_usb_attach(int unit) { (void)unit; return -1; }
void blk_usb_detach(int disk) { (void)disk; }
void blkpart_scan(int dev) { (void)dev; }
void blkpart_drop(int dev) { (void)dev; }
int blkpart_disk_in_use(int dev) { (void)dev; return 0; }

/* ── the simulated stick ─────────────────────────────────────────────────── */
static struct sim {
    int phase;                /* 0 CBW, 1 data, 2 CSW */
    cbw_t cbw;
    uint8_t fill;             /* byte value of every sector on the medium */
    uint32_t short_by;        /* data phase ends this many bytes early */
    uint32_t residue;         /* what the CSW reports */
    uint8_t status;
} sim;

int usb_bulk(struct usb_device *dev, int i, uint32_t phys, uint32_t len,
             uint32_t *actual, uint32_t timeout_ms) {
    (void)dev; (void)timeout_ms;
    uint8_t *p = dma_at(phys);
    *actual = 0;
    if (sim.phase == 0) {
        if (i != EP_OUT || len != 31) return -1;
        memcpy(&sim.cbw, p, 31);
        sim.phase = sim.cbw.data_len ? 1 : 2;
        *actual = 31;
        return 0;
    }
    if (sim.phase == 1) {
        uint32_t n = len - (sim.short_by < len ? sim.short_by : len);
        if (sim.cbw.flags & 0x80) {
            if (i != EP_IN) return -1;
            if (sim.cbw.cb[0] == 0x28)
                memset(p, sim.fill, n);
            else if (sim.cbw.cb[0] == 0x12)
                memset(p, 0, n);
        } else if (i != EP_OUT) {
            return -1;
        }
        *actual = n;
        sim.phase = 2;
        return 0;
    }
    if (i != EP_IN || len != 13) return -1;
    csw_t c = { CSW_SIGNATURE, sim.cbw.tag, sim.residue, sim.status };
    memcpy(p, &c, 13);
    *actual = 13;
    sim.phase = 0;
    return 0;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int all(const uint8_t *b, uint32_t n, uint8_t v) {
    for (uint32_t i = 0; i < n; i++) if (b[i] != v) return 0;
    return 1;
}

static void stick(uint8_t fill, uint32_t short_by, uint32_t residue) {
    memset(&sim, 0, sizeof(sim));
    sim.fill = fill;
    sim.short_by = short_by;
    sim.residue = residue;
}

int main(void) {
    static uint8_t buf[65536];
    struct usb_device *fake = (struct usb_device *)(uintptr_t)0x1000;
    for (int u = 0; u < 2; u++) {
        disks[u].dev = fake;
        disks[u].block_size = 512;
        disks[u].block_count = 4096;
        disks[u].blk_index = -1;
        snprintf(disks[u].name, sizeof(disks[u].name), "usbdisk%d", u);
    }

    /* a good read: every byte the stick's */
    stick(0xAA, 0, 0);
    memset(buf, 0, sizeof(buf));
    CHECK(usb_msc_read(0, 8, 32, buf) == 0, "plain read failed");
    CHECK(all(buf, 32 * 512, 0xAA), "plain read data wrong");

    /* disk 1's stick ends the data phase halfway and claims all was well
     * (residue 0, status passed): the bounce buffer's tail still holds
     * disk 0's 0xAA blocks */
    stick(0x55, 8 * 512, 0);
    memset(buf, 0, sizeof(buf));
    int r = usb_msc_read(1, 0, 16, buf);
    CHECK(r != 0, "short data phase with residue 0 read as success");
    CHECK(!memchr(buf, 0xAA, 16 * 512), "disk 0's data handed out as disk 1's");

    /* full-length data phase, but the CSW says 4 blocks were not sent */
    stick(0xAA, 0, 0);
    usb_msc_read(0, 0, 16, buf);
    stick(0x55, 0, 4 * 512);
    CHECK(usb_msc_read(1, 0, 16, buf) != 0, "residue 2048 read as success");

    /* short data phase and a matching residue: still not the 16 blocks asked */
    stick(0x55, 512, 512);
    CHECK(usb_msc_read(1, 0, 16, buf) != 0, "honest short read read as success");

    /* a residue larger than the transfer is a phase error */
    stick(0x55, 0, 1u << 20);
    CHECK(usb_msc_read(1, 0, 1, buf) != 0, "residue > length read as success");

    /* writes: a short OUT data phase or a residue is a failed write */
    stick(0, 512, 0);
    memset(buf, 0x11, sizeof(buf));
    CHECK(usb_msc_write(1, 0, 4, buf) != 0, "short write reported done");
    stick(0, 0, 512);
    CHECK(usb_msc_write(1, 0, 4, buf) != 0, "write with residue reported done");
    stick(0, 0, 0);
    CHECK(usb_msc_write(1, 0, 4, buf) == 0, "plain write failed");

    /* the raw node path (disk_io) returns only what was read */
    stick(0xAA, 0, 0);
    usb_msc_read(0, 0, 64, buf);
    stick(0x55, 4096, 0);
    memset(buf, 0, sizeof(buf));
    uint32_t got = disk_io(1, 0, 16384, buf, 0);
    CHECK(got < 16384, "disk_io short read returned %u", got);
    CHECK(!memchr(buf, 0xAA, 16384), "disk_io handed out stale data");

    /* a short INQUIRY (fewer than 36 bytes; residue says so) leaves zeroes,
     * not the previous command's bytes */
    stick(0xAA, 0, 0);
    usb_msc_read(0, 0, 1, buf);
    stick(0, 20, 20);
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    CHECK(disk_scsi(&disks[1], inq, 6, 1, 36) == 0, "short INQUIRY failed");
    CHECK(all(bounce, 36, 0), "short INQUIRY left stale bytes");

    if (fails) {
        printf("test_usb_msc: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_usb_msc: ok\n");
    return 0;
}
