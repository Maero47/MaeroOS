/*
 * xHCI host controller driver: enough of it to run low/full/high/super-speed
 * devices plugged straight into the root hub, with control transfers on
 * endpoint 0 and one interrupt-IN endpoint per device (HID keyboards, mice
 * and tablets, see usb_hid.c).
 *
 * Written from the eXtensible Host Controller Interface specification (Intel,
 * revision 1.2), sections cited as "xHCI 4.x".  The OSDev wiki's xHCI page
 * was used as an overview.  No code was copied from any other implementation.
 *
 * Model:
 *   - Every structure the controller reads or writes (DCBAA, rings, contexts,
 *     transfer buffers) lives in this file's .bss, so its physical address is
 *     virt - KERNEL_VMA, like the other drivers' DMA buffers.  32-bit
 *     addresses only, which every controller accepts.
 *   - One kernel thread (kusbd) owns the controller: it resets it, enumerates
 *     the ports, and then polls the event ring every timer tick.  Nothing runs
 *     in interrupt context, so the driver needs no locking of its own; the
 *     input rings it feeds are protected by keyboard.c/mouse.c.  Polling at
 *     the 100 Hz tick matches the 10 ms report interval of most HID devices.
 *   - Ports are scanned at start and rescanned on Port Status Change events,
 *     so devices plugged in later are picked up; unplugging frees the slot.
 *
 * Class drivers: usb_hid.c (keyboards, mice, tablets) and usb_msc.c (mass
 * storage, bulk-only transport); USB 2.0 hubs are handled here (hub_*),
 * their ports polled by kusbd four times a second.  They call back in through usb.h:
 * usb_control(), usb_configure_eps() and usb_bulk().  usb_lock() serialises
 * every use of the controller between kusbd and processes doing disk I/O.
 *
 * Not done (see the report / ROADMAP): USB 3 hubs, MSI, isochronous
 * transfers, streams, 64-bit DMA addresses.
 */
#include "xhci.h"
#include "usb.h"
#include "../pci.h"
#include "../../arch/i686/cpu/pit.h"
#include "../../arch/i686/mm/paging.h"
#include "../../include/kernel/config.h"
#include "../../kernel/printk.h"
#include "../../lib/string.h"
#include "../../mm/pmm.h"
#include "../../proc/process.h"
#include "../../proc/scheduler.h"
#include <stdint.h>

/* Kernel virtual window for the controller's register BAR (free range between
 * the kernel-stack window, which ends at 0xF2000000, and the LAPIC). */
#define XHCI_MMIO_VIRT   0xF6000000U
#define XHCI_MMIO_MAX    0x00100000U    /* map at most 1 MiB of the BAR */

#define XHCI_MAX_DEVS    16
#define HUB_MAX_PORTS    15
#define HUB_MAX_DEPTH    5
#define CMD_RING_TRBS    64
#define EVT_RING_TRBS    256
#define XFER_RING_TRBS   64
#define CTRL_BUF_SIZE    1024
#define REPORT_BUF_SIZE  64
#define XFER_MAX_PIECES  4      /* TRBs per bulk transfer (64 KiB pieces) */

/* ── registers (xHCI 5) ──────────────────────────────────────────────────── */

/* capability */
#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* operational */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTSC(n)    (0x400 + 0x10 * ((n) - 1))

#define USBCMD_RS       (1U << 0)
#define USBCMD_HCRST    (1U << 1)
#define USBSTS_HCH      (1U << 0)
#define USBSTS_HSE      (1U << 2)
#define USBSTS_EINT     (1U << 3)
#define USBSTS_PCD      (1U << 4)
#define USBSTS_CNR      (1U << 11)

#define PORTSC_CCS      (1U << 0)
#define PORTSC_PED      (1U << 1)
#define PORTSC_PR       (1U << 4)
#define PORTSC_PP       (1U << 9)
#define PORTSC_SPEED(v) (((v) >> 10) & 0xF)
#define PORTSC_CHANGES  (0x7FU << 17)   /* CSC PEC WRC OCC PRC PLC CEC: RW1C */
#define PORTSC_PRC      (1U << 21)
/* Bits that keep their value when written back: PP, the indicator control and
 * the wake enables.  Everything else is RO, RW1C (writing the 1 back would
 * clear a change bit or, for PED, disable the port) or RW1S. */
#define PORTSC_KEEP     ((1U << 9) | (3U << 14) | (7U << 25))

/* runtime: interrupter 0 */
#define RT_IMAN         0x20
#define RT_IMOD         0x24
#define RT_ERSTSZ       0x28
#define RT_ERSTBA       0x30
#define RT_ERDP         0x38
#define ERDP_EHB        (1U << 3)

/* ── TRBs (xHCI 6.4) ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

#define TRB_CYCLE       (1U << 0)
#define TRB_TC          (1U << 1)       /* Link: toggle cycle */
#define TRB_ISP         (1U << 2)
#define TRB_CHAIN       (1U << 4)
#define TRB_IOC         (1U << 5)
#define TRB_IDT         (1U << 6)
#define TRB_DIR_IN      (1U << 16)
#define TRB_TYPE(t)     ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3F)
#define TRB_SLOT(s)     ((uint32_t)(s) << 24)

#define TRB_NORMAL       1
#define TRB_SETUP        2
#define TRB_DATA         3
#define TRB_STATUS       4
#define TRB_LINK         6
#define TRB_ENABLE_SLOT  9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV  11
#define TRB_CONFIG_EP    12
#define TRB_EVAL_CTX     13
#define TRB_RESET_EP     14
#define TRB_SET_TR_DEQ   16
#define TRB_EV_TRANSFER  32
#define TRB_EV_CMD       33
#define TRB_EV_PORT      34
#define TRB_EV_HC        37

#define CC_SUCCESS       1
#define CC_STALL         6
#define CC_SHORT_PACKET  13
#define CC_CONTEXT_STATE 19

typedef struct {
    xhci_trb_t *trbs;
    uint32_t phys;
    uint32_t n;          /* TRBs including the trailing Link TRB */
    uint32_t enq;
    uint32_t cycle;
} ring_t;

/* ── DMA memory ──────────────────────────────────────────────────────────── */

/* Per-device structures, one page-aligned block per device slot we use. */
typedef struct {
    uint8_t    in_ctx[4096];                 /* input context: 33 x 64 bytes  */
    uint8_t    out_ctx[2048];                /* device context: 32 x 64 bytes */
    xhci_trb_t ep0[XFER_RING_TRBS];          /* 1 KiB */
    xhci_trb_t rings[USB_MAX_EPS][XFER_RING_TRBS];   /* 1 KiB each */
    uint8_t    buf[CTRL_BUF_SIZE];           /* control transfer data */
    uint8_t    report[REPORT_BUF_SIZE];      /* interrupt-IN data */
} __attribute__((aligned(4096))) dev_dma_t;

static dev_dma_t dev_dma[XHCI_MAX_DEVS];
static uint64_t dcbaa[256] __attribute__((aligned(4096)));
static xhci_trb_t cmd_trbs[CMD_RING_TRBS] __attribute__((aligned(4096)));
static xhci_trb_t evt_trbs[EVT_RING_TRBS] __attribute__((aligned(4096)));
static struct { uint64_t base; uint32_t size, rsvd; } erst[1]
    __attribute__((aligned(64)));
#define SCRATCH_MAX 256
static uint64_t scratch_array[SCRATCH_MAX] __attribute__((aligned(4096)));

static inline uint32_t phys_of(const volatile void *p) {
    return (uint32_t)((uintptr_t)p - KERNEL_VMA);
}

/* ── device state ────────────────────────────────────────────────────────── */

struct usb_device {
    int used;
    int slot;
    int port;            /* root-hub port the device is (eventually) behind */
    int speed;
    /* topology: parent hub (NULL on a root port) and its port, route string
     * (xHCI 8.9), tier below the root hub, and the transaction translator
     * a low/full-speed device behind a high-speed hub uses */
    struct usb_device *parent;
    int parent_port;
    uint32_t route;
    int depth;
    int tt_slot, tt_port;
    /* hubs */
    int hub_ports;
    struct usb_device *child[HUB_MAX_PORTS + 1];
    uint16_t hub_bad;        /* ports whose device failed to enumerate */
    dev_dma_t *dma;
    ring_t ep0;
    struct {
        int dci;             /* 0 = unused */
        int addr;            /* bEndpointAddress */
        int mps;
        ring_t ring;
        /* the transfer usb_bulk() waits for: its TRBs (one per piece of the
         * buffer between 64 KiB boundaries) and their lengths */
        uint32_t trb[XFER_MAX_PIECES];
        uint32_t trb_len[XFER_MAX_PIECES];
        int ntrb;
        int done, code;
        uint32_t actual;
    } eps[USB_MAX_EPS];
    int cls;                 /* USB_CLS_* */
    int intr_active;         /* HID: eps[0] has a report queued */
    int reported;            /* HID: logged the first report */
    /* control transfer in flight */
    uint32_t ctl_data_trb, ctl_status_trb;
    uint32_t ctl_residual;
    int ctl_done, ctl_code;
    hid_state_t hid;
    uint16_t vid, pid;
};

static struct usb_device devs[XHCI_MAX_DEVS];
static struct usb_device *slot_dev[256];
static struct usb_device *port_dev[256];

/* ── controller state ────────────────────────────────────────────────────── */

static const pci_device_t *hc_pci;
static volatile uint8_t *cap_regs, *op_regs, *rt_regs;
static volatile uint32_t *db_regs;
static uint32_t max_slots, max_ports, ctx_size;
static ring_t cmd_ring;
static uint32_t evt_deq, evt_cycle;
static uint32_t port_pending[8];     /* ports with a status change to look at */
static int hc_running;

/* last command completion */
static uint32_t cmd_done_trb;
static int cmd_done_code, cmd_done_slot;

static int sleep_chan;

/* Register accessors are also compiler barriers: the controller reads the
 * DMA structures this file fills with plain stores, which must not be moved
 * past the register write that hands them over. */
static inline uint32_t rd32(volatile uint8_t *base, uint32_t off) {
    __asm__ volatile("" ::: "memory");
    uint32_t v = *(volatile uint32_t *)(base + off);
    __asm__ volatile("" ::: "memory");
    return v;
}

static inline void wr32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    __asm__ volatile("" ::: "memory");
    *(volatile uint32_t *)(base + off) = v;
    __asm__ volatile("" ::: "memory");
}

/* 64-bit registers written as two dwords, low first (xHCI 5.1). */
static inline void wr64(volatile uint8_t *base, uint32_t off, uint32_t lo) {
    wr32(base, off, lo);
    wr32(base, off + 4, 0);
}

static void sleep_ticks(uint32_t ticks) {
    if (!current_proc) return;
    current_proc->wake_tick = pit_ticks() + (ticks ? ticks : 1);
    sleep_on(&sleep_chan);
}

static void sleep_ms(uint32_t ms) {
    sleep_ticks((ms + 9) / 10);
}

/* Wait (sleeping) until (reg & mask) == want; 0 on success, -1 on timeout. */
static int wait_reg(volatile uint8_t *base, uint32_t off, uint32_t mask,
                    uint32_t want, uint32_t timeout_ms) {
    for (uint32_t i = 0; i < 1000; i++)
        if ((rd32(base, off) & mask) == want) return 0;
    uint32_t end = pit_ticks() + (timeout_ms + 9) / 10 + 1;
    while ((int32_t)(pit_ticks() - end) < 0) {
        if ((rd32(base, off) & mask) == want) return 0;
        sleep_ticks(1);
    }
    return (rd32(base, off) & mask) == want ? 0 : -1;
}

/* ── rings ───────────────────────────────────────────────────────────────── */

static void ring_init(ring_t *r, xhci_trb_t *trbs, uint32_t n) {
    memset(trbs, 0, n * sizeof(xhci_trb_t));
    r->trbs = trbs;
    r->phys = phys_of(trbs);
    r->n = n;
    r->enq = 0;
    r->cycle = 1;
    xhci_trb_t *link = &trbs[n - 1];
    link->param_lo = r->phys;
    link->param_hi = 0;
    link->status = 0;
    link->control = TRB_TYPE(TRB_LINK) | TRB_TC;
}

/* Queue one TRB (the cycle bit is supplied here) and return its physical
 * address.  The control dword, which carries the cycle bit that hands the TRB
 * to the controller, is written last. */
static uint32_t ring_push(ring_t *r, uint32_t p_lo, uint32_t p_hi,
                          uint32_t status, uint32_t control) {
    volatile xhci_trb_t *t = &r->trbs[r->enq];
    uint32_t phys = r->phys + r->enq * (uint32_t)sizeof(xhci_trb_t);
    t->param_lo = p_lo;
    t->param_hi = p_hi;
    t->status = status;
    __sync_synchronize();
    t->control = (control & ~TRB_CYCLE) | r->cycle;
    __sync_synchronize();
    if (++r->enq == r->n - 1) {
        /* Hand the Link TRB over with the current cycle, then wrap. */
        volatile xhci_trb_t *link = &r->trbs[r->n - 1];
        link->control = (link->control & ~TRB_CYCLE) | r->cycle;
        __sync_synchronize();
        r->enq = 0;
        r->cycle ^= 1;
    }
    return phys;
}

static inline void ring_doorbell(uint32_t slot, uint32_t target) {
    __sync_synchronize();
    db_regs[slot] = target;
}

/* ── events ──────────────────────────────────────────────────────────────── */

static void hid_transfer_done(struct usb_device *d, int code,
                              uint32_t residual);

/* A transfer event for a bulk transfer in flight.  Only the last TRB
 * interrupts on completion; a short packet in an earlier piece reports that
 * piece (ISP) and ends the transfer (xHCI 4.10.1.1). */
static void bulk_event(struct usb_device *d, int i, uint32_t trb, int code,
                       uint32_t residual) {
    int n = d->eps[i].ntrb;
    uint32_t before = 0;
    for (int k = 0; k < n; k++) {
        uint32_t len = d->eps[i].trb_len[k];
        if (d->eps[i].trb[k] == trb) {
            if (code == CC_SUCCESS && k != n - 1) return;
            d->eps[i].actual = before + len - (residual <= len ? residual : len);
            d->eps[i].code = code;
            d->eps[i].done = 1;
            return;
        }
        before += len;
    }
    if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
        d->eps[i].actual = 0;
        d->eps[i].code = code;
        d->eps[i].done = 1;
    }
}

static void handle_event(const xhci_trb_t *ev) {
    uint32_t type = TRB_GET_TYPE(ev->control);
    int code = (int)(ev->status >> 24);
    int slot = (int)(ev->control >> 24);

    switch (type) {
    case TRB_EV_CMD:
        cmd_done_trb = ev->param_lo;
        cmd_done_code = code;
        cmd_done_slot = slot;
        break;
    case TRB_EV_TRANSFER: {
        struct usb_device *d = slot_dev[slot & 0xFF];
        int ep = (int)((ev->control >> 16) & 0x1F);
        if (!d) break;
        if (ep == 1) {
            if (ev->param_lo == d->ctl_data_trb)
                d->ctl_residual = ev->status & 0xFFFFFF;
            if (ev->param_lo == d->ctl_status_trb ||
                (code != CC_SUCCESS && code != CC_SHORT_PACKET)) {
                d->ctl_code = code;
                d->ctl_done = 1;
            }
        } else {
            for (int i = 0; i < USB_MAX_EPS; i++) {
                if (d->eps[i].dci != ep) continue;
                if (d->cls == USB_CLS_HID && i == 0) {
                    hid_transfer_done(d, code, ev->status & 0xFFFFFF);
                } else {
                    bulk_event(d, i, ev->param_lo, code,
                               ev->status & 0xFFFFFF);
                }
                break;
            }
        }
        break;
    }
    case TRB_EV_PORT: {
        uint32_t port = ev->param_lo >> 24;
        if (port < 256) port_pending[port / 32] |= 1U << (port % 32);
        break;
    }
    case TRB_EV_HC:
        printk("[XHCI] host controller event, code %d\n", code);
        break;
    default:
        break;
    }
}

/* Drain the event ring.  Returns the number of events handled. */
static int process_events(void) {
    int n = 0;
    for (;;) {
        volatile xhci_trb_t *e = &evt_trbs[evt_deq];
        if ((e->control & TRB_CYCLE) != evt_cycle) break;
        __sync_synchronize();
        xhci_trb_t ev = { e->param_lo, e->param_hi, e->status, e->control };
        if (++evt_deq == EVT_RING_TRBS) {
            evt_deq = 0;
            evt_cycle ^= 1;
        }
        handle_event(&ev);
        n++;
    }
    if (n) {
        /* Tell the controller how far we got; EHB is RW1C. */
        wr32(rt_regs, RT_ERDP,
             (phys_of(&evt_trbs[evt_deq]) & ~0xFU) | ERDP_EHB);
        wr32(rt_regs, RT_ERDP + 4, 0);
        wr32(op_regs, OP_USBSTS, USBSTS_EINT);
    }
    return n;
}

/* ── commands ────────────────────────────────────────────────────────────── */

/* Run one command; returns its completion code (or -1 on timeout) and, for
 * Enable Slot, the new slot ID through *slot_out. */
static int run_command(uint32_t p_lo, uint32_t status, uint32_t control,
                       int *slot_out) {
    uint32_t trb = ring_push(&cmd_ring, p_lo, 0, status, control);
    cmd_done_trb = 0;
    ring_doorbell(0, 0);
    uint32_t end = pit_ticks() + 100 + 1;    /* 1 s */
    for (uint32_t spin = 0;; spin++) {
        process_events();
        if (cmd_done_trb == trb) {
            if (slot_out) *slot_out = cmd_done_slot;
            return cmd_done_code;
        }
        if ((int32_t)(pit_ticks() - end) >= 0) break;
        if (spin > 200) sleep_ticks(1);
    }
    printk("[XHCI] command type %u timed out (usbsts %x crcr %x evt0 %x/%x "
           "cmd %x trb %x)\n",
           (unsigned)((control >> 10) & 0x3F),
           (unsigned)rd32(op_regs, OP_USBSTS), (unsigned)rd32(op_regs, OP_CRCR),
           (unsigned)evt_trbs[0].control, (unsigned)evt_trbs[0].param_lo,
           (unsigned)cmd_trbs[0].control, (unsigned)trb);
    return -1;
}

/* ── control transfers (xHCI 4.11.2.2) ───────────────────────────────────── */

int usb_control(struct usb_device *d, const usb_setup_t *setup, void *data) {
    uint32_t len = setup->wLength;
    int in = (setup->bmRequestType & USB_DIR_IN) != 0;
    if (len > CTRL_BUF_SIZE) return -1;
    if (!in && len) memcpy(d->dma->buf, data, len);

    uint32_t s_lo, s_hi;
    memcpy(&s_lo, (const uint8_t *)setup, 4);
    memcpy(&s_hi, (const uint8_t *)setup + 4, 4);
    uint32_t trt = len ? (in ? 3U : 2U) : 0U;

    d->ctl_done = 0;
    d->ctl_code = 0;
    d->ctl_residual = 0;
    d->ctl_data_trb = 0xFFFFFFFFU;
    ring_push(&d->ep0, s_lo, s_hi, 8,
              TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (len)
        d->ctl_data_trb = ring_push(&d->ep0, phys_of(d->dma->buf), 0, len,
                                    TRB_TYPE(TRB_DATA) | TRB_ISP |
                                    (in ? TRB_DIR_IN : 0));
    /* Status stage runs the other way from the data (IN when there is none). */
    d->ctl_status_trb = ring_push(&d->ep0, 0, 0, 0,
                                  TRB_TYPE(TRB_STATUS) | TRB_IOC |
                                  ((len && in) ? 0 : TRB_DIR_IN));
    ring_doorbell((uint32_t)d->slot, 1);

    uint32_t end = pit_ticks() + 100 + 1;
    for (uint32_t spin = 0; !d->ctl_done; spin++) {
        process_events();
        if (d->ctl_done) break;
        if ((int32_t)(pit_ticks() - end) >= 0) {
            printk("[USB] slot %d: control request %02x/%02x timed out\n",
                   d->slot, setup->bmRequestType, setup->bRequest);
            return -1;
        }
        if (spin > 200) sleep_ticks(1);
    }
    if (d->ctl_code != CC_SUCCESS && d->ctl_code != CC_SHORT_PACKET) {
        printk("[USB] slot %d: control request %02x/%02x failed, code %d\n",
               d->slot, setup->bmRequestType, setup->bRequest, d->ctl_code);
        return -1;
    }
    uint32_t got = len - (d->ctl_residual <= len ? d->ctl_residual : len);
    if (in && got) memcpy(data, d->dma->buf, got);
    return (int)got;
}

static int get_descriptor(struct usb_device *d, uint8_t recip, uint8_t type,
                          uint8_t index, uint16_t windex, void *buf,
                          uint16_t len) {
    usb_setup_t s = {
        .bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | recip,
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)((type << 8) | index),
        .wIndex = windex,
        .wLength = len,
    };
    return usb_control(d, &s, buf);
}

static int control_out(struct usb_device *d, uint8_t reqtype, uint8_t req,
                       uint16_t value, uint16_t index) {
    usb_setup_t s = { reqtype, req, value, index, 0 };
    return usb_control(d, &s, 0);
}

/* ── contexts (xHCI 6.2) ─────────────────────────────────────────────────── */

static inline uint32_t *in_ctrl(struct usb_device *d) {
    return (uint32_t *)d->dma->in_ctx;
}

/* Input context entry `i`: 0 = slot context, 1 = EP0, dci = endpoint. */
static inline uint32_t *in_entry(struct usb_device *d, int i) {
    return (uint32_t *)(d->dma->in_ctx + (uint32_t)(i + 1) * ctx_size);
}

static inline uint32_t *out_entry(struct usb_device *d, int i) {
    return (uint32_t *)(d->dma->out_ctx + (uint32_t)i * ctx_size);
}

static uint32_t default_mps0(int speed) {
    switch (speed) {
    case USB_SPEED_LOW:   return 8;
    case USB_SPEED_FULL:  return 8;    /* corrected from the descriptor */
    case USB_SPEED_HIGH:  return 64;
    default:              return 512;
    }
}

static const char *speed_name(int speed) {
    switch (speed) {
    case USB_SPEED_LOW:   return "low";
    case USB_SPEED_FULL:  return "full";
    case USB_SPEED_HIGH:  return "high";
    case USB_SPEED_SUPER: return "super";
    default:              return "super+";
    }
}

/* "3" for a root port, "3.2.1" behind hubs. */
static const char *where(const struct usb_device *d) {
    static char buf[32];
    int len = 0;
    int n = d->port;
    char tmp[4];
    int t = 0;
    do { tmp[t++] = (char)('0' + n % 10); n /= 10; } while (n && t < 3);
    while (t) buf[len++] = tmp[--t];
    for (int i = 0; i < d->depth && len < 28; i++) {
        buf[len++] = '.';
        int p = (int)((d->route >> (4 * i)) & 0xF);
        if (p >= 10) buf[len++] = '1';
        buf[len++] = (char)('0' + p % 10);
    }
    buf[len] = 0;
    return buf;
}

/* Interval field of an endpoint context from bInterval (xHCI 6.2.3.6):
 * units of 2^n x 125 us. */
static uint32_t ep_interval(int speed, uint8_t binterval) {
    if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
        uint32_t frames = binterval ? binterval : 1;
        uint32_t n = 3;                       /* 8 x 125 us = 1 frame */
        while (n < 10 && (1U << (n + 1)) <= frames * 8) n++;
        return n;
    }
    uint32_t n = binterval ? (uint32_t)binterval - 1 : 0;
    return n > 15 ? 15 : n;
}

/* ── enumeration ─────────────────────────────────────────────────────────── */

static void free_device(struct usb_device *d) {
    if (d->cls == USB_CLS_HUB) {
        for (int p = 1; p <= HUB_MAX_PORTS; p++)
            if (d->child[p]) free_device(d->child[p]);
    }
    if (d->cls == USB_CLS_MSC) usb_msc_detach(d);
    d->cls = 0;
    d->intr_active = 0;
    if (d->slot) {
        run_command(0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(d->slot), 0);
        dcbaa[d->slot] = 0;
        slot_dev[d->slot] = 0;
    }
    if (d->parent) {
        if (d->parent->child[d->parent_port] == d)
            d->parent->child[d->parent_port] = 0;
    } else if (d->port > 0 && d->port < 256 && port_dev[d->port] == d) {
        port_dev[d->port] = 0;
    }
    d->used = 0;
}

/* Find the HID interface to drive, its interrupt-IN endpoint and the length
 * of its report descriptor in a configuration descriptor. */
static int pick_hid(const uint8_t *cfg, uint32_t len,
                    const usb_interface_desc_t **intf_out,
                    const usb_endpoint_desc_t **ep_out, uint16_t *rdlen_out) {
    const usb_interface_desc_t *cur = 0;
    const usb_interface_desc_t *best = 0;
    const usb_endpoint_desc_t *best_ep = 0;
    uint16_t cur_rdlen = 0, best_rdlen = 0;
    int best_score = 0;
    for (uint32_t off = 0; off + 2 <= len;) {
        uint8_t blen = cfg[off], type = cfg[off + 1];
        if (blen < 2 || off + blen > len) break;
        if (type == USB_DT_INTERFACE && blen >= 9) {
            cur = (const usb_interface_desc_t *)(cfg + off);
            cur_rdlen = 0;
            if (cur->bInterfaceClass != USB_CLASS_HID ||
                cur->bAlternateSetting != 0)
                cur = 0;
        } else if (type == USB_DT_HID && blen >= 9 && cur) {
            /* bNumDescriptors at 5, then (type, length) pairs */
            if (cfg[off + 6] == USB_DT_REPORT)
                cur_rdlen = (uint16_t)(cfg[off + 7] | (cfg[off + 8] << 8));
        } else if (type == USB_DT_ENDPOINT && blen >= 7 && cur) {
            const usb_endpoint_desc_t *ep =
                (const usb_endpoint_desc_t *)(cfg + off);
            if ((ep->bEndpointAddress & 0x80) && (ep->bmAttributes & 3) == 3) {
                /* boot keyboard > boot mouse > any other HID function */
                int score = 1;
                if (cur->bInterfaceSubClass == 1 &&
                    cur->bInterfaceProtocol == 1) score = 3;
                else if (cur->bInterfaceSubClass == 1 &&
                         cur->bInterfaceProtocol == 2) score = 2;
                if (score > best_score) {
                    best_score = score;
                    best = cur;
                    best_ep = ep;
                    best_rdlen = cur_rdlen;
                }
                cur = 0;   /* one endpoint per interface */
            }
        }
        off += blen;
    }
    if (!best) return -1;
    *intf_out = best;
    *ep_out = best_ep;
    *rdlen_out = best_rdlen;
    return 0;
}

/* ── endpoints and bulk transfers (class driver interface) ──────────────── */

static volatile int usb_locked;

void usb_lock(void) {
    while (__sync_lock_test_and_set(&usb_locked, 1))
        sleep_ticks(1);
}

void usb_unlock(void) {
    __sync_lock_release(&usb_locked);
}

int usb_device_slot(const struct usb_device *d) {
    return d->slot;
}

/* Configure Endpoint (xHCI 4.6.6) for up to USB_MAX_EPS endpoints; eps[i]
 * becomes endpoint index i for usb_bulk(). */
int usb_configure_eps(struct usb_device *d,
                      const usb_endpoint_desc_t *const *eps, int n) {
    if (n < 1 || n > USB_MAX_EPS) return -1;
    memset(d->dma->in_ctx, 0, sizeof(d->dma->in_ctx));
    uint32_t add = 1;
    int max_dci = 1;
    for (int i = 0; i < n; i++) {
        const usb_endpoint_desc_t *ep = eps[i];
        int in = (ep->bEndpointAddress & 0x80) != 0;
        int dci = (ep->bEndpointAddress & 0x0F) * 2 + in;
        int xfer = ep->bmAttributes & 3;            /* 2 bulk, 3 interrupt */
        uint32_t mps = ep->wMaxPacketSize & 0x7FF;
        if (dci < 2 || mps == 0 || (xfer != 2 && xfer != 3)) return -1;
        d->eps[i].dci = dci;
        d->eps[i].addr = ep->bEndpointAddress;
        d->eps[i].mps = (int)mps;
        ring_init(&d->eps[i].ring, d->dma->rings[i], XFER_RING_TRBS);
        add |= 1U << dci;
        if (dci > max_dci) max_dci = dci;

        /* EP type: 2 bulk OUT, 3 interrupt OUT, 6 bulk IN, 7 interrupt IN */
        uint32_t type = (uint32_t)xfer + (in ? 4U : 0U);
        uint32_t *ec = in_entry(d, dci);
        ec[0] = xfer == 3 ? ep_interval(d->speed, ep->bInterval) << 16 : 0;
        ec[1] = (3U << 1) | (type << 3) | (mps << 16);
        ec[2] = d->eps[i].ring.phys | 1;
        ec[3] = 0;
        /* Average TRB length, and for periodic endpoints the max ESIT
         * payload (xHCI 4.14.1.1, 6.2.3). */
        ec[4] = xfer == 3 ? (mps | (mps << 16)) : 3072;
    }
    in_ctrl(d)[1] = add;
    uint32_t *sc = in_entry(d, 0);
    uint32_t *oc = out_entry(d, 0);
    sc[0] = (oc[0] & ~(0x1FU << 27)) | ((uint32_t)max_dci << 27);
    sc[1] = oc[1];
    sc[2] = oc[2];
    int cc = run_command(phys_of(d->dma->in_ctx), 0,
                         TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(d->slot), 0);
    if (cc != CC_SUCCESS) {
        printk("[USB] slot %d: configure endpoint failed, code %d\n",
               d->slot, cc);
        return -1;
    }
    return 0;
}

/* One bulk transfer of `len` bytes (at most 64 KiB) at physical address
 * `phys` on endpoint index `i`, queued as one TD of chained Normal TRBs
 * split where the buffer crosses a 64 KiB boundary (xHCI 4.11.7.1).
 * Returns 0 (with the byte count in *actual), USB_STALL, or -1 on another
 * error or a timeout. */
int usb_bulk(struct usb_device *d, int i, uint32_t phys, uint32_t len,
             uint32_t *actual, uint32_t timeout_ms) {
    if (i < 0 || i >= USB_MAX_EPS || !d->eps[i].dci || len == 0 ||
        len > 0x10000) return -1;
    int n = 0;
    uint32_t p = phys, left = len, piece[XFER_MAX_PIECES];
    while (left && n < XFER_MAX_PIECES) {
        uint32_t room = 0x10000U - (p & 0xFFFFU);
        piece[n] = left < room ? left : room;
        p += piece[n];
        left -= piece[n];
        n++;
    }
    if (left) return -1;
    d->eps[i].done = 0;
    d->eps[i].code = 0;
    d->eps[i].actual = 0;
    d->eps[i].ntrb = 0;
    p = phys;
    for (int k = 0; k < n; k++) {
        /* TD Size (remaining packets) stays 0: allowed for the last TRB
         * and only a hint otherwise. */
        uint32_t ctl = TRB_TYPE(TRB_NORMAL) | TRB_ISP |
                       (k == n - 1 ? TRB_IOC : TRB_CHAIN);
        d->eps[i].trb_len[k] = piece[k];
        d->eps[i].trb[k] = ring_push(&d->eps[i].ring, p, 0, piece[k], ctl);
        d->eps[i].ntrb = k + 1;
        p += piece[k];
    }
    ring_doorbell((uint32_t)d->slot, (uint32_t)d->eps[i].dci);
    uint32_t end = pit_ticks() + (timeout_ms + 9) / 10 + 1;
    for (uint32_t spin = 0; !d->eps[i].done; spin++) {
        process_events();
        if (d->eps[i].done) break;
        if ((int32_t)(pit_ticks() - end) >= 0) {
            printk("[USB] slot %d: bulk transfer on endpoint %02x timed out\n",
                   d->slot, d->eps[i].addr);
            return -1;
        }
        if (spin > 200) sleep_ticks(1);
    }
    int cc = d->eps[i].code;
    if (cc == CC_STALL) return USB_STALL;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        printk("[USB] slot %d: bulk transfer on endpoint %02x failed, "
               "code %d\n", d->slot, d->eps[i].addr, cc);
        return -1;
    }
    if (actual) *actual = d->eps[i].actual;
    return 0;
}

/* Recover a halted endpoint (xHCI 4.6.8, 4.6.10; USB 2.0 9.4.1): Reset
 * Endpoint, move its dequeue pointer past whatever was queued, and clear the
 * device's halt feature. */
int usb_clear_halt(struct usb_device *d, int i) {
    if (i < 0 || i >= USB_MAX_EPS || !d->eps[i].dci) return -1;
    uint32_t ep = (uint32_t)d->eps[i].dci << 16;
    int cc = run_command(0, 0, TRB_TYPE(TRB_RESET_EP) | TRB_SLOT(d->slot) | ep,
                         0);
    if (cc != CC_SUCCESS && cc != CC_CONTEXT_STATE)
        printk("[USB] slot %d: reset endpoint failed, code %d\n", d->slot, cc);
    ring_t *r = &d->eps[i].ring;
    cc = run_command((r->phys + r->enq * (uint32_t)sizeof(xhci_trb_t)) |
                     r->cycle, 0,
                     TRB_TYPE(TRB_SET_TR_DEQ) | TRB_SLOT(d->slot) | ep, 0);
    if (cc != CC_SUCCESS)
        printk("[USB] slot %d: set dequeue pointer failed, code %d\n",
               d->slot, cc);
    usb_setup_t s = { 0x02, 1 /* CLEAR_FEATURE */, 0 /* ENDPOINT_HALT */,
                      (uint16_t)d->eps[i].addr, 0 };
    return usb_control(d, &s, 0) < 0 ? -1 : 0;
}

static void queue_report(struct usb_device *d) {
    ring_push(&d->eps[0].ring, phys_of(d->dma->report), 0,
              (uint32_t)d->eps[0].mps, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    ring_doorbell((uint32_t)d->slot, (uint32_t)d->eps[0].dci);
}

static void hid_transfer_done(struct usb_device *d, int code,
                              uint32_t residual) {
    if (!d->intr_active) return;
    if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
        uint32_t mps = (uint32_t)d->eps[0].mps;
        uint32_t got = mps - (residual <= mps ? residual : mps);
        if (!d->reported) {
            d->reported = 1;
            printk("[USB] slot %d: first %s report\n", d->slot,
                   hid_kind_name(d->hid.kind));
        }
        hid_report(&d->hid, d->dma->report, got);
        queue_report(d);
    } else {
        /* A halted endpoint would need Reset Endpoint + Set TR Dequeue;
         * a HID device that stalls its interrupt pipe is rare enough to
         * just stop listening to it. */
        printk("[USB] slot %d: interrupt transfer failed, code %d; "
               "device stopped\n", d->slot, code);
        d->intr_active = 0;
    }
}

static int setup_hid(struct usb_device *d, const usb_interface_desc_t *intf,
                     const usb_endpoint_desc_t *ep, uint16_t rdlen) {
    uint8_t ifn = intf->bInterfaceNumber;
    const uint8_t *rdesc = 0;
    static uint8_t rbuf[CTRL_BUF_SIZE];
    int boot = intf->bInterfaceSubClass == 1 &&
               (intf->bInterfaceProtocol == 1 || intf->bInterfaceProtocol == 2);

    if (boot) {
        /* Boot protocol: fixed report layouts (HID 1.11 appendix B). */
        control_out(d, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                    HID_REQ_SET_PROTOCOL, 0, ifn);
    } else if (rdlen) {
        if (rdlen > sizeof(rbuf)) rdlen = sizeof(rbuf);
        int got = get_descriptor(d, USB_RECIP_INTERFACE, USB_DT_REPORT, 0, ifn,
                                 rbuf, rdlen);
        if (got <= 0) return -1;
        rdesc = rbuf;
        rdlen = (uint16_t)got;
    }
    /* Report only on change; a STALL here is allowed (HID 7.2.4). */
    control_out(d, USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_REQ_SET_IDLE,
                0, ifn);

    memset(&d->hid, 0, sizeof(d->hid));
    hid_setup(&d->hid, intf, rdesc, rdesc ? rdlen : 0);
    if (d->hid.kind == HID_KIND_NONE) return -1;

    if ((ep->wMaxPacketSize & 0x7FF) > REPORT_BUF_SIZE) return -1;
    if (usb_configure_eps(d, &ep, 1) != 0) return -1;
    d->cls = USB_CLS_HID;
    d->intr_active = 1;
    queue_report(d);
    return 0;
}

static void hub_attach(struct usb_device *d, const usb_device_desc_t *dd);

/* Address and configure the device on root port `port`, or on port `pport`
 * of hub `parent`.  Returns it (also when no driver claims it, so that the
 * port is not enumerated again), or NULL if it could not be set up. */
static struct usb_device *enumerate(int port, int speed,
                                    struct usb_device *parent, int pport) {
    struct usb_device *d = 0;
    int idx = 0;
    for (; idx < XHCI_MAX_DEVS; idx++)
        if (!devs[idx].used) { d = &devs[idx]; break; }
    if (!d) {
        printk("[USB] port %d: too many devices, ignored\n", port);
        return 0;
    }
    memset(d, 0, sizeof(*d));
    d->used = 1;
    d->port = port;
    d->speed = speed;
    d->dma = &dev_dma[idx];
    memset(d->dma, 0, sizeof(*d->dma));
    if (parent) {
        d->parent = parent;
        d->parent_port = pport;
        d->depth = parent->depth + 1;
        d->route = parent->route | ((uint32_t)pport << (4 * parent->depth));
        if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
            if (parent->speed == USB_SPEED_HIGH) {
                d->tt_slot = parent->slot;
                d->tt_port = pport;
            } else {
                d->tt_slot = parent->tt_slot;
                d->tt_port = parent->tt_port;
            }
        }
        parent->child[pport] = d;
    } else {
        port_dev[port] = d;
    }

    int slot = 0;
    int cc = run_command(0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot);
    if (cc != CC_SUCCESS || slot <= 0 || slot > (int)max_slots) {
        printk("[USB] port %s: enable slot failed, code %d\n", where(d), cc);
        d->slot = 0;
        free_device(d);
        return 0;
    }
    d->slot = slot;
    slot_dev[slot] = d;

    /* Address Device (xHCI 4.3.3): slot context + EP0. */
    uint32_t mps0 = default_mps0(speed);
    ring_init(&d->ep0, d->dma->ep0, XFER_RING_TRBS);
    in_ctrl(d)[1] = 0x3;                                  /* A0 | A1 */
    uint32_t *sc = in_entry(d, 0);
    sc[0] = (1U << 27) | ((uint32_t)speed << 20) | d->route;
    sc[1] = (uint32_t)port << 16;
    sc[2] = (uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8);
    uint32_t *e0 = in_entry(d, 1);
    e0[1] = (3U << 1) | (4U << 3) | (mps0 << 16);         /* CErr 3, control */
    e0[2] = d->ep0.phys | 1;                              /* DCS = 1 */
    e0[3] = 0;
    e0[4] = 8;                                            /* average TRB len */
    dcbaa[slot] = phys_of(d->dma->out_ctx);
    __sync_synchronize();
    cc = run_command(phys_of(d->dma->in_ctx), 0,
                     TRB_TYPE(TRB_ADDRESS_DEV) | TRB_SLOT(slot), 0);
    if (cc != CC_SUCCESS) {
        printk("[USB] port %s: address device failed, code %d\n", where(d),
               cc);
        free_device(d);
        return 0;
    }

    usb_device_desc_t dd;
    memset(&dd, 0, sizeof(dd));
    if (get_descriptor(d, USB_RECIP_DEVICE, USB_DT_DEVICE, 0, 0, &dd, 8) < 8) {
        free_device(d);
        return 0;
    }
    if (speed < USB_SPEED_SUPER && dd.bMaxPacketSize0 &&
        dd.bMaxPacketSize0 != mps0) {
        /* Evaluate Context with the real EP0 max packet size. */
        mps0 = dd.bMaxPacketSize0;
        memset(d->dma->in_ctx, 0, sizeof(d->dma->in_ctx));
        in_ctrl(d)[1] = 0x2;                              /* A1 */
        e0 = in_entry(d, 1);
        e0[1] = (3U << 1) | (4U << 3) | (mps0 << 16);
        cc = run_command(phys_of(d->dma->in_ctx), 0,
                         TRB_TYPE(TRB_EVAL_CTX) | TRB_SLOT(slot), 0);
        if (cc != CC_SUCCESS)
            printk("[USB] slot %d: evaluate context failed, code %d\n",
                   slot, cc);
    }
    if (get_descriptor(d, USB_RECIP_DEVICE, USB_DT_DEVICE, 0, 0, &dd,
                       sizeof(dd)) < (int)sizeof(dd)) {
        free_device(d);
        return 0;
    }
    d->vid = dd.idVendor;
    d->pid = dd.idProduct;

    static uint8_t cfg[CTRL_BUF_SIZE];
    if (get_descriptor(d, USB_RECIP_DEVICE, USB_DT_CONFIG, 0, 0, cfg, 9) < 9) {
        free_device(d);
        return 0;
    }
    uint16_t total = ((usb_config_desc_t *)cfg)->wTotalLength;
    if (total > sizeof(cfg)) total = sizeof(cfg);
    int clen = get_descriptor(d, USB_RECIP_DEVICE, USB_DT_CONFIG, 0, 0, cfg,
                              total);
    if (clen < 9) {
        free_device(d);
        return 0;
    }
    uint8_t cfg_value = ((usb_config_desc_t *)cfg)->bConfigurationValue;

    const usb_interface_desc_t *intf;
    const usb_endpoint_desc_t *ep;
    uint16_t rdlen;
    int is_hub = dd.bDeviceClass == USB_CLASS_HUB;
    int is_hid = !is_hub &&
                 pick_hid(cfg, (uint32_t)clen, &intf, &ep, &rdlen) == 0;
    int is_msc = !is_hub && !is_hid && usb_msc_match(cfg, (uint32_t)clen);
    if (!is_hub && !is_hid && !is_msc) {
        printk("[USB] port %s: device %04x:%04x class %u, %s speed: "
               "no driver\n", where(d), dd.idVendor, dd.idProduct,
               dd.bDeviceClass, speed_name(speed));
        /* Keep the slot so the port is not re-enumerated in a loop. */
        return d;
    }
    if (control_out(d, USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_SET_CONFIGURATION, cfg_value, 0) < 0) {
        free_device(d);
        return 0;
    }
    if (is_hub) {
        hub_attach(d, &dd);
        return d;
    }
    if (is_msc) {
        printk("[USB] port %s: %04x:%04x %s speed, slot %d: mass storage\n",
               where(d), dd.idVendor, dd.idProduct, speed_name(speed), slot);
        if (usb_msc_attach(d, cfg, (uint32_t)clen) == 0)
            d->cls = USB_CLS_MSC;
        return d;
    }
    if (setup_hid(d, intf, ep, rdlen) != 0) {
        printk("[USB] port %s: device %04x:%04x: HID setup failed\n",
               where(d), dd.idVendor, dd.idProduct);
        return d;
    }
    printk("[USB] port %s: %04x:%04x %s speed, slot %d: HID %s "
           "(endpoint %d, %d bytes)\n", where(d), dd.idVendor, dd.idProduct,
           speed_name(speed), slot, hid_kind_name(d->hid.kind),
           d->eps[0].addr & 0x0F, d->eps[0].mps);
    return d;
}

/* ── USB 2.0 hubs (USB 2.0 chapter 11.24) ────────────────────────────────── */

#define HUB_RT_PORT_OUT   0x23     /* class, other (port), host-to-device */
#define HUB_RT_PORT_IN    0xA3
#define HUB_RT_HUB_IN     0xA0
#define HUB_REQ_GET_STATUS     0
#define HUB_REQ_CLEAR_FEATURE  1
#define HUB_REQ_SET_FEATURE    3
#define HUB_DT_HUB        0x29
#define PORT_RESET        4
#define PORT_POWER        8
#define C_PORT_CONNECTION 16       /* change features: 16 + change bit */
#define PS_CONNECTION     (1U << 0)
#define PS_ENABLE         (1U << 1)
#define PS_RESET          (1U << 4)
#define PS_LOW_SPEED      (1U << 9)
#define PS_HIGH_SPEED     (1U << 10)
#define PC_RESET          (1U << 4)

static int hub_port_feature(struct usb_device *h, int set, int feature,
                            int port) {
    return control_out(h, HUB_RT_PORT_OUT,
                       set ? HUB_REQ_SET_FEATURE : HUB_REQ_CLEAR_FEATURE,
                       (uint16_t)feature, (uint16_t)port);
}

/* wPortStatus | wPortChange << 16, or 0xFFFFFFFF on error. */
static uint32_t hub_port_status(struct usb_device *h, int port) {
    uint8_t st[4];
    usb_setup_t s = { HUB_RT_PORT_IN, HUB_REQ_GET_STATUS, 0, (uint16_t)port,
                      4 };
    if (usb_control(h, &s, st) != 4) return 0xFFFFFFFFU;
    return (uint32_t)st[0] | ((uint32_t)st[1] << 8) |
           ((uint32_t)st[2] << 16) | ((uint32_t)st[3] << 24);
}

static void hub_attach(struct usb_device *d, const usb_device_desc_t *dd) {
    uint8_t hd[9];
    if (d->speed >= USB_SPEED_SUPER) {
        printk("[USB] port %s: USB 3 hub %04x:%04x not supported\n",
               where(d), dd->idVendor, dd->idProduct);
        return;
    }
    if (d->depth >= HUB_MAX_DEPTH) {
        printk("[USB] port %s: hub too deep, ignored\n", where(d));
        return;
    }
    usb_setup_t s = { HUB_RT_HUB_IN, USB_REQ_GET_DESCRIPTOR,
                      HUB_DT_HUB << 8, 0, sizeof(hd) };
    if (usb_control(d, &s, hd) < 7) {
        printk("[USB] port %s: no hub descriptor\n", where(d));
        return;
    }
    int nports = hd[2] > HUB_MAX_PORTS ? HUB_MAX_PORTS : hd[2];
    uint32_t chars = (uint32_t)hd[3] | ((uint32_t)hd[4] << 8);
    uint32_t pwr_ms = (uint32_t)hd[5] * 2;

    /* Tell the controller this slot is a hub (xHCI 4.6.6, 6.2.2): Hub,
     * Number of Ports and, for a high-speed hub, the TT think time. */
    memset(d->dma->in_ctx, 0, sizeof(d->dma->in_ctx));
    in_ctrl(d)[1] = 1;
    uint32_t *sc = in_entry(d, 0);
    uint32_t *oc = out_entry(d, 0);
    sc[0] = oc[0] | (1U << 26);
    sc[1] = (oc[1] & 0x00FFFFFFU) | ((uint32_t)nports << 24);
    sc[2] = oc[2];
    if (d->speed == USB_SPEED_HIGH)
        sc[2] = (sc[2] & ~(3U << 16)) | (((chars >> 5) & 3) << 16);
    int cc = run_command(phys_of(d->dma->in_ctx), 0,
                         TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(d->slot), 0);
    if (cc != CC_SUCCESS)
        printk("[USB] slot %d: hub configure failed, code %d\n", d->slot, cc);

    d->hub_ports = nports;
    d->cls = USB_CLS_HUB;
    for (int p = 1; p <= nports; p++)
        hub_port_feature(d, 1, PORT_POWER, p);
    sleep_ms(pwr_ms < 100 ? 100 : pwr_ms);
    printk("[USB] port %s: %04x:%04x %s speed, slot %d: hub, %d ports\n",
           where(d), dd->idVendor, dd->idProduct, speed_name(d->speed),
           d->slot, nports);
}

/* Look at every port of hub `h`: acknowledge changes, drop unplugged
 * devices, reset and enumerate new ones. */
static void hub_scan(struct usb_device *h) {
    for (int p = 1; p <= h->hub_ports; p++) {
        uint32_t st = hub_port_status(h, p);
        if (st == 0xFFFFFFFFU) return;          /* the hub itself is gone */
        uint32_t change = st >> 16;
        for (int bit = 0; bit < 5; bit++)
            if (change & (1U << bit))
                hub_port_feature(h, 0, C_PORT_CONNECTION + bit, p);

        if (!(st & PS_CONNECTION) || (change & 1)) {
            /* Gone, or replaced since the last look. */
            h->hub_bad &= (uint16_t)~(1U << p);
            if (h->child[p]) {
                printk("[USB] port %s: device removed\n", where(h->child[p]));
                free_device(h->child[p]);
            }
            if (!(st & PS_CONNECTION)) continue;
        }
        if (h->child[p] || (h->hub_bad & (1U << p))) continue;

        hub_port_feature(h, 1, PORT_RESET, p);
        uint32_t end = pit_ticks() + 50 + 1;               /* 500 ms */
        do {
            sleep_ms(10);
            st = hub_port_status(h, p);
        } while (st != 0xFFFFFFFFU && !((st >> 16) & PC_RESET) &&
                 (int32_t)(pit_ticks() - end) < 0);
        if (st == 0xFFFFFFFFU) return;
        hub_port_feature(h, 0, C_PORT_CONNECTION + 4, p);   /* C_PORT_RESET */
        sleep_ms(10);
        st = hub_port_status(h, p);
        if (st == 0xFFFFFFFFU || !(st & PS_ENABLE)) {
            h->hub_bad |= (uint16_t)(1U << p);
            continue;
        }
        int speed = (st & PS_LOW_SPEED) ? USB_SPEED_LOW :
                    (st & PS_HIGH_SPEED) ? USB_SPEED_HIGH : USB_SPEED_FULL;
        if (!enumerate(h->port, speed, h, p))
            h->hub_bad |= (uint16_t)(1U << p);
    }
}

/* ── ports (xHCI 4.3, 4.19) ──────────────────────────────────────────────── */

static void port_check(int port) {
    uint32_t sc = rd32(op_regs, OP_PORTSC(port));
    /* Acknowledge every change bit we saw. */
    if (sc & PORTSC_CHANGES)
        wr32(op_regs, OP_PORTSC(port), (sc & PORTSC_KEEP) |
                                       (sc & PORTSC_CHANGES));

    if (!(sc & PORTSC_CCS)) {
        if (port_dev[port]) {
            printk("[USB] port %s: device removed\n", where(port_dev[port]));
            free_device(port_dev[port]);
        }
        return;
    }
    if (port_dev[port]) return;            /* already running */

    if (!(sc & PORTSC_PED)) {
        /* USB2 ports must be reset to enable them; USB3 ports enable
         * themselves after link training. */
        wr32(op_regs, OP_PORTSC(port), (sc & PORTSC_KEEP) | PORTSC_PR);
        if (wait_reg(op_regs, OP_PORTSC(port), PORTSC_PRC, PORTSC_PRC,
                     500) != 0) {
            printk("[USB] port %d: reset timed out\n", port);
            return;
        }
        sc = rd32(op_regs, OP_PORTSC(port));
        wr32(op_regs, OP_PORTSC(port), (sc & PORTSC_KEEP) |
                                       (sc & PORTSC_CHANGES));
        sleep_ms(10);                      /* reset recovery (USB 2.0 7.1.7.5) */
        sc = rd32(op_regs, OP_PORTSC(port));
        if (!(sc & PORTSC_PED)) {
            printk("[USB] port %d: not enabled after reset (portsc %08x)\n",
                   port, (unsigned)sc);
            return;
        }
    }
    enumerate(port, (int)PORTSC_SPEED(sc), 0, 0);
}

/* ── controller bring-up (xHCI 4.2) ──────────────────────────────────────── */

static void bios_handoff(void) {
    uint32_t xecp = (rd32(cap_regs, CAP_HCCPARAMS1) >> 16) << 2;
    for (int guard = 0; xecp && guard < 64; guard++) {
        uint32_t cap = rd32(cap_regs, xecp);
        if ((cap & 0xFF) == 1) {                     /* USB Legacy Support */
            if (cap & (1U << 16)) {
                wr32(cap_regs, xecp, cap | (1U << 24));   /* OS owned */
                if (wait_reg(cap_regs, xecp, (1U << 16), 0, 1000) != 0)
                    printk("[XHCI] BIOS did not release the controller\n");
                else
                    printk("[XHCI] took the controller over from the BIOS\n");
            }
            /* Disable the SMIs and clear their status (RW1C). */
            uint32_t ctl = rd32(cap_regs, xecp + 4);
            wr32(cap_regs, xecp + 4, ctl & 0xE0000000U);
            return;
        }
        uint32_t next = (cap >> 8) & 0xFF;
        if (!next) break;
        xecp += next << 2;
    }
}

static int hc_start(void) {
    bios_handoff();

    /* Stop, then reset. */
    wr32(op_regs, OP_USBCMD, rd32(op_regs, OP_USBCMD) & ~USBCMD_RS);
    if (wait_reg(op_regs, OP_USBSTS, USBSTS_HCH, USBSTS_HCH, 100) != 0)
        printk("[XHCI] controller did not halt\n");
    wr32(op_regs, OP_USBCMD, USBCMD_HCRST);
    if (wait_reg(op_regs, OP_USBCMD, USBCMD_HCRST, 0, 1000) != 0 ||
        wait_reg(op_regs, OP_USBSTS, USBSTS_CNR, 0, 1000) != 0) {
        printk("[XHCI] controller reset timed out\n");
        return -1;
    }
    if (!(rd32(op_regs, OP_PAGESIZE) & 1)) {
        printk("[XHCI] controller does not support 4 KiB pages\n");
        return -1;
    }

    uint32_t hcs1 = rd32(cap_regs, CAP_HCSPARAMS1);
    uint32_t hcs2 = rd32(cap_regs, CAP_HCSPARAMS2);
    max_slots = hcs1 & 0xFF;
    max_ports = (hcs1 >> 24) & 0xFF;
    if (max_slots > 255) max_slots = 255;
    wr32(op_regs, OP_CONFIG, max_slots);

    /* Scratchpad buffers the controller asks for (xHCI 4.20). */
    uint32_t nscratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    memset(dcbaa, 0, sizeof(dcbaa));
    if (nscratch) {
        if (nscratch > SCRATCH_MAX) {
            printk("[XHCI] wants %u scratchpad pages; too many\n",
                   (unsigned)nscratch);
            return -1;
        }
        for (uint32_t i = 0; i < nscratch; i++) {
            uint32_t f = pmm_alloc_frame();
            if (!f) {
                printk("[XHCI] out of memory for scratchpads\n");
                return -1;
            }
            scratch_array[i] = f;
        }
        dcbaa[0] = phys_of(scratch_array);
    }
    wr64(op_regs, OP_DCBAAP, phys_of(dcbaa));

    ring_init(&cmd_ring, cmd_trbs, CMD_RING_TRBS);
    wr64(op_regs, OP_CRCR, cmd_ring.phys | 1);            /* RCS = 1 */

    memset(evt_trbs, 0, sizeof(evt_trbs));
    evt_deq = 0;
    evt_cycle = 1;
    erst[0].base = phys_of(evt_trbs);
    erst[0].size = EVT_RING_TRBS;
    erst[0].rsvd = 0;
    wr32(rt_regs, RT_ERSTSZ, 1);
    wr64(rt_regs, RT_ERDP, phys_of(evt_trbs));
    wr64(rt_regs, RT_ERSTBA, phys_of(erst));
    /* Interrupts stay off (IMAN.IE = 0, USBCMD.INTE = 0): kusbd polls. */

    wr32(op_regs, OP_USBCMD, USBCMD_RS);
    if (wait_reg(op_regs, OP_USBSTS, USBSTS_HCH, 0, 100) != 0) {
        printk("[XHCI] controller did not start\n");
        return -1;
    }
    /* With Port Power Control the ports come out of reset unpowered
     * (xHCI 4.19.4). */
    if (rd32(cap_regs, CAP_HCCPARAMS1) & (1U << 3)) {
        for (uint32_t p = 1; p <= max_ports; p++) {
            uint32_t sc = rd32(op_regs, OP_PORTSC(p));
            if (!(sc & PORTSC_PP))
                wr32(op_regs, OP_PORTSC(p), (sc & PORTSC_KEEP) | PORTSC_PP);
        }
        sleep_ms(20);
    }
    printk("[XHCI] running: %u ports, %u slots, %u-byte contexts, "
           "%u scratchpads\n", (unsigned)max_ports, (unsigned)max_slots,
           (unsigned)ctx_size, (unsigned)nscratch);
    return 0;
}

static void kusbd(void) {
    usb_lock();
    if (hc_start() != 0) {
        printk("[XHCI] giving up on the controller\n");
        for (;;) sleep_ticks(1000000);
    }
    hc_running = 1;
    /* Ports with a device attached at power-on may not report a change
     * after the reset; give the links a moment, then look at all of them. */
    sleep_ms(100);
    process_events();
    for (uint32_t p = 1; p <= max_ports; p++)
        port_check((int)p);
    memset(port_pending, 0, sizeof(port_pending));
    uint32_t next_hub_poll = pit_ticks();
    usb_unlock();

    for (;;) {
        usb_lock();
        process_events();
        for (uint32_t w = 0; w < 8; w++) {
            while (port_pending[w]) {
                int bit = __builtin_ctz(port_pending[w]);
                port_pending[w] &= ~(1U << bit);
                uint32_t p = w * 32 + (uint32_t)bit;
                if (p >= 1 && p <= max_ports) port_check((int)p);
            }
        }
        for (int i = 0; i < XHCI_MAX_DEVS; i++)
            if (devs[i].used && devs[i].cls == USB_CLS_HID &&
                devs[i].intr_active)
                hid_tick(&devs[i].hid);
        if ((int32_t)(pit_ticks() - next_hub_poll) >= 0) {
            next_hub_poll = pit_ticks() + 25;
            for (int i = 0; i < XHCI_MAX_DEVS; i++)
                if (devs[i].used && devs[i].cls == USB_CLS_HUB)
                    hub_scan(&devs[i]);
        }
        if (rd32(op_regs, OP_USBSTS) & USBSTS_HSE) {
            printk("[XHCI] host system error; controller stopped\n");
            for (;;) sleep_ticks(1000000);
        }
        usb_unlock();
        sleep_ticks(1);
    }
}

void xhci_init(void) {
    hc_pci = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *p = pci_get_device(i);
        if (p->class_code == 0x0C && p->subclass == 0x03 &&
            p->prog_if == 0x30) {
            hc_pci = p;
            break;
        }
    }
    if (!hc_pci) return;

    uint32_t bar0 = hc_pci->bar[0];
    if (bar0 & 1) {
        printk("[XHCI] BAR0 is not a memory BAR\n");
        hc_pci = 0;
        return;
    }
    if (((bar0 >> 1) & 3) == 2 && hc_pci->bar[1] != 0) {
        printk("[XHCI] registers above 4 GiB are not supported\n");
        hc_pci = 0;
        return;
    }
    uint32_t base = bar0 & ~0xFU;

    /* Intel 7/8/9-series PCHs power up with every port routed to their EHCI
     * controllers; switch the ones the firmware allows over to xHCI
     * (USB3_PSSEN/XUSB2PR from their masks USB3PRM/XUSB2PRM, per the PCH
     * datasheets). */
    static const uint16_t intel_switchable[] = {
        0x1E31, 0x8C31, 0x8CB1, 0x8D31, 0x9C31, 0x9CB1,
    };
    for (unsigned i = 0; hc_pci->vendor_id == 0x8086 &&
                         i < sizeof(intel_switchable) / sizeof(uint16_t); i++) {
        if (hc_pci->device_id != intel_switchable[i]) continue;
        uint8_t b0 = hc_pci->bus, s0 = hc_pci->slot, f0 = hc_pci->func;
        pci_write_config32(b0, s0, f0, 0xD8, pci_read_config32(b0, s0, f0, 0xDC));
        pci_write_config32(b0, s0, f0, 0xD0, pci_read_config32(b0, s0, f0, 0xD4));
        printk("[XHCI] routed the Intel PCH's ports to xHCI\n");
    }

    /* BAR size: write all ones, read the mask back, restore. */
    uint8_t b = hc_pci->bus, s = hc_pci->slot, f = hc_pci->func;
    uint32_t cmd = pci_read_config32(b, s, f, 0x04);
    pci_write_config32(b, s, f, 0x04, cmd & ~0x3U);
    pci_write_config32(b, s, f, 0x10, 0xFFFFFFFFU);
    uint32_t size = ~(pci_read_config32(b, s, f, 0x10) & ~0xFU) + 1;
    pci_write_config32(b, s, f, 0x10, bar0);
    /* memory space + bus master; INTx stays as firmware left it */
    pci_write_config32(b, s, f, 0x04, (cmd & 0xFFFFU) | 0x6U);
    if (size == 0 || size > XHCI_MMIO_MAX) size = XHCI_MMIO_MAX;

    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        if (paging_map(XHCI_MMIO_VIRT + off, base + off,
                       PAGE_PRESENT | PAGE_WRITABLE | PAGE_NOCACHE |
                       PAGE_WRITETHRU) != 0) {
            printk("[XHCI] cannot map registers\n");
            hc_pci = 0;
            return;
        }
    }

    cap_regs = (volatile uint8_t *)XHCI_MMIO_VIRT;
    op_regs = cap_regs + (rd32(cap_regs, CAP_CAPLENGTH) & 0xFF);
    rt_regs = cap_regs + (rd32(cap_regs, CAP_RTSOFF) & ~0x1FU);
    db_regs = (volatile uint32_t *)(cap_regs +
                                    (rd32(cap_regs, CAP_DBOFF) & ~0x3U));
    ctx_size = (rd32(cap_regs, CAP_HCCPARAMS1) & (1U << 2)) ? 64 : 32;
    printk("[XHCI] %02x:%02x.%u %04x:%04x at 0x%08x (%u KiB), version "
           "%x.%02x\n", b, s, f, hc_pci->vendor_id, hc_pci->device_id,
           (unsigned)base, (unsigned)(size / 1024),
           (unsigned)(rd32(cap_regs, 0) >> 24),
           (unsigned)((rd32(cap_regs, 0) >> 16) & 0xFF));
}

void xhci_start_thread(void) {
    if (!hc_pci) return;
    if (!proc_create_kthread(kusbd, "kusbd"))
        printk("[XHCI] cannot start kusbd\n");
}
