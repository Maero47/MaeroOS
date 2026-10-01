#include "nvme.h"
#include "pci.h"
#include "../kernel/printk.h"
#include "../lib/string.h"
#include "../arch/i686/mm/paging.h"
#include "../mm/mmio.h"
#include "../arch/i686/cpu/spinlock.h"
#include "../arch/i686/cpu/tsc.h"
#include <kernel/config.h>
#include <io.h>
#include <stdint.h>

/*
 * NVM Express controller driver (NVM Express Base Specification, rev 1.4 /
 * 2.0; register, command and field names below follow it).
 *
 * The host talks to the controller through queues in host memory: it writes
 * 64-byte commands into a submission queue (SQ) and rings that queue's tail
 * doorbell; the controller posts 16-byte entries to a completion queue (CQ),
 * flipping each entry's phase tag (P) so the host can tell new entries from
 * old ones without reading a register.  Queue 0 is the admin queue, set up
 * through the AQA/ASQ/ACQ registers; I/O queues are created with admin
 * commands.  Data buffers are described by PRP entries: PRP1 points at the
 * first page, PRP2 at the second page or, for more than two pages, at a PRP
 * list (one 8-byte page address per entry).
 *
 * As in drivers/ahci.c, the driver uses this the simplest way that is still
 * correct:
 *   - one command at a time, polled for completion with interrupts off and
 *     `nvme_lock` held; all interrupt vectors are masked (INTMS) and the
 *     queues are created without interrupts enabled, so the controller never
 *     raises one;
 *   - one I/O queue pair (QID 1) per controller;
 *   - data moves through a physically contiguous bounce buffer in .bss big
 *     enough for the largest request (256 sectors = 32 pages); a transfer of
 *     more than two pages uses a PRP list, and requests larger than the
 *     controller's MDTS are split;
 *   - queues and PRP lists live in page-aligned .bss.  The kernel image is
 *     physically contiguous and below 4 GiB, so kphys() is a subtraction and
 *     the upper halves of 64-bit addresses are 0.
 *
 * Error handling: a command that fails with a status is reported and
 * returned as an error.  A command that times out leaves the queues in an
 * unknown state, so the controller is reset and its I/O queue recreated
 * (namespace data is kept) before the request is retried once.
 */

/* ── Controller registers, offsets from BAR0 ───────────────────────────── */
#define NVME_CAP     0x00   /* 64-bit */
#define NVME_VS      0x08
#define NVME_INTMS   0x0C
#define NVME_CC      0x14
#define NVME_CSTS    0x1C
#define NVME_AQA     0x24
#define NVME_ASQ     0x28   /* 64-bit */
#define NVME_ACQ     0x30   /* 64-bit */
#define NVME_DBS     0x1000 /* doorbells */

/* CAP (low and high dword) */
#define CAP_MQES(lo)     ((lo) & 0xFFFFu)            /* max queue entries - 1 */
#define CAP_TO(lo)       (((lo) >> 24) & 0xFFu)      /* ready timeout, 500 ms units */
#define CAP_DSTRD(hi)    ((hi) & 0xFu)               /* doorbell stride: 4 << DSTRD */
#define CAP_CSS_NVM(hi)  (((hi) >> 5) & 1u)          /* bit 37: NVM command set */
#define CAP_MPSMIN(hi)   (((hi) >> 16) & 0xFu)       /* min page size: 4 KiB << MPSMIN */

#define CC_EN            (1u << 0)
#define CC_SHN_NORMAL    (1u << 14)
#define CC_SHN_MASK      (3u << 14)
#define CC_IOSQES        (6u << 16)                  /* 2^6 = 64-byte SQ entries */
#define CC_IOCQES        (4u << 20)                  /* 2^4 = 16-byte CQ entries */

#define CSTS_RDY         (1u << 0)
#define CSTS_CFS         (1u << 1)
#define CSTS_SHST_MASK   (3u << 2)
#define CSTS_SHST_DONE   (2u << 2)

/* ── Commands ──────────────────────────────────────────────────────────── */
#define ADM_CREATE_SQ    0x01
#define ADM_CREATE_CQ    0x05
#define ADM_IDENTIFY     0x06
#define ADM_SET_FEATURES 0x09

#define NVM_FLUSH        0x00
#define NVM_WRITE        0x01
#define NVM_READ         0x02

#define CNS_NAMESPACE    0x00
#define CNS_CONTROLLER   0x01
#define CNS_ACTIVE_NSIDS 0x02

#define FEAT_NUM_QUEUES  0x07

typedef struct {
    uint32_t cdw0;         /* OPC[7:0] FUSE[9:8] PSDT[15:14] CID[31:16] */
    uint32_t nsid;
    uint32_t rsv[2];
    uint32_t mptr[2];
    uint32_t prp1[2];
    uint32_t prp2[2];
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} nvme_sqe_t;

typedef struct {
    uint32_t dw0;          /* command specific */
    uint32_t rsv;
    uint32_t dw2;          /* SQHD[15:0] SQID[31:16] */
    uint32_t dw3;          /* CID[15:0] P[16] SF[31:17] */
} nvme_cqe_t;

_Static_assert(sizeof(nvme_sqe_t) == 64, "submission queue entry is 64 bytes");
_Static_assert(sizeof(nvme_cqe_t) == 16, "completion queue entry is 16 bytes");

#define PAGE             4096u
#define ADMIN_DEPTH      16u
#define IO_DEPTH         32u

/* Queues and the PRP list of one controller, each in its own page (queues
 * must be page aligned with CC.MPS = 4 KiB). */
typedef struct {
    nvme_sqe_t asq[PAGE / sizeof(nvme_sqe_t)];
    nvme_cqe_t acq[PAGE / sizeof(nvme_cqe_t)];
    nvme_sqe_t iosq[PAGE / sizeof(nvme_sqe_t)];
    nvme_cqe_t iocq[PAGE / sizeof(nvme_cqe_t)];
    uint32_t   prp_list[PAGE / 4];          /* 8-byte entries, low/high */
} __attribute__((aligned(4096))) nvme_ctrl_mem_t;

_Static_assert(sizeof(nvme_ctrl_mem_t) == 5 * 4096, "one page each");

#define NVME_BOUNCE_BYTES (256u * 512u)     /* 32 pages */

typedef struct {
    nvme_sqe_t *sq;
    nvme_cqe_t *cq;
    uint16_t    depth;
    uint16_t    sq_tail, cq_head;
    uint8_t     phase;
    uint16_t    cid;
    uint32_t    sq_db, cq_db;               /* doorbell offsets */
} nvme_queue_t;

typedef struct {
    volatile uint8_t *regs;
    nvme_ctrl_mem_t  *mem;
    nvme_queue_t      admin, io;
    uint32_t          timeout_ms;           /* CAP.TO */
    uint32_t          max_pages;            /* per command (MDTS), 0 = no limit */
    uint16_t          io_depth;
    uint8_t           vwc;                  /* volatile write cache present */
    uint8_t           ok;                   /* enabled with an I/O queue */
    /* Reported at shutdown (smoke-nvme checks them). */
    uint32_t          n_io, n_prp_list, n_split, max_io_pages;
} nvme_ctrl_t;

typedef struct {
    nvme_ctrl_t *ctrl;
    uint32_t     nsid;
    uint64_t     sectors;
} nvme_disk_t;

/*
 * Each controller's registers plus the first doorbells, 16 KiB, are mapped
 * uncached through mmio_map() during boot (see the kernel virtual map in
 * include/kernel/config.h).
 */
#define NVME_MMIO_SIZE   0x4000u
#define NVME_MAX_CTRL    4

static nvme_ctrl_mem_t ctrl_mem[NVME_MAX_CTRL];
static uint8_t         nvme_bounce[NVME_BOUNCE_BYTES] __attribute__((aligned(4096)));

static nvme_ctrl_t ctrls[NVME_MAX_CTRL];
static int         nctrls;
static nvme_disk_t disks[NVME_MAX_DISKS];
static int         ndisks;
static spinlock_t  nvme_lock;

static uint32_t kphys(const void *p) {
    return (uint32_t)(uintptr_t)p - (uint32_t)KERNEL_VMA;
}

static inline uint32_t rd(nvme_ctrl_t *c, uint32_t off) {
    return *(volatile uint32_t *)(c->regs + off);
}

static inline void wr(nvme_ctrl_t *c, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(c->regs + off) = v;
}

static inline uint32_t nvme_irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void nvme_irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* Wall-clock timeouts, as in drivers/ahci.c: on the TSC once it is
 * calibrated, else a poll budget where each poll writes the POST port. */
typedef struct {
    uint64_t end;
    uint32_t spins, max_spins;
} nvme_timer_t;

static inline uint64_t nvme_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void timer_start(nvme_timer_t *t, uint32_t ms) {
    uint32_t cpt = tsc_cycles_per_tick();          /* cycles per 10 ms */
    t->spins = 0;
    if (cpt) {
        t->end = nvme_rdtsc() + (uint64_t)cpt * ms / 10u + 1u;
        t->max_spins = 0;
    } else {
        t->end = 0;
        t->max_spins = ms * 1000u;
    }
}

static int timer_expired(nvme_timer_t *t) {
    if (t->end)
        return nvme_rdtsc() >= t->end;
    outb(0x80, 0);
    return ++t->spins > t->max_spins;
}

/* Wait until (CSTS & mask) == want; 0 on success, -1 on timeout. */
static int wait_csts(nvme_ctrl_t *c, uint32_t mask, uint32_t want, uint32_t ms) {
    nvme_timer_t t;
    timer_start(&t, ms);
    do {
        if ((rd(c, NVME_CSTS) & mask) == want)
            return 0;
    } while (!timer_expired(&t));
    return (rd(c, NVME_CSTS) & mask) == want ? 0 : -1;
}

/* ── Queues ────────────────────────────────────────────────────────────── */

static void queue_init(nvme_ctrl_t *c, nvme_queue_t *q, uint16_t qid,
                       nvme_sqe_t *sq, nvme_cqe_t *cq, uint16_t depth) {
    uint32_t stride = 4u << CAP_DSTRD(rd(c, NVME_CAP + 4));
    memset(sq, 0, PAGE);
    memset(cq, 0, PAGE);
    q->sq = sq;
    q->cq = cq;
    q->depth = depth;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->phase = 1;                       /* the controller posts P=1 first */
    q->sq_db = NVME_DBS + (2u * qid) * stride;
    q->cq_db = NVME_DBS + (2u * qid + 1u) * stride;
}

/*
 * Submit `cmd` on `q` and poll for its completion.  The status field
 * (SF without the phase bit: SCT/SC/more/DNR) goes to *status and dword 0 to
 * *dw0.  Returns 0 on success, -1 on an error status, -2 on a timeout.
 */
static int queue_exec(nvme_ctrl_t *c, nvme_queue_t *q, nvme_sqe_t *cmd,
                      uint32_t timeout_ms, uint32_t *dw0) {
    uint16_t cid = q->cid++;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFFu) | ((uint32_t)cid << 16);
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (uint16_t)((q->sq_tail + 1u) % q->depth);
    __asm__ volatile("" ::: "memory");          /* entry before doorbell */
    wr(c, q->sq_db, q->sq_tail);

    nvme_timer_t t;
    timer_start(&t, timeout_ms);
    volatile nvme_cqe_t *e;
    for (;;) {
        e = &q->cq[q->cq_head];
        if (((e->dw3 >> 16) & 1u) == q->phase)
            break;
        if (timer_expired(&t))
            return -2;
        __asm__ volatile("pause");
    }
    __asm__ volatile("" ::: "memory");
    uint32_t dw3 = e->dw3;
    if (dw0)
        *dw0 = e->dw0;
    if (++q->cq_head == q->depth) {
        q->cq_head = 0;
        q->phase ^= 1u;
    }
    wr(c, q->cq_db, q->cq_head);

    uint32_t status = dw3 >> 17;
    if ((dw3 & 0xFFFFu) != cid) {
        printk("[NVMe] completion for CID %u, expected %u\n",
               (unsigned)(dw3 & 0xFFFFu), (unsigned)cid);
        return -2;
    }
    if (status & 0x7FFu) {                      /* SCT[10:8] SC[7:0] */
        printk("[NVMe] command 0x%02x failed: SCT %u SC 0x%02x\n",
               (unsigned)(cmd->cdw0 & 0xFFu), (unsigned)((status >> 8) & 7u),
               (unsigned)(status & 0xFFu));
        return -1;
    }
    return 0;
}

/* PRP1/PRP2 for `bytes` at `off` (page aligned) in the bounce buffer. */
static void set_prps(nvme_ctrl_t *c, nvme_sqe_t *cmd, uint32_t off, uint32_t bytes) {
    uint32_t base = kphys(nvme_bounce) + off;
    uint32_t pages = (bytes + PAGE - 1u) / PAGE;
    cmd->prp1[0] = base;
    if (pages == 2) {
        cmd->prp2[0] = base + PAGE;
    } else if (pages > 2) {
        c->n_prp_list++;
        uint32_t *l = c->mem->prp_list;
        for (uint32_t i = 1; i < pages; i++) {
            l[2u * (i - 1u)]      = base + i * PAGE;
            l[2u * (i - 1u) + 1u] = 0;
        }
        cmd->prp2[0] = kphys(l);
    }
}

static int admin_identify(nvme_ctrl_t *c, uint32_t cns, uint32_t nsid) {
    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = ADM_IDENTIFY;
    cmd.nsid = nsid;
    cmd.cdw10 = cns;
    set_prps(c, &cmd, 0, PAGE);
    return queue_exec(c, &c->admin, &cmd, c->timeout_ms, 0);
}

/* ── Bring-up ──────────────────────────────────────────────────────────── */

/*
 * Reset (CC.EN = 0), program the admin queue, enable, and create I/O queue
 * pair 1 (spec 3.5.1, "memory-based controller initialization").  0 on
 * success.
 */
static int ctrl_enable(nvme_ctrl_t *c) {
    uint32_t cc = rd(c, NVME_CC);
    if (cc & CC_EN) {
        /* Clearing EN while RDY is still on its way to 1 is undefined. */
        (void)wait_csts(c, CSTS_RDY, CSTS_RDY, c->timeout_ms);
        wr(c, NVME_CC, cc & ~CC_EN);
    }
    if (wait_csts(c, CSTS_RDY, 0, c->timeout_ms) < 0) {
        printk("[NVMe] controller does not reset (CSTS=0x%08x)\n",
               (unsigned)rd(c, NVME_CSTS));
        return -1;
    }

    nvme_ctrl_mem_t *m = c->mem;
    queue_init(c, &c->admin, 0, m->asq, m->acq, ADMIN_DEPTH);
    wr(c, NVME_AQA, ((ADMIN_DEPTH - 1u) << 16) | (ADMIN_DEPTH - 1u));
    wr(c, NVME_ASQ, kphys(m->asq));
    wr(c, NVME_ASQ + 4, 0);
    wr(c, NVME_ACQ, kphys(m->acq));
    wr(c, NVME_ACQ + 4, 0);
    wr(c, NVME_INTMS, 0xFFFFFFFFu);             /* polled: mask every vector */
    /* NVM command set (CSS 0), 4 KiB pages (MPS 0), round robin (AMS 0). */
    wr(c, NVME_CC, CC_IOSQES | CC_IOCQES | CC_EN);
    if (wait_csts(c, CSTS_RDY | CSTS_CFS, CSTS_RDY, c->timeout_ms) < 0) {
        printk("[NVMe] controller does not become ready (CSTS=0x%08x)\n",
               (unsigned)rd(c, NVME_CSTS));
        return -1;
    }

    nvme_sqe_t cmd;
    /* One I/O submission and one completion queue. */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = ADM_SET_FEATURES;
    cmd.cdw10 = FEAT_NUM_QUEUES;
    cmd.cdw11 = 0;                              /* NCQR = NSQR = 0 (one each) */
    if (queue_exec(c, &c->admin, &cmd, c->timeout_ms, 0) != 0)
        return -1;

    queue_init(c, &c->io, 1, m->iosq, m->iocq, c->io_depth);
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = ADM_CREATE_CQ;
    cmd.prp1[0] = kphys(m->iocq);
    cmd.cdw10 = ((uint32_t)(c->io_depth - 1u) << 16) | 1u;
    cmd.cdw11 = 1u;                             /* PC, IEN = 0 */
    if (queue_exec(c, &c->admin, &cmd, c->timeout_ms, 0) != 0)
        return -1;

    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = ADM_CREATE_SQ;
    cmd.prp1[0] = kphys(m->iosq);
    cmd.cdw10 = ((uint32_t)(c->io_depth - 1u) << 16) | 1u;
    cmd.cdw11 = (1u << 16) | 1u;                /* CQID 1, PC */
    if (queue_exec(c, &c->admin, &cmd, c->timeout_ms, 0) != 0)
        return -1;
    return 0;
}

static void id_string(const uint8_t *s, int len, char *out) {
    int n = 0;
    for (int i = 0; i < len; i++)
        out[n++] = (char)s[i];
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0))
        n--;
    out[n] = '\0';
}

/* IDENTIFY one namespace and register it as a disk if it is usable. */
static void ns_probe(nvme_ctrl_t *c, int ctrl, uint32_t nsid) {
    if (admin_identify(c, CNS_NAMESPACE, nsid) != 0) {
        printk("[NVMe] ctrl %d: IDENTIFY namespace %u failed\n", ctrl, (unsigned)nsid);
        return;
    }
    const uint8_t *id = nvme_bounce;
    uint64_t nsze;
    memcpy(&nsze, id + 0, 8);
    if (!nsze)
        return;                                 /* inactive */
    uint8_t  flbas = id[26] & 0x0Fu;
    uint32_t lbaf;
    memcpy(&lbaf, id + 128 + 4u * flbas, 4);
    uint32_t ms = lbaf & 0xFFFFu, lbads = (lbaf >> 16) & 0xFFu;
    if (lbads != 9 || ms != 0) {
        printk("[NVMe] ctrl %d ns %u: %u-byte blocks with %u bytes metadata, "
               "not supported\n", ctrl, (unsigned)nsid,
               lbads < 32 ? (unsigned)(1u << lbads) : 0u, (unsigned)ms);
        return;
    }
    if (ndisks >= NVME_MAX_DISKS) {
        printk("[NVMe] ctrl %d ns %u: disk table full, ignoring\n", ctrl, (unsigned)nsid);
        return;
    }
    nvme_disk_t *d = &disks[ndisks];
    d->ctrl = c;
    d->nsid = nsid;
    d->sectors = nsze;
    printk("[NVMe] disk %d: controller %d namespace %u, %u MiB\n",
           ndisks, ctrl, (unsigned)nsid, (unsigned)(nsze >> 11));
    ndisks++;
}

static void nvme_init_ctrl(const pci_device_t *pd, int ctrl) {
    uint32_t bar0 = pd->bar[0];
    if ((bar0 & 1u) || !(bar0 & ~0xFu)) {
        printk("[NVMe] %02x:%02x.%u: no memory BAR0, skipping\n",
               (unsigned)pd->bus, (unsigned)pd->slot, (unsigned)pd->func);
        return;
    }
    /* A 64-bit BAR placed above 4 GiB is out of reach of 32-bit paging. */
    if (((bar0 >> 1) & 3u) == 2u && pd->bar[1]) {
        printk("[NVMe] %02x:%02x.%u: registers above 4 GiB, skipping\n",
               (unsigned)pd->bus, (unsigned)pd->slot, (unsigned)pd->func);
        return;
    }
    uint32_t phys = bar0 & ~0xFu;

    uint32_t pcmd = pci_read_config32(pd->bus, pd->slot, pd->func, 0x04);
    /* Memory space + bus master; INTx disabled (the driver polls). */
    pci_write_config32(pd->bus, pd->slot, pd->func, 0x04,
                       (pcmd & 0xFFFFu) | 0x0006u | 0x0400u);

    /* The window starts at the page holding the BAR, so the doorbell
     * check below keeps measuring from that page. */
    volatile uint8_t *regs = mmio_map(phys & ~0xFFFu, NVME_MMIO_SIZE);
    if (!regs) {
        printk("[NVMe] cannot map the register window\n");
        return;
    }

    nvme_ctrl_t *c = &ctrls[ctrl];
    memset(c, 0, sizeof(*c));
    c->regs = regs + (phys & 0xFFFu);
    c->mem = &ctrl_mem[ctrl];

    uint32_t cap_lo = rd(c, NVME_CAP), cap_hi = rd(c, NVME_CAP + 4);
    uint32_t vs = rd(c, NVME_VS);
    /* Our doorbells (QID 0 and 1) must fall inside the mapped window. */
    if (!CAP_CSS_NVM(cap_hi) || CAP_MPSMIN(cap_hi) != 0 ||
        NVME_DBS + 4u * (4u << CAP_DSTRD(cap_hi)) > NVME_MMIO_SIZE - (phys & 0xFFFu)) {
        printk("[NVMe] %02x:%02x.%u: unsupported controller (CAP=0x%08x%08x)\n",
               (unsigned)pd->bus, (unsigned)pd->slot, (unsigned)pd->func,
               (unsigned)cap_hi, (unsigned)cap_lo);
        return;
    }
    c->timeout_ms = CAP_TO(cap_lo) ? CAP_TO(cap_lo) * 500u : 500u;
    uint32_t mqes = CAP_MQES(cap_lo) + 1u;
    c->io_depth = (uint16_t)(mqes < IO_DEPTH ? mqes : IO_DEPTH);

    if (ctrl_enable(c) < 0) {
        printk("[NVMe] %02x:%02x.%u: controller skipped\n",
               (unsigned)pd->bus, (unsigned)pd->slot, (unsigned)pd->func);
        return;
    }

    if (admin_identify(c, CNS_CONTROLLER, 0) != 0) {
        printk("[NVMe] controller %d: IDENTIFY controller failed\n", ctrl);
        return;
    }
    const uint8_t *id = nvme_bounce;
    char sn[21], mn[41], fr[9];
    id_string(id + 4, 20, sn);
    id_string(id + 24, 40, mn);
    id_string(id + 64, 8, fr);
    uint8_t mdts = id[77];
    uint32_t nn;
    memcpy(&nn, id + 516, 4);
    c->vwc = id[525] & 1u;
    c->max_pages = (mdts && mdts < 16) ? (1u << mdts) : 0;
    c->ok = 1;

    printk("[NVMe] controller %d at %02x:%02x.%u (%04x:%04x), NVMe %u.%u, "
           "\"%s\" fw %s sn %s, %u namespace(s)%s\n",
           ctrl, (unsigned)pd->bus, (unsigned)pd->slot, (unsigned)pd->func,
           (unsigned)pd->vendor_id, (unsigned)pd->device_id,
           (unsigned)(vs >> 16), (unsigned)((vs >> 8) & 0xFF), mn, fr, sn,
           (unsigned)nn, c->vwc ? ", write cache" : "");

    /* NVMe 1.1+ lists the active namespace IDs (CNS 02h, up to 1024 per
     * page); before that every ID from 1 to NN is probed. */
    if (vs >= 0x00010100u) {
        if (admin_identify(c, CNS_ACTIVE_NSIDS, 0) != 0) {
            printk("[NVMe] controller %d: namespace list failed\n", ctrl);
            return;
        }
        static uint32_t list[PAGE / 4];         /* ns_probe reuses the bounce */
        memcpy(list, nvme_bounce, sizeof(list));
        for (uint32_t i = 0; i < PAGE / 4 && list[i]; i++)
            ns_probe(c, ctrl, list[i]);
    } else {
        for (uint32_t nsid = 1; nsid <= nn && nsid <= 1024; nsid++)
            ns_probe(c, ctrl, nsid);
    }
}

void nvme_init(void) {
    spin_init(&nvme_lock);
    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *pd = pci_get_device(i);
        if (pd->class_code != 0x01 || pd->subclass != 0x08 || pd->prog_if != 0x02)
            continue;
        if (nctrls >= NVME_MAX_CTRL) {
            printk("[NVMe] more than %d controllers, ignoring the rest\n", NVME_MAX_CTRL);
            break;
        }
        nvme_init_ctrl(pd, nctrls++);
    }
    if (nctrls)
        printk("[NVMe] %d disk(s) on %d controller(s)\n", ndisks, nctrls);
}

int nvme_disk_count(void) {
    return ndisks;
}

uint32_t nvme_disk_sectors(int disk) {
    if (disk < 0 || disk >= ndisks)
        return 0;
    return disks[disk].sectors > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                               : (uint32_t)disks[disk].sectors;
}

/* One I/O command on `d` for `count` sectors at `off` in the bounce buffer.  0, -1 (error status),
 * -2 (timeout). */
static int io_exec(nvme_disk_t *d, uint8_t opc, uint64_t lba, uint32_t count,
                   uint32_t off) {
    nvme_ctrl_t *c = d->ctrl;
    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = opc;
    cmd.nsid = d->nsid;
    if (opc != NVM_FLUSH) {
        uint32_t pages = (count * 512u + PAGE - 1u) / PAGE;
        c->n_io++;
        if (pages > c->max_io_pages)
            c->max_io_pages = pages;
        set_prps(c, &cmd, off, count * 512u);
        cmd.cdw10 = (uint32_t)lba;
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = count - 1u;                 /* NLB, 0-based */
    }
    uint32_t timeout = opc == NVM_FLUSH ? 30000u : 10000u;
    if (timeout < c->timeout_ms)
        timeout = c->timeout_ms;
    return queue_exec(c, &c->io, &cmd, timeout, 0);
}

static int nvme_rw(int disk, uint32_t lba, uint32_t count, void *buf, int write) {
    if (disk < 0 || disk >= ndisks || count == 0 || count > 256)
        return -1;
    nvme_disk_t *d = &disks[disk];
    nvme_ctrl_t *c = d->ctrl;
    if ((uint64_t)lba + count > d->sectors)
        return -1;

    uint32_t irq = nvme_irq_save();
    spin_lock(&nvme_lock);
    if (write)
        memcpy(nvme_bounce, buf, count * 512u);
    /* Split at MDTS (a whole number of pages, so every chunk starts on a
     * page boundary of the bounce buffer). */
    uint32_t chunk = c->max_pages ? c->max_pages * (PAGE / 512u) : count;
    int rc = c->ok ? 0 : -1;
    for (uint32_t done = 0; rc == 0 && done < count; ) {
        uint32_t n = count - done < chunk ? count - done : chunk;
        if (n < count)
            c->n_split++;
        rc = -1;
        for (int attempt = 0; attempt < 2 && rc != 0; attempt++) {
            rc = io_exec(d, write ? NVM_WRITE : NVM_READ, (uint64_t)lba + done, n,
                         done * 512u);
            if (rc == 0 && write && c->vwc)
                rc = io_exec(d, NVM_FLUSH, 0, 0, 0);
            if (rc == -2) {
                printk("[NVMe] nsid %u: command timed out at LBA %u, resetting "
                       "the controller\n", (unsigned)d->nsid, (unsigned)(lba + done));
                if (ctrl_enable(c) < 0) {
                    printk("[NVMe] controller reset failed, giving up on it\n");
                    c->ok = 0;
                    break;
                }
            }
        }
        done += n;
    }
    if (rc == 0 && !write)
        memcpy(buf, nvme_bounce, count * 512u);
    spin_unlock(&nvme_lock);
    nvme_irq_restore(irq);
    return rc == 0 ? 0 : -1;
}

int nvme_read(int disk, uint32_t lba, uint32_t count, void *buf) {
    return nvme_rw(disk, lba, count, buf, 0);
}

int nvme_write(int disk, uint32_t lba, uint32_t count, const void *buf) {
    return nvme_rw(disk, lba, count, (void *)buf, 1);
}

void nvme_shutdown(void) {
    uint32_t irq = nvme_irq_save();
    spin_lock(&nvme_lock);
    for (int i = 0; i < nctrls; i++) {
        nvme_ctrl_t *c = &ctrls[i];
        if (!c->ok)
            continue;
        wr(c, NVME_CC, (rd(c, NVME_CC) & ~CC_SHN_MASK) | CC_SHN_NORMAL);
        if (wait_csts(c, CSTS_SHST_MASK, CSTS_SHST_DONE, 2000) < 0)
            printk("[NVMe] controller %d: shutdown did not complete\n", i);
        else
            printk("[NVMe] controller %d shut down: %u I/O commands, %u with a "
                   "PRP list, %u MDTS chunks, largest %u pages\n", i,
                   (unsigned)c->n_io, (unsigned)c->n_prp_list,
                   (unsigned)c->n_split, (unsigned)c->max_io_pages);
        c->ok = 0;
    }
    spin_unlock(&nvme_lock);
    nvme_irq_restore(irq);
}
