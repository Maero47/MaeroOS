/*
 * xHCI host controller driver: low/full/high/super-speed devices on the root
 * hub and behind USB 2.0 and USB 3 hubs, with control transfers on endpoint
 * 0, bulk endpoints (mass storage, usb_msc.c) and interrupt-IN endpoints
 * (every HID interface of a device, see usb_hid.c; hub status changes).
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
 *     the ports, and then sleeps until the controller interrupts (MSI when
 *     the function has it and there is a Local APIC, else INTx on its PIC
 *     line, which may be shared), with a slow fallback poll in case an
 *     interrupt is lost.  The interrupt handler only acknowledges it and
 *     wakes the sleepers; the event ring is read in thread context by
 *     whoever holds the USB lock (kusbd, or a process waiting for its own
 *     transfer), so the driver needs no locking of its own; the input rings
 *     it feeds are protected by keyboard.c/mouse.c.  Interrupter moderation
 *     caps the rate at 4000 interrupts a second.  Without an interrupt line,
 *     or with "xhci=poll" on the command line, kusbd polls every tick.
 *   - Ports are scanned at start and rescanned on Port Status Change events,
 *     so devices plugged in later are picked up; unplugging frees the slot.
 *     A device whose root port lost its connection is marked gone at once,
 *     so transfers waiting on it fail without running into their timeouts.
 *
 * Class drivers: usb_hid.c (keyboards, mice, tablets) and usb_msc.c (mass
 * storage, bulk-only transport); USB 2.0 and USB 3 hubs are handled here
 * (hub_*), their ports looked at when the hub's status-change endpoint says
 * so (and every few seconds regardless).  They call back in through usb.h:
 * usb_control(), usb_configure_eps() and usb_bulk().  usb_lock() serialises
 * every use of the controller between kusbd and processes doing disk I/O.
 *
 * Not done (see the report / ROADMAP): MSI-X, isochronous transfers,
 * streams, 64-bit DMA addresses.
 */
#include "xhci.h"
#include "usb.h"
#include "../pci.h"
#include "../../arch/i686/cpu/pit.h"
#include "../../arch/i686/mm/paging.h"
#include "../../mm/mmio.h"
#include "../../include/kernel/config.h"
#include "../../kernel/printk.h"
#include "../../lib/string.h"
#include "../../mm/pmm.h"
#include "../../proc/process.h"
#include "../../proc/scheduler.h"
#include "../../arch/i686/cpu/irq.h"
#include "../../arch/i686/cpu/apic.h"
#include "../../arch/i686/cpu/pic.h"
#include "../../include/kernel/boot_info.h"
#include "../keyboard.h"
#include <stdint.h>

/* The register BAR is mapped uncached through mmio_map(), at most this much
 * of it (see the kernel virtual map in include/kernel/config.h). */
#define XHCI_MMIO_MAX    0x00100000U

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
#define USBCMD_INTE     (1U << 2)
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
#define IMAN_IP         (1U << 0)
#define IMAN_IE         (1U << 1)
/* Interrupter moderation interval, in 250 ns units: at most one interrupt
 * per 250 us (xHCI 5.5.2.2). */
#define IMOD_INTERVAL   1000U

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
#define TRB_STOP_EP      15
#define TRB_SET_TR_DEQ   16
#define TRB_EV_TRANSFER  32
#define TRB_EV_CMD       33
#define TRB_EV_PORT      34
#define TRB_EV_HC        37

#define CC_SUCCESS       1
#define CC_STALL         6
#define CC_SHORT_PACKET  13
#define CC_CONTEXT_STATE 19
#define CC_STOPPED       26
#define CC_STOPPED_LEN   27
#define CC_STOPPED_SHORT 28
#define CC_IS_STOPPED(c) ((c) >= CC_STOPPED && (c) <= CC_STOPPED_SHORT)

/* Endpoint Context EP State (xHCI 6.2.3) */
#define EP_STATE_RUNNING 1
#define EP_STATE_HALTED  2

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
    uint8_t    report[USB_MAX_EPS][REPORT_BUF_SIZE];   /* interrupt-IN data */
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
    uint32_t hub_ttt;        /* high-speed hub: TT think time */
    int hub_ss;              /* a USB 3 (SuperSpeed) hub */
    int hub_change;          /* the status-change endpoint reported */
    uint32_t hub_next_scan;  /* tick of the next unprompted port scan */
    int gone;                /* unplugged: fail transfers at once */
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
        /* interrupt-IN endpoints: what the reports are for, and whether a
         * TD is queued (a failed one is not queued again) */
        int role;            /* EP_BULK, EP_HID or EP_HUB */
        int active;
        int reported;        /* HID: logged the first report */
    } eps[USB_MAX_EPS];
    int cls;                 /* USB_CLS_* */
    int nhid;                /* HID interfaces, eps[0..nhid-1] */
    /* control transfer in flight */
    uint32_t ctl_data_trb, ctl_status_trb;
    uint32_t ctl_residual;
    int ctl_done, ctl_code;
    hid_state_t hid[USB_MAX_EPS];
    uint16_t vid, pid;
};

static struct usb_device devs[XHCI_MAX_DEVS];
static struct usb_device *slot_dev[256];
static struct usb_device *port_dev[256];

/* ── controller state ────────────────────────────────────────────────────── */

static const pci_device_t *hc_pci;
static volatile uint8_t *cap_regs, *op_regs, *rt_regs;
static uint32_t mmio_size;
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

/* Interrupts: how they arrive, and a count of them (plus kicks from other
 * code) that sleepers compare against to see whether anything happened
 * since they last looked at the event ring. */
#define IRQ_POLL 0
#define IRQ_INTX 1
#define IRQ_MSI  2
static int irq_mode;
static int irq_line;
static volatile uint32_t evt_seq;
static volatile uint32_t irq_count;
static int evt_chan;
/* kusbd's fallback poll, in ticks, when interrupts are on */
#define FALLBACK_TICKS   50
/* unprompted hub port scans: with a status-change endpoint, and without */
#define HUB_SCAN_SLOW    500
#define HUB_SCAN_FAST    25

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

/* ── interrupts ──────────────────────────────────────────────────────────── */

/* The interrupt handler (MSI vector, or a PIC line that other devices may
 * share).  It only acknowledges the interrupt and wakes whoever waits for
 * the controller; the event ring itself is read by process_events() under
 * the USB lock.  Interrupter 0 interrupts again only once the event ring
 * dequeue pointer is written back with EHB cleared (xHCI 4.17.2), which
 * process_events() does after draining the ring. */
static void xhci_irq(registers_t *regs) {
    (void)regs;
    if (!rt_regs) return;
    uint32_t iman = rd32(rt_regs, RT_IMAN);
    if (irq_mode == IRQ_INTX) {
        /* A shared line: IP set means it was us.  With MSI the controller
         * clears IP itself once the message is sent (xHCI 5.5.2.1). */
        if (!(iman & IMAN_IP)) return;
        wr32(rt_regs, RT_IMAN, iman | IMAN_IP);           /* RW1C */
    }
    wr32(op_regs, OP_USBSTS, USBSTS_EINT);                /* RW1C */
    irq_count++;
    evt_seq++;
    wake_up(&evt_chan);
}

/* Something outside the event ring wants kusbd to look (keyboard LEDs). */
void usb_kick(void) {
    evt_seq++;
    wake_up(&evt_chan);
}

/* Sleep until the controller interrupts, or `ticks` pass.  `seen` is the
 * evt_seq the caller read before it last drained the event ring: if it moved
 * since, an interrupt came in between and there is no sleeping.  Polled,
 * this is a plain timed sleep.  (An interrupt landing on another CPU right
 * between the check and the sleep is caught by the timeout.) */
static void wait_event(uint32_t seen, uint32_t ticks) {
    if (!current_proc) return;
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    if (irq_mode != IRQ_POLL && evt_seq != seen) {
        if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
        return;
    }
    current_proc->wake_tick = pit_ticks() + (ticks ? ticks : 1);
    sleep_on(irq_mode != IRQ_POLL ? (void *)&evt_chan : (void *)&sleep_chan);
    if (!(fl & 0x200)) __asm__ volatile("cli" ::: "memory");
}

/* Waiting for a transfer or command: spin briefly (QEMU and fast devices
 * complete within microseconds), then sleep until an interrupt, with a
 * one-tick safety net when interrupts are on. */
static void wait_progress(uint32_t seen, uint32_t spin) {
    if (spin <= 200) return;
    wait_event(seen, irq_mode != IRQ_POLL ? 2 : 1);
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
        /* Hand the Link TRB over with the current cycle, then wrap.  A Link
         * TRB inside a TD must have its Chain bit set, or the controller
         * may end the TD there (xHCI 4.11.5.1, 6.4.4.1): it takes the
         * Chain bit of the TRB just queued. */
        volatile xhci_trb_t *link = &r->trbs[r->n - 1];
        link->control = (link->control & ~(TRB_CYCLE | TRB_CHAIN)) |
                        (control & TRB_CHAIN) | r->cycle;
        __sync_synchronize();
        r->enq = 0;
        r->cycle ^= 1;
    }
    return phys;
}

/* Boot-time check of the ring logic on a scratch ring nobody executes: a
 * chained TRB queued just before the Link TRB must leave the Link chained
 * and the cycle toggled; an unchained one must leave it unchained. */
static int ring_selftest(void) {
    static xhci_trb_t t[8];
    ring_t r;
    ring_init(&r, t, 8);
    for (int i = 0; i < 6; i++)
        ring_push(&r, 0, 0, 0, TRB_TYPE(TRB_NORMAL));
    ring_push(&r, 0, 0, 0, TRB_TYPE(TRB_NORMAL) | TRB_CHAIN);
    int ok = r.enq == 0 && r.cycle == 0 &&
             (t[7].control & TRB_CHAIN) && (t[7].control & TRB_CYCLE) &&
             (t[7].control & TRB_TC);
    for (int i = 0; i < 6; i++)
        ring_push(&r, 0, 0, 0, TRB_TYPE(TRB_NORMAL) | TRB_CHAIN);
    ring_push(&r, 0, 0, 0, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ok = ok && r.enq == 0 && r.cycle == 1 && !(t[7].control & TRB_CHAIN) &&
         !(t[7].control & TRB_CYCLE) && (t[6].control & TRB_CYCLE) == 0;
    return ok ? 0 : -1;
}

static inline void ring_doorbell(uint32_t slot, uint32_t target) {
    __sync_synchronize();
    db_regs[slot] = target;
}

/* ── events ──────────────────────────────────────────────────────────────── */

static void intr_transfer_done(struct usb_device *d, int i, int code,
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
    /* An error on a TRB not in the transfer: the endpoint failed before
     * reaching it.  A Stopped event for an abandoned TD (ep_abort) is not
     * news for the transfer in flight. */
    if (code != CC_SUCCESS && code != CC_SHORT_PACKET && !CC_IS_STOPPED(code)) {
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
                (code != CC_SUCCESS && code != CC_SHORT_PACKET &&
                 !CC_IS_STOPPED(code))) {
                d->ctl_code = code;
                d->ctl_done = 1;
            }
        } else {
            for (int i = 0; i < USB_MAX_EPS; i++) {
                if (d->eps[i].dci != ep) continue;
                if (d->eps[i].role != EP_BULK) {
                    intr_transfer_done(d, i, code, ev->status & 0xFFFFFF);
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
        /* Disconnected: everything behind this root port is gone, and a
         * transfer waiting on it should not sit out its timeout. */
        if (port >= 1 && port <= max_ports &&
            !(rd32(op_regs, OP_PORTSC(port)) & PORTSC_CCS))
            for (int i = 0; i < XHCI_MAX_DEVS; i++)
                if (devs[i].used && devs[i].port == (int)port)
                    devs[i].gone = 1;
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
        /* INTx: drop the line here too.  On an edge-triggered PIC input a
         * line that stays high (IP set again before the handler's clear,
         * the handler run late on a shared line) gives no further edges;
         * the next event raises it afresh. */
        if (irq_mode == IRQ_INTX) {
            uint32_t iman = rd32(rt_regs, RT_IMAN);
            if (iman & IMAN_IP) wr32(rt_regs, RT_IMAN, iman | IMAN_IP);
        }
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
        uint32_t seen = evt_seq;
        process_events();
        if (cmd_done_trb == trb) {
            if (slot_out) *slot_out = cmd_done_slot;
            return cmd_done_code;
        }
        if ((int32_t)(pit_ticks() - end) >= 0) break;
        wait_progress(seen, spin);
    }
    printk("[XHCI] command type %u timed out (usbsts %x crcr %x evt0 %x/%x "
           "cmd %x trb %x)\n",
           (unsigned)((control >> 10) & 0x3F),
           (unsigned)rd32(op_regs, OP_USBSTS), (unsigned)rd32(op_regs, OP_CRCR),
           (unsigned)evt_trbs[0].control, (unsigned)evt_trbs[0].param_lo,
           (unsigned)cmd_trbs[0].control, (unsigned)trb);
    return -1;
}

/* ── endpoint recovery (xHCI 4.6.8, 4.6.9, 4.6.10) ──────────────────────── */

static inline uint32_t *out_ep_ctx(struct usb_device *d, int dci);

/* Throw away whatever is queued on endpoint `dci` (ring `r`): a timed-out
 * TD, or the rest of a TD after a STALL or error.  A Halted endpoint is
 * reset, a Running one stopped (Reset Endpoint and Set TR Dequeue Pointer
 * both fail with a Context State Error on a Running endpoint), then its
 * dequeue pointer is moved to our enqueue pointer, so the controller never
 * touches the abandoned TRBs or the buffers they point at again.  Returns
 * 0 when the endpoint ended up Stopped at our enqueue pointer. */
static int ep_abort(struct usb_device *d, int dci, ring_t *r) {
    uint32_t ep = (uint32_t)dci << 16;
    uint32_t state = out_ep_ctx(d, dci)[0] & 7;
    int cc = CC_SUCCESS;
    if (state == EP_STATE_HALTED)
        cc = run_command(0, 0, TRB_TYPE(TRB_RESET_EP) | TRB_SLOT(d->slot) | ep,
                         0);
    else if (state == EP_STATE_RUNNING)
        cc = run_command(0, 0, TRB_TYPE(TRB_STOP_EP) | TRB_SLOT(d->slot) | ep,
                         0);
    if (cc != CC_SUCCESS && cc != CC_CONTEXT_STATE)
        printk("[USB] slot %d: %s endpoint %d failed, code %d\n", d->slot,
               state == EP_STATE_HALTED ? "reset" : "stop", dci, cc);
    uint32_t deq = r->phys + r->enq * (uint32_t)sizeof(xhci_trb_t);
    cc = run_command(deq | r->cycle, 0,
                     TRB_TYPE(TRB_SET_TR_DEQ) | TRB_SLOT(d->slot) | ep, 0);
    if (cc != CC_SUCCESS) {
        printk("[USB] slot %d: set dequeue pointer on endpoint %d failed, "
               "code %d\n", d->slot, dci, cc);
        return -1;
    }
    uint32_t *ec = out_ep_ctx(d, dci);
    return ((ec[0] & 7) == 3 && (ec[2] & ~0xFU) == deq) ? 0 : -1;
}

/* ── control transfers (xHCI 4.11.2.2) ───────────────────────────────────── */

int usb_control(struct usb_device *d, const usb_setup_t *setup, void *data) {
    uint32_t len = setup->wLength;
    int in = (setup->bmRequestType & USB_DIR_IN) != 0;
    if (len > CTRL_BUF_SIZE || d->gone) return -1;
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
        uint32_t seen = evt_seq;
        process_events();
        if (d->ctl_done) break;
        if (d->gone) return -1;              /* unplugged: slot goes soon */
        if ((int32_t)(pit_ticks() - end) >= 0) {
            printk("[USB] slot %d: control request %02x/%02x timed out\n",
                   d->slot, setup->bmRequestType, setup->bRequest);
            ep_abort(d, 1, &d->ep0);
            return -1;
        }
        wait_progress(seen, spin);
    }
    if (d->ctl_code != CC_SUCCESS && d->ctl_code != CC_SHORT_PACKET) {
        if (d->ctl_code != CC_STALL)
            printk("[USB] slot %d: control request %02x/%02x failed, "
                   "code %d\n", d->slot, setup->bmRequestType,
                   setup->bRequest, d->ctl_code);
        /* A STALL (a request the device does not support) or an error
         * halts EP0 in the controller; the next request needs it back. */
        ep_abort(d, 1, &d->ep0);
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

/* Output (device) context of endpoint `dci` (entry 0 is the slot). */
static inline uint32_t *out_ep_ctx(struct usb_device *d, int dci) {
    return out_entry(d, dci);
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
    for (int i = 0; i < USB_MAX_EPS; i++) d->eps[i].active = 0;
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

/* The HID interfaces of a configuration (alternate setting 0, with an
 * interrupt-IN endpoint), at most USB_MAX_EPS: a keyboard often has a boot
 * keyboard interface and a second one for its media keys.  Fills the
 * interface, its endpoint and its report descriptor length per entry and
 * returns how many there are. */
typedef struct {
    const usb_interface_desc_t *intf;
    const usb_endpoint_desc_t *ep;
    uint16_t rdlen;
} hid_intf_t;

static int find_hids(const uint8_t *cfg, uint32_t len, hid_intf_t *out) {
    const usb_interface_desc_t *cur = 0;
    uint16_t cur_rdlen = 0;
    int n = 0;
    for (uint32_t off = 0; off + 2 <= len && n < USB_MAX_EPS;) {
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
                out[n].intf = cur;
                out[n].ep = ep;
                out[n].rdlen = cur_rdlen;
                n++;
                cur = 0;   /* one endpoint per interface */
            }
        }
        off += blen;
    }
    return n;
}

/* ── endpoints and bulk transfers (class driver interface) ──────────────── */

static volatile int usb_locked;

/* Waiters sleep a tick at a time, so a thread that unlocks and locks again
 * at once (a reader looping over a stick that keeps timing out) could keep
 * the lock from them for good -- kusbd then never scans the hub that would
 * tell it the stick is gone.  A newcomer that finds anyone waiting queues
 * behind them instead of taking the lock straight away. */
static volatile int usb_waiters;

void usb_lock(void) {
    if (!usb_waiters && !__sync_lock_test_and_set(&usb_locked, 1))
        return;
    __sync_add_and_fetch(&usb_waiters, 1);
    int first = 1;
    for (;;) {
        if (!first && !__sync_lock_test_and_set(&usb_locked, 1)) break;
        first = 0;
        sleep_ticks(1);
    }
    __sync_sub_and_fetch(&usb_waiters, 1);
}

void usb_unlock(void) {
    __sync_lock_release(&usb_locked);
}

int usb_device_slot(const struct usb_device *d) {
    return d->slot;
}

/* A hub's slot context fields (xHCI 6.2.2): Hub, Number of Ports and, for
 * a high-speed hub, the TT think time from wHubCharacteristics. */
static void hub_slot_bits(struct usb_device *d, uint32_t *sc) {
    if (!d->hub_ports) return;
    sc[0] |= 1U << 26;
    sc[1] = (sc[1] & 0x00FFFFFFU) | ((uint32_t)d->hub_ports << 24);
    if (d->speed == USB_SPEED_HIGH)
        sc[2] = (sc[2] & ~(3U << 16)) | ((d->hub_ttt & 3) << 16);
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
        d->eps[i].role = EP_BULK;
        d->eps[i].active = 0;
        d->eps[i].reported = 0;
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
    hub_slot_bits(d, sc);
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
        len > 0x10000 || d->gone) return -1;
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
        uint32_t seen = evt_seq;
        process_events();
        if (d->eps[i].done) break;
        if (d->gone) return -1;
        if ((int32_t)(pit_ticks() - end) >= 0) {
            printk("[USB] slot %d: bulk transfer on endpoint %02x timed out\n",
                   d->slot, d->eps[i].addr);
            /* Take the TD back before the caller reuses its buffer. */
            ep_abort(d, d->eps[i].dci, &d->eps[i].ring);
            return -1;
        }
        wait_progress(seen, spin);
    }
    int cc = d->eps[i].code;
    if (cc == CC_STALL) return USB_STALL;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        printk("[USB] slot %d: bulk transfer on endpoint %02x failed, "
               "code %d\n", d->slot, d->eps[i].addr, cc);
        ep_abort(d, d->eps[i].dci, &d->eps[i].ring);
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
    ep_abort(d, d->eps[i].dci, &d->eps[i].ring);
    usb_setup_t s = { 0x02, 1 /* CLEAR_FEATURE */, 0 /* ENDPOINT_HALT */,
                      (uint16_t)d->eps[i].addr, 0 };
    return usb_control(d, &s, 0) < 0 ? -1 : 0;
}

/* Queue one interrupt-IN TD on endpoint index i (HID report, hub status). */
static void queue_intr(struct usb_device *d, int i) {
    ring_push(&d->eps[i].ring, phys_of(d->dma->report[i]), 0,
              (uint32_t)d->eps[i].mps, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    ring_doorbell((uint32_t)d->slot, (uint32_t)d->eps[i].dci);
}

static void intr_transfer_done(struct usb_device *d, int i, int code,
                               uint32_t residual) {
    if (!d->eps[i].active) return;
    if (CC_IS_STOPPED(code)) return;     /* a TD ep_abort() took back */
    if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
        uint32_t mps = (uint32_t)d->eps[i].mps;
        uint32_t got = mps - (residual <= mps ? residual : mps);
        if (d->eps[i].role == EP_HUB) {
            /* Ports changed: kusbd looks at them (clearing the change
             * bits) and queues the next status TD. */
            if (got) d->hub_change = 1;
            else queue_intr(d, i);
            return;
        }
        if (!d->eps[i].reported) {
            d->eps[i].reported = 1;
            printk("[USB] slot %d: first %s report\n", d->slot,
                   hid_kind_name(d->hid[i].kind));
        }
        hid_report(&d->hid[i], d->dma->report[i], got);
        queue_intr(d, i);
    } else {
        /* A halted endpoint would need Reset Endpoint + Set TR Dequeue;
         * a device that stalls its interrupt pipe is rare enough to just
         * stop listening to it (a hub's ports are then polled). */
        if (!d->gone)
            printk("[USB] slot %d: interrupt transfer failed, code %d; "
                   "endpoint %02x stopped\n", d->slot, code, d->eps[i].addr);
        d->eps[i].active = 0;
    }
}

#define HID_REQ_SET_REPORT  0x09

/* Keyboard LEDs (HID 1.11 7.2.2, boot output report B.1): SET_REPORT of a
 * one-byte output report, bit 0 Num Lock, 1 Caps Lock, 2 Scroll Lock. */
static void hid_set_leds(struct usb_device *d, int i, uint8_t leds) {
    usb_setup_t s = { USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_REQ_SET_REPORT,
                      0x0200, d->hid[i].ifnum, 1 };
    int r = usb_control(d, &s, &leds);
    d->hid[i].leds = leds;
    printk("[USB] slot %d: keyboard LEDs num %s caps %s scroll %s "
           "(SET_REPORT %s)\n", d->slot, (leds & 1) ? "on" : "off",
           (leds & 2) ? "on" : "off", (leds & 4) ? "on" : "off",
           r < 0 ? "failed" : "ok");
}

/* Set up one HID interface as endpoint index `i`'s function: protocol,
 * report descriptor, idle rate.  0 when the driver handles it. */
static int setup_hid_intf(struct usb_device *d, int i, const hid_intf_t *h) {
    const usb_interface_desc_t *intf = h->intf;
    uint8_t ifn = intf->bInterfaceNumber;
    const uint8_t *rdesc = 0;
    uint16_t rdlen = h->rdlen;
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

    memset(&d->hid[i], 0, sizeof(d->hid[i]));
    hid_setup(&d->hid[i], intf, rdesc, rdesc ? rdlen : 0);
    d->hid[i].ifnum = ifn;
    d->hid[i].leds = 0xFF;                   /* unknown: set on first look */
    if (d->hid[i].kind == HID_KIND_NONE) return -1;
    if ((h->ep->wMaxPacketSize & 0x7FF) > REPORT_BUF_SIZE) return -1;
    return 0;
}

/* Every HID interface the driver handles, each on its own interrupt-IN
 * endpoint; returns how many (0: none, the device is left alone). */
static int setup_hid(struct usb_device *d, const hid_intf_t *hids, int n) {
    const usb_endpoint_desc_t *eps[USB_MAX_EPS];
    int used = 0;
    for (int k = 0; k < n; k++) {
        if (setup_hid_intf(d, used, &hids[k]) != 0) continue;
        eps[used++] = hids[k].ep;
    }
    if (!used || usb_configure_eps(d, eps, used) != 0) return 0;
    d->cls = USB_CLS_HID;
    d->nhid = used;
    for (int i = 0; i < used; i++) {
        d->eps[i].role = EP_HID;
        d->eps[i].active = 1;
        queue_intr(d, i);
    }
    static int abort_checked;
    if (!abort_checked) {
        /* Check the timeout path once per boot on a real endpoint: the
         * first report TD sits on a Running endpoint (nothing to report
         * yet); abort it as a timed-out transfer is and queue it again. */
        abort_checked = 1;
        int was = (int)(out_ep_ctx(d, d->eps[0].dci)[0] & 7);
        int ok = ep_abort(d, d->eps[0].dci, &d->eps[0].ring) == 0;
        printk("[XHCI] self-test: abort of a pending TD on a %s endpoint "
               "%s\n", was == EP_STATE_RUNNING ? "running" : "non-running",
               ok ? "ok" : "FAILED");
        queue_intr(d, 0);
    }
    return used;
}

static void hub_attach(struct usb_device *d, const usb_device_desc_t *dd,
                       const uint8_t *cfg, uint32_t clen);

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

    hid_intf_t hids[USB_MAX_EPS];
    int is_hub = dd.bDeviceClass == USB_CLASS_HUB;
    int nhids = is_hub ? 0 : find_hids(cfg, (uint32_t)clen, hids);
    int is_hid = nhids > 0;
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
        hub_attach(d, &dd, cfg, (uint32_t)clen);
        return d;
    }
    if (is_msc) {
        printk("[USB] port %s: %04x:%04x %s speed, slot %d: mass storage\n",
               where(d), dd.idVendor, dd.idProduct, speed_name(speed), slot);
        if (usb_msc_attach(d, cfg, (uint32_t)clen) == 0)
            d->cls = USB_CLS_MSC;
        return d;
    }
    if (!setup_hid(d, hids, nhids)) {
        printk("[USB] port %s: device %04x:%04x: HID setup failed\n",
               where(d), dd.idVendor, dd.idProduct);
        return d;
    }
    for (int i = 0; i < d->nhid; i++)
        printk("[USB] port %s: %04x:%04x %s speed, slot %d: HID %s "
               "(interface %d, endpoint %d, %d bytes)\n", where(d),
               dd.idVendor, dd.idProduct, speed_name(speed), slot,
               hid_kind_name(d->hid[i].kind), d->hid[i].ifnum,
               d->eps[i].addr & 0x0F, d->eps[i].mps);
    return d;
}

/* ── hubs (USB 2.0 chapter 11.24, USB 3.2 chapter 10.16) ────────────────── */

#define HUB_RT_PORT_OUT   0x23     /* class, other (port), host-to-device */
#define HUB_RT_PORT_IN    0xA3
#define HUB_RT_HUB_OUT    0x20
#define HUB_RT_HUB_IN     0xA0
#define HUB_REQ_GET_STATUS     0
#define HUB_REQ_CLEAR_FEATURE  1
#define HUB_REQ_SET_FEATURE    3
#define HUB_REQ_SET_HUB_DEPTH  12  /* USB 3 hubs only (USB 3.2 10.16.2.9) */
#define HUB_DT_HUB        0x29
#define HUB_DT_SS_HUB     0x2A
#define PORT_RESET        4
#define PORT_POWER        8
#define C_PORT_CONNECTION 16       /* change features: 16 + change bit */
#define PS_CONNECTION     (1U << 0)
#define PS_ENABLE         (1U << 1)
#define PS_RESET          (1U << 4)
#define PS_LOW_SPEED      (1U << 9)
#define PS_HIGH_SPEED     (1U << 10)
#define PC_RESET          (1U << 4)

/* wPortChange bit -> the feature that clears it.  USB 2.0 hubs: C_PORT_
 * CONNECTION, ENABLE, SUSPEND, OVER_CURRENT, RESET (16..20).  USB 3 hubs
 * (USB 3.2 table 10-11): CONNECTION 16, OVER_CURRENT 19, RESET 20,
 * BH_RESET 29, LINK_STATE 25, CONFIG_ERROR 26; bits 1 and 2 are reserved. */
static const uint8_t ss_change_feature[8] = { 16, 0, 0, 19, 20, 29, 25, 26 };

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

/* Acknowledge every change bit in `change` on port `p`. */
static void hub_clear_changes(struct usb_device *h, int p, uint32_t change) {
    for (int bit = 0; bit < 8; bit++) {
        if (!(change & (1U << bit))) continue;
        int f = h->hub_ss ? ss_change_feature[bit]
                          : (bit < 5 ? C_PORT_CONNECTION + bit : 0);
        if (f) hub_port_feature(h, 0, f, p);
    }
}

/* The hub's interrupt-IN status-change endpoint (USB 2.0 11.12.1). */
static const usb_endpoint_desc_t *hub_status_ep(const uint8_t *cfg,
                                                uint32_t len) {
    for (uint32_t off = 0; off + 2 <= len;) {
        uint8_t blen = cfg[off], type = cfg[off + 1];
        if (blen < 2 || off + blen > len) break;
        if (type == USB_DT_ENDPOINT && blen >= 7) {
            const usb_endpoint_desc_t *ep =
                (const usb_endpoint_desc_t *)(cfg + off);
            if ((ep->bEndpointAddress & 0x80) && (ep->bmAttributes & 3) == 3)
                return ep;
        }
        off += blen;
    }
    return 0;
}

static void hub_attach(struct usb_device *d, const usb_device_desc_t *dd,
                       const uint8_t *cfg, uint32_t clen) {
    uint8_t hd[12];
    int ss = d->speed >= USB_SPEED_SUPER;
    if (d->depth >= HUB_MAX_DEPTH) {
        printk("[USB] port %s: hub too deep, ignored\n", where(d));
        return;
    }
    /* USB 3 hubs have their own descriptor type (USB 3.2 10.15.2.1): the
     * number of ports and the power-on time sit where USB 2.0's do. */
    usb_setup_t s = { HUB_RT_HUB_IN, USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)((ss ? HUB_DT_SS_HUB : HUB_DT_HUB) << 8), 0,
                      (uint16_t)(ss ? 12 : 9) };
    if (usb_control(d, &s, hd) < 7) {
        printk("[USB] port %s: no hub descriptor\n", where(d));
        return;
    }
    int nports = hd[2] > HUB_MAX_PORTS ? HUB_MAX_PORTS : hd[2];
    uint32_t chars = (uint32_t)hd[3] | ((uint32_t)hd[4] << 8);
    uint32_t pwr_ms = (uint32_t)hd[5] * 2;
    if (ss) {
        /* The hub's tier below the root hub, so it can take its port
         * number out of the route string (USB 3.2 10.16.2.9, 8.9). */
        if (control_out(d, HUB_RT_HUB_OUT, HUB_REQ_SET_HUB_DEPTH,
                        (uint16_t)d->depth, 0) < 0)
            printk("[USB] port %s: SET_HUB_DEPTH failed\n", where(d));
    }

    /* Tell the controller this slot is a hub (xHCI 4.6.6, 6.2.2), and
     * set up the status-change endpoint in the same Configure Endpoint. */
    d->hub_ports = nports;
    d->hub_ss = ss;
    d->hub_ttt = (chars >> 5) & 3;
    const usb_endpoint_desc_t *sep = hub_status_ep(cfg, clen);
    int have_ep = 0;
    if (sep && (sep->wMaxPacketSize & 0x7FF) <= REPORT_BUF_SIZE &&
        usb_configure_eps(d, &sep, 1) == 0) {
        have_ep = 1;
    } else {
        memset(d->dma->in_ctx, 0, sizeof(d->dma->in_ctx));
        in_ctrl(d)[1] = 1;
        uint32_t *sc = in_entry(d, 0);
        uint32_t *oc = out_entry(d, 0);
        sc[0] = oc[0];
        sc[1] = oc[1];
        sc[2] = oc[2];
        hub_slot_bits(d, sc);
        int cc = run_command(phys_of(d->dma->in_ctx), 0,
                             TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(d->slot), 0);
        if (cc != CC_SUCCESS)
            printk("[USB] slot %d: hub configure failed, code %d\n",
                   d->slot, cc);
    }

    d->cls = USB_CLS_HUB;
    for (int p = 1; p <= nports; p++)
        hub_port_feature(d, 1, PORT_POWER, p);
    sleep_ms(pwr_ms < 100 ? 100 : pwr_ms);
    /* Look at the ports right away; then when the hub says so. */
    d->hub_change = 1;
    d->hub_next_scan = pit_ticks();
    if (have_ep) {
        d->eps[0].role = EP_HUB;
        d->eps[0].active = 1;
    }
    printk("[USB] port %s: %04x:%04x %s speed, slot %d: %shub, %d ports%s\n",
           where(d), dd->idVendor, dd->idProduct, speed_name(d->speed),
           d->slot, ss ? "USB 3 " : "", nports,
           have_ep ? ", status endpoint" : ", polled");
}

/* Look at every port of hub `h`: acknowledge changes, drop unplugged
 * devices, reset and enumerate new ones. */
static void hub_scan(struct usb_device *h) {
    for (int p = 1; p <= h->hub_ports; p++) {
        uint32_t st = hub_port_status(h, p);
        if (st == 0xFFFFFFFFU) return;          /* the hub itself is gone */
        uint32_t change = st >> 16;
        hub_clear_changes(h, p, change);

        if (!(st & PS_CONNECTION) || (change & 1)) {
            /* Gone, or replaced since the last look. */
            h->hub_bad &= (uint16_t)~(1U << p);
            if (h->child[p]) {
                h->child[p]->gone = 1;
                printk("[USB] port %s: device removed\n", where(h->child[p]));
                free_device(h->child[p]);
            }
            if (!(st & PS_CONNECTION)) continue;
        }
        if (h->child[p] || (h->hub_bad & (1U << p))) continue;

        /* USB 2.0 ports are enabled by a reset; a USB 3 port enables
         * itself after link training, and is reset only if it did not. */
        if (!h->hub_ss || !(st & PS_ENABLE)) {
            hub_port_feature(h, 1, PORT_RESET, p);
            uint32_t end = pit_ticks() + 50 + 1;               /* 500 ms */
            do {
                sleep_ms(10);
                st = hub_port_status(h, p);
            } while (st != 0xFFFFFFFFU && !((st >> 16) & PC_RESET) &&
                     (int32_t)(pit_ticks() - end) < 0);
            if (st == 0xFFFFFFFFU) return;
            hub_clear_changes(h, p, (st >> 16) & PC_RESET);
            sleep_ms(10);
            st = hub_port_status(h, p);
        }
        if (st == 0xFFFFFFFFU || !(st & PS_ENABLE)) {
            h->hub_bad |= (uint16_t)(1U << p);
            continue;
        }
        /* Behind a USB 3 hub everything is SuperSpeed (its USB 2.0 half
         * is a separate hub on the companion root port). */
        int speed = h->hub_ss ? USB_SPEED_SUPER :
                    (st & PS_LOW_SPEED) ? USB_SPEED_LOW :
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
            port_dev[port]->gone = 1;
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
    printk("[XHCI] self-test: ring wrap with chained TD %s\n",
           ring_selftest() == 0 ? "ok" : "FAILED");
    printk("[USB-HID] self-test: consumer control (array, variables) %s\n",
           hid_selftest() == 0 ? "ok" : "FAILED");
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
    /* Interrupter 0 on, moderated (xHCI 4.17.2, 5.5.2); polled, it stays
     * off (IMAN.IE = 0, USBCMD.INTE = 0). */
    uint32_t cmd = USBCMD_RS;
    if (irq_mode != IRQ_POLL) {
        wr32(rt_regs, RT_IMOD, IMOD_INTERVAL);
        wr32(rt_regs, RT_IMAN, IMAN_IE | IMAN_IP);
        cmd |= USBCMD_INTE;
    }

    wr32(op_regs, OP_USBCMD, cmd);
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

/* Count the devices in use (the replug test checks nothing leaks). */
static int devs_in_use(void) {
    int n = 0;
    for (int i = 0; i < XHCI_MAX_DEVS; i++) n += devs[i].used;
    return n;
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
    usb_unlock();
    int last_in_use = -1;

    for (;;) {
        uint32_t seen = evt_seq;
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
        /* Hubs: when the status-change endpoint reported, and every so
         * often anyway (often when there is no working endpoint). */
        uint32_t now = pit_ticks();
        for (int i = 0; i < XHCI_MAX_DEVS; i++) {
            struct usb_device *h = &devs[i];
            if (!h->used || h->cls != USB_CLS_HUB) continue;
            if (!h->hub_change && (int32_t)(now - h->hub_next_scan) < 0)
                continue;
            int prompted = h->hub_change;
            h->hub_change = 0;
            h->hub_next_scan = now + (h->eps[0].active ? HUB_SCAN_SLOW
                                                       : HUB_SCAN_FAST);
            hub_scan(h);
            if (h->used && prompted && h->eps[0].active) queue_intr(h, 0);
        }
        int repeating = 0;
        uint8_t leds = keyboard_leds();
        for (int i = 0; i < XHCI_MAX_DEVS; i++) {
            struct usb_device *d = &devs[i];
            if (!d->used || d->cls != USB_CLS_HID) continue;
            for (int k = 0; k < d->nhid; k++) {
                if (!d->eps[k].active) continue;
                hid_tick(&d->hid[k]);
                if (d->hid[k].repeat_key) repeating = 1;
                if (d->hid[k].kind == HID_KIND_KEYBOARD &&
                    d->hid[k].leds != leds)
                    hid_set_leds(d, k, leds);
            }
        }
        if (devs_in_use() != last_in_use) {
            last_in_use = devs_in_use();
            printk("[USB] %d device(s) in use, %u interrupts\n", last_in_use,
                   (unsigned)irq_count);
        }
        if (rd32(op_regs, OP_USBSTS) & USBSTS_HSE) {
            printk("[XHCI] host system error; controller stopped\n");
            for (;;) sleep_ticks(1000000);
        }
        usb_unlock();
        usb_msc_service();
        /* Key repeat needs the tick; otherwise sleep until the controller
         * interrupts, or the fallback poll. */
        wait_event(seen, (repeating || irq_mode == IRQ_POLL) ? 1
                                                             : FALLBACK_TICKS);
    }
}

/* Interrupts: MSI-X or MSI on the BSP's Local APIC if the function has
 * them, else its INTx line through the PIC ("xhci=intx" skips MSI,
 * "xhci=poll" both). */
static void irq_setup(void) {
    const char *cl = boot_info_cmdline();
    int want = 2;
    for (const char *c = cl; c && *c; c++)
        if ((c == cl || c[-1] == ' ') && strncmp(c, "xhci=", 5) == 0) {
            if (strncmp(c + 5, "poll", 4) == 0) want = 0;
            else if (strncmp(c + 5, "intx", 4) == 0) want = 1;
        }
    irq_mode = IRQ_POLL;
    uint8_t bir;
    uint32_t toff;
    uint8_t xcap = pci_msix_table(hc_pci, &bir, &toff);
    if (want >= 2 && xcap && bir == 0 && toff + 16 <= mmio_size) {
        /* MSI-X entry 0 (interrupter 0's), in the register BAR we map. */
        int vec = msi_install_handler(xhci_irq);
        if (vec >= 0) {
            volatile uint8_t *e = cap_regs + toff;
            wr32(e, 0, 0xFEE00000U | (apic_id() << 12));
            wr32(e, 4, 0);
            wr32(e, 8, (uint32_t)vec);
            wr32(e, 12, 0);                               /* unmasked */
            pci_msix_enable(hc_pci, xcap);
            irq_mode = IRQ_MSI;
            printk("[XHCI] interrupts: MSI-X, vector 0x%02x to APIC %u\n",
                   vec, (unsigned)apic_id());
            return;
        }
    }
    if (want >= 2 && pci_find_cap(hc_pci, 0x05)) {
        int vec = msi_install_handler(xhci_irq);
        if (vec >= 0 &&
            pci_enable_msi(hc_pci, (uint8_t)vec, (uint8_t)apic_id()) == 0) {
            irq_mode = IRQ_MSI;
            printk("[XHCI] interrupts: MSI, vector 0x%02x to APIC %u\n", vec,
                   (unsigned)apic_id());
            return;
        }
    }
    if (want >= 1 && hc_pci->irq_line >= 1 && hc_pci->irq_line <= 15) {
        irq_line = hc_pci->irq_line;
        irq_install_handler((uint8_t)irq_line, xhci_irq);
        pic_unmask((uint8_t)irq_line);
        /* INTx back on in case firmware (or an earlier MSI) disabled it. */
        uint8_t b = hc_pci->bus, s = hc_pci->slot, f = hc_pci->func;
        uint32_t cmd = pci_read_config32(b, s, f, 0x04);
        pci_write_config32(b, s, f, 0x04, (cmd & 0xFFFFU) & ~(1U << 10));
        irq_mode = IRQ_INTX;
        printk("[XHCI] interrupts: INTx on IRQ %d (shared)\n", irq_line);
        return;
    }
    printk("[XHCI] interrupts: none, polling every tick\n");
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

    mmio_size = size;
    cap_regs = mmio_map(base, size);
    if (!cap_regs) {
        printk("[XHCI] cannot map registers\n");
        hc_pci = 0;
        return;
    }
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
    irq_setup();
    if (!proc_create_kthread(kusbd, "kusbd"))
        printk("[XHCI] cannot start kusbd\n");
}
