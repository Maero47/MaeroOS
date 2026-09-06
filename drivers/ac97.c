#include "ac97.h"
#include "pci.h"
#include "../arch/i686/include/io.h"
#include "../arch/i686/cpu/irq.h"
#include "../arch/i686/cpu/pic.h"
#include "../arch/i686/include/registers.h"
#include "../kernel/printk.h"
#include "../proc/scheduler.h"
#include "../proc/process.h"
#include "../proc/signal.h"
#include "../arch/i686/cpu/pit.h"
#include "../lib/string.h"
#include <kernel/config.h>
#include <stdint.h>

/*
 * AC'97 (Intel 82801AA, QEMU `-device AC97`) PCM output driver.
 *
 * NAM (mixer) and NABM (bus master) are I/O BARs 0 and 1.  PCM out runs a
 * 32-entry Buffer Descriptor List of 4 KiB DMA buffers at a fixed 48 kHz
 * S16LE stereo.  The ISR only acks + wakes (the RTL8139 lesson); refill
 * happens in process context from a byte ring fed by /dev/dsp writes.
 */

#define AC97_VENDOR 0x8086
#define AC97_DEVICE 0x2415

/* NAM (mixer) registers */
#define NAM_RESET        0x00
#define NAM_MASTER_VOL   0x02
#define NAM_PCM_VOL      0x18

/* NABM (bus master) — PCM OUT box at 0x10 */
#define PO_BDBAR 0x10    /* descriptor list base (u32) */
#define PO_CIV   0x14    /* current index (u8, RO) */
#define PO_LVI   0x15    /* last valid index (u8) */
#define PO_SR    0x16    /* status (u16, write-1-clear) */
#define PO_PICB  0x18    /* samples left in current buffer (u16) */
#define PO_CR    0x1B    /* control (u8) */
#define GLOB_CNT 0x2C    /* global control (u32) */
#define GLOB_STA 0x30    /* global status (u32) */

#define CR_RPBM  0x01    /* run */
#define CR_RR    0x02    /* reset registers */
#define CR_IOCE  0x10    /* interrupt-on-completion enable */

#define SR_DCH   0x01    /* DMA halted */
#define SR_LVBCI 0x04    /* last valid buffer completion */
#define SR_BCIS  0x08    /* buffer completion */
#define SR_FIFOE 0x10    /* FIFO error */

#define NUM_BUFS 32
#define BUF_BYTES 4096

/* BDL entry: physical pointer + length in SAMPLES + flags */
typedef struct {
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;       /* bit15 = IOC */
} __attribute__((packed)) bdl_entry_t;

static bdl_entry_t bdl[NUM_BUFS] __attribute__((aligned(8)));
static uint8_t dma_buf[NUM_BUFS][BUF_BYTES] __attribute__((aligned(4)));

/* PCM byte ring fed by /dev/dsp writes, drained into DMA buffers. */
#define RING_BYTES (256 * 1024)
static uint8_t ring[RING_BYTES];
static volatile uint32_t ring_head, ring_tail;   /* head=write, tail=read */
static int ring_waiters;                          /* writer sleep channel */

static uint16_t nam_base, nabm_base;
static int present;
static volatile int playing;
static uint8_t next_buf;                          /* next BDL slot to fill */
static uint8_t last_filled;                       /* LVI target (has data) */
static volatile uint32_t irq_count;

static uint32_t virt_to_phys(const void *p) {
    return (uint32_t)((uintptr_t)p - KERNEL_VMA);
}

static uint32_t ring_used(void) {
    return ring_head - ring_tail;
}

/* Fill one DMA buffer from the ring (zero-pad a partial tail). */
static void fill_buf(int idx) {
    uint32_t avail = ring_used();
    uint32_t n = avail > BUF_BYTES ? BUF_BYTES : avail;

    for (uint32_t i = 0; i < n; i++)
        dma_buf[idx][i] = ring[(ring_tail + i) % RING_BYTES];
    ring_tail += n;
    if (n < BUF_BYTES)
        memset(dma_buf[idx] + n, 0, BUF_BYTES - n);
    bdl[idx].addr = virt_to_phys(dma_buf[idx]);
    bdl[idx].samples = BUF_BYTES / 2;       /* 16-bit samples */
    bdl[idx].flags = 0x8000;                /* IOC */
    if (ring_waiters)
        wake_up(&ring_waiters);
}

/* Advance playback: refill completed buffers, start/stop the engine. */
static void pump(void) {
    if (!present) return;

    if (!playing) {
        int primed = 0;

        if (ring_used() == 0) return;
        /* Prime only buffers that have data; LVI marks the last one. */
        next_buf = 0;
        while (primed < NUM_BUFS && ring_used() > 0) {
            fill_buf(primed);
            last_filled = (uint8_t)primed;
            primed++;
        }
        next_buf = 0;
        outl((uint16_t)(nabm_base + PO_BDBAR), virt_to_phys(bdl));
        outb((uint16_t)(nabm_base + PO_LVI), last_filled);
        outb((uint16_t)(nabm_base + PO_CR), CR_RPBM | CR_IOCE);
        playing = 1;
        printk("[AC97] engine start (lvi=%u)\n", (unsigned)last_filled);
        return;
    }

    /* Refill buffers the engine has finished, while there is data.
     * Stale buffers (no fresh data) are zeroed so they can never replay. */
    {
        uint8_t civ = inb((uint16_t)(nabm_base + PO_CIV));
        while (next_buf != civ) {
            if (ring_used() > 0) {
                fill_buf(next_buf);
                last_filled = next_buf;
            } else {
                memset(dma_buf[next_buf], 0, BUF_BYTES);
            }
            next_buf = (uint8_t)((next_buf + 1) % NUM_BUFS);
        }
        outb((uint16_t)(nabm_base + PO_LVI), last_filled);

        /* QEMU's engine does not reliably halt at LVI — stop ourselves
         * once the last data buffer has completed (civ just past it). */
        if (ring_used() == 0) {
            uint8_t delta = (uint8_t)((civ - last_filled + NUM_BUFS)
                                      % NUM_BUFS);
            if (delta >= 1 && delta <= 8) {
                outb((uint16_t)(nabm_base + PO_CR), 0);
                outw((uint16_t)(nabm_base + PO_SR),
                     SR_LVBCI | SR_BCIS | SR_FIFOE);
                playing = 0;
                printk("[AC97] playback done (%u irqs)\n",
                       (unsigned)irq_count);
            }
        }
    }
}

static void ac97_irq(registers_t *regs) {
    (void)regs;
    uint16_t sr = inw((uint16_t)(nabm_base + PO_SR));
    /* ack everything (write-1-clear), wake the pump kthread */
    outw((uint16_t)(nabm_base + PO_SR), sr & (SR_LVBCI | SR_BCIS | SR_FIFOE));
    irq_count++;
    io_wake();
}

/* Sound bottom-half: refills DMA buffers; woken by the ISR / writers. */
static void ksoundd(void) {
    for (;;) {
        preempt_disable();
        pump();
        preempt_enable();
        current_proc->wake_tick = pit_ticks() + 2;   /* 20ms safety tick */
        sleep_on(&io_activity);
    }
}

/* ── /dev/dsp backend ──────────────────────────────────────────────────── */

int ac97_present(void) {
    return present;
}

/* Spawn the refill kthread — must run AFTER proc_init() (which wipes the
 * process table; creating the thread inside ac97_init would lose it). */
void ac97_start_thread(void) {
    if (present)
        proc_create_kthread(ksoundd, "ksoundd");
}

uint32_t ac97_irq_count(void) {
    return irq_count;
}

/* Blocking PCM write (48kHz S16LE stereo).  EINTR-aware like the pipes. */
int ac97_write(const uint8_t *data, uint32_t len) {
    uint32_t written = 0;

    if (!present) return -19;   /* -ENODEV */
    while (written < len) {
        uint32_t space = RING_BYTES - ring_used();

        if (space == 0) {
            if (signal_interrupt_pending(current_proc))
                return written ? (int)written : -4;
            ring_waiters = 1;
            current_proc->wake_tick = pit_ticks() + 5;
            sleep_on(&ring_waiters);
            ring_waiters = 0;
            continue;
        }
        {
            uint32_t n = len - written;
            if (n > space) n = space;
            for (uint32_t i = 0; i < n; i++)
                ring[(ring_head + i) % RING_BYTES] = data[written + i];
            ring_head += n;
            written += n;
        }
        io_wake();             /* kick ksoundd to start/refill */
    }
    return (int)written;
}

void ac97_init(void) {
    const pci_device_t *dev = pci_find_device(AC97_VENDOR, AC97_DEVICE);

    if (!dev) {
        printk("[AC97] not present\n");
        return;
    }
    nam_base  = (uint16_t)(dev->bar[0] & ~0x3U);
    nabm_base = (uint16_t)(dev->bar[1] & ~0x3U);

    {   /* enable I/O + bus mastering */
        uint32_t cmd = pci_read_config32(dev->bus, dev->slot, dev->func, 0x04);
        pci_write_config32(dev->bus, dev->slot, dev->func, 0x04,
                           cmd | 0x00000005U);
    }

    outl((uint16_t)(nabm_base + GLOB_CNT), 0x2);    /* cold reset deassert */
    outw((uint16_t)(nam_base + NAM_RESET), 0);      /* mixer reset */
    outw((uint16_t)(nam_base + NAM_MASTER_VOL), 0x0000);  /* 0dB attenuation */
    outw((uint16_t)(nam_base + NAM_PCM_VOL), 0x0808);

    outb((uint16_t)(nabm_base + PO_CR), CR_RR);     /* reset PCM-out box */
    {
        int spin = 100000;
        while ((inb((uint16_t)(nabm_base + PO_CR)) & CR_RR) && --spin) {}
    }

    irq_install_handler((int)dev->irq_line, (void *)ac97_irq);
    pic_unmask(dev->irq_line);

    present = 1;
    printk("[AC97] up: nam=0x%x nabm=0x%x irq=%u\n",
           (unsigned)nam_base, (unsigned)nabm_base, (unsigned)dev->irq_line);
}
