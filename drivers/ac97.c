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
static volatile int drop_req;                     /* ac97_drop() -> pump() */
static int drop_chan;                             /* its sleep channel */

/* PCM byte ring fed by /dev/dsp writes, drained into DMA buffers. */
#define RING_BYTES (256 * 1024)
static uint8_t ring[RING_BYTES];
static volatile uint32_t ring_head, ring_tail;   /* head=write, tail=read */
static int ring_waiters;                          /* writer sleep channel */
/* One /dev/dsp write at a time.  ring_head has a single producer by design:
 * two writers preempted between reading it and advancing it copied over each
 * other's samples and lost a head update.  Holding this across the whole
 * write also keeps each write()'s PCM contiguous in the stream. */
static int writer_busy;

static uint16_t nam_base, nabm_base;
static int present;
static volatile int playing;
static volatile uint32_t irq_count;

/*
 * The 32 DMA buffers form one cyclic buffer that the engine plays round and
 * round (LVI is kept just behind CIV, so it never reaches the end of the
 * list), exactly like the HDA driver's.  Positions are absolute byte counts
 * since the engine started: done_bufs buffers have completed, the engine is
 * PICB samples from the end of the current one (play_abs), and real PCM is
 * written up to write_abs.  Played space is zeroed at once, so stale PCM
 * never comes round again; PCM always continues where the last write ended,
 * however little arrived in between (refilling whole just-played buffers
 * instead left silent buffers between a slow writer's chunks).  After an
 * underrun, writing resumes RESYNC_LEAD bytes ahead of the engine.
 */
#define CYC_BYTES    (NUM_BUFS * BUF_BYTES)
#define WRITE_GUARD  1024
#define RESYNC_LEAD  4096
#define START_BYTES  (32 * 1024)
static uint8_t *const cyc = &dma_buf[0][0];
static uint32_t done_bufs, play_abs, write_abs, underruns;
/* Positions are byte counts that wrap at 2^32 (6.2 h of 48 kHz stereo), so
 * they are only ever compared through their signed difference.  A stream's
 * positions start at AUDIO_POS_START; `make AUDIO_POS_START=0xFFFE0000U`
 * starts them 128 KiB before the wrap, so every sound crosses it (test). */
#ifndef AUDIO_POS_START
#define AUDIO_POS_START 0U
#endif
#define POS_LT(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) < 0)
_Static_assert(AUDIO_POS_START % CYC_BYTES == 0, "positions start at a cyclic-buffer boundary");
static uint8_t last_civ;

static uint32_t virt_to_phys(const void *p) {
    return (uint32_t)((uintptr_t)p - KERNEL_VMA);
}

static uint32_t ring_used(void) {
    return ring_head - ring_tail;
}

/* Copy ring bytes into the cyclic buffer at write_abs, up to `limit`. */
static void copy_in(uint32_t limit) {
    uint32_t n = ring_used();

    if (!POS_LT(write_abs, limit)) return;
    if (n > limit - write_abs) n = limit - write_abs;
    for (uint32_t i = 0; i < n; i++)
        cyc[(write_abs + i) % CYC_BYTES] = ring[(ring_tail + i) % RING_BYTES];
    ring_tail += n;
    write_abs += n;
    if (n && ring_waiters)
        wake_up(&ring_waiters);
}

/* Zero [from, to) (absolute) in the cyclic buffer. */
static void zero_range(uint32_t from, uint32_t to) {
    while (POS_LT(from, to)) {
        uint32_t o = from % CYC_BYTES;
        uint32_t n = CYC_BYTES - o;
        if (n > to - from) n = to - from;
        memset(cyc + o, 0, n);
        from += n;
    }
}

/* Advance play_abs from CIV/PICB and silence what was played. */
static void update_play(void) {
    uint8_t civ;
    uint16_t picb;
    uint32_t old = play_abs, pos;

    do {
        civ = inb((uint16_t)(nabm_base + PO_CIV));
        picb = inw((uint16_t)(nabm_base + PO_PICB));
    } while (civ != inb((uint16_t)(nabm_base + PO_CIV)));
    civ %= NUM_BUFS;
    done_bufs += (uint32_t)(civ - last_civ + NUM_BUFS) % NUM_BUFS;
    last_civ = civ;
    pos = (uint32_t)picb * 2;
    if (pos > BUF_BYTES) pos = BUF_BYTES;
    pos = done_bufs * BUF_BYTES + (BUF_BYTES - pos);
    if (POS_LT(play_abs, pos)) play_abs = pos;
    zero_range(old, play_abs);
}

/* Advance playback: refill the cyclic buffer, start/stop the engine. */
static void pump(void) {
    static uint32_t idle_head;

    if (!present) return;

    if (drop_req) {
        /* Everything queued goes: the ring, and the PCM ahead of the engine
         * (bar the guard bytes it may have fetched already). */
        ring_tail = ring_head;
        if (playing && POS_LT(play_abs + WRITE_GUARD, write_abs)) {
            zero_range(play_abs + WRITE_GUARD, write_abs);
            write_abs = play_abs + WRITE_GUARD;
        }
        drop_req = 0;
        wake_up(&drop_chan);
    }

    if (!playing) {
        uint32_t used = ring_used();
        /* start with a cushion, or with whatever there is once the writer
         * has gone quiet for a pump interval (a short sound) */
        int quiet = ring_head == idle_head;
        idle_head = ring_head;
        if (used == 0 || (used < START_BYTES && !quiet)) return;

        outb((uint16_t)(nabm_base + PO_CR), CR_RR);   /* CIV back to 0 */
        {
            int spin = 100000;
            while ((inb((uint16_t)(nabm_base + PO_CR)) & CR_RR) && --spin) {}
        }
        memset(cyc, 0, CYC_BYTES);
        for (int i = 0; i < NUM_BUFS; i++) {
            bdl[i].addr = virt_to_phys(dma_buf[i]);
            bdl[i].samples = BUF_BYTES / 2;       /* 16-bit samples */
            bdl[i].flags = 0x8000;                /* IOC */
        }
        done_bufs = AUDIO_POS_START / BUF_BYTES;  /* CIV 0 */
        play_abs = AUDIO_POS_START;
        last_civ = 0;
        write_abs = AUDIO_POS_START + (ring_tail & 3);   /* frame alignment */
        copy_in(play_abs + CYC_BYTES - BUF_BYTES);
        outl((uint16_t)(nabm_base + PO_BDBAR), virt_to_phys(bdl));
        outb((uint16_t)(nabm_base + PO_LVI), NUM_BUFS - 1);
        outb((uint16_t)(nabm_base + PO_CR), CR_RPBM | CR_IOCE);
        playing = 1;
        printk("[AC97] engine start\n");
        return;
    }

    update_play();
    if (POS_LT(write_abs, play_abs) && ring_used()) {
        underruns++;
        write_abs = play_abs + RESYNC_LEAD;
        write_abs += (ring_tail - write_abs) & 3;   /* keep frames aligned */
    }
    if (POS_LT(write_abs, play_abs + WRITE_GUARD) && ring_used())
        write_abs += (play_abs + WRITE_GUARD - write_abs + 3) & ~3U;
    /* up to the end of the buffer before the current one */
    copy_in((done_bufs + NUM_BUFS - 1) * BUF_BYTES);
    outb((uint16_t)(nabm_base + PO_LVI), (uint8_t)((last_civ + NUM_BUFS - 1) % NUM_BUFS));

    if (ring_used() == 0 && !POS_LT(play_abs, write_abs)) {
        outb((uint16_t)(nabm_base + PO_CR), 0);
        outw((uint16_t)(nabm_base + PO_SR), SR_LVBCI | SR_BCIS | SR_FIFOE);
        playing = 0;
        printk("[AC97] playback done (%u irqs, %u underruns)\n",
               (unsigned)irq_count, (unsigned)underruns);
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

/* PCM bytes written but not yet played: the ring plus the PCM ahead of the
 * engine in the cyclic buffer (as of the last pump, <= 20 ms old). */
uint32_t ac97_queued(void) {
    uint32_t q;

    if (!present) return 0;
    q = ring_used();
    if (playing && POS_LT(play_abs, write_abs))
        q += write_abs - play_abs;
    return q;
}

/* Throw away everything queued (ALSA drop); returns once ksoundd has. */
void ac97_drop(void) {
    if (!present) return;
    drop_req = 1;
    io_wake();
    while (drop_req) {
        current_proc->wake_tick = pit_ticks() + 1;
        sleep_on(&drop_chan);
    }
}

/* Blocking PCM write (48kHz S16LE stereo).  EINTR-aware like the pipes. */
int ac97_write(const uint8_t *data, uint32_t len) {
    uint32_t written = 0;

    if (!present) return -19;   /* -ENODEV */
    for (;;) {
        preempt_disable();
        int got = !writer_busy;
        if (got) writer_busy = 1;
        preempt_enable();
        if (got) break;
        if (signal_interrupt_pending(current_proc))
            return -4;
        current_proc->wake_tick = pit_ticks() + 5;
        sleep_on(&writer_busy);
    }

    while (written < len) {
        uint32_t space = RING_BYTES - ring_used();

        if (space == 0) {
            if (signal_interrupt_pending(current_proc))
                break;
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

    writer_busy = 0;
    wake_up(&writer_busy);
    return written ? (int)written : (len ? -4 : 0);
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

    /* Interrupt Line is 0xFF when firmware routed no IRQ; unmasking it would
     * shift by 247 (undefined) — polling via ksoundd's safety tick still runs. */
    if (dev->irq_line >= 1 && dev->irq_line <= 15) {
        irq_install_handler((int)dev->irq_line, (void *)ac97_irq);
        pic_unmask(dev->irq_line);
    }

    present = 1;
    printk("[AC97] up: nam=0x%x nabm=0x%x irq=%u\n",
           (unsigned)nam_base, (unsigned)nabm_base, (unsigned)dev->irq_line);
}
