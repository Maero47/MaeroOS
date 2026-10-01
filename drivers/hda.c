#include "hda.h"
#include "pci.h"
#include "../arch/i686/include/io.h"
#include "../arch/i686/cpu/irq.h"
#include "../arch/i686/cpu/pic.h"
#include "../arch/i686/cpu/spinlock.h"
#include "../arch/i686/include/registers.h"
#include "../kernel/printk.h"
#include "../mm/mmio.h"
#include "../proc/scheduler.h"
#include "../proc/process.h"
#include "../proc/signal.h"
#include "../arch/i686/cpu/pit.h"
#include "../lib/string.h"
#include <kernel/config.h>
#include <stdint.h>

/*
 * Intel High Definition Audio controller + codec driver, PCM out only.
 *
 * Controller (Intel HDA spec rev 1.0a, section 3): BAR0 is a 16 KiB MMIO
 * window.  Codec verbs go out through the CORB and come back through the
 * RIRB, both polled (the immediate command registers are the fallback when
 * the rings do not answer).  Codec (section 7): the first audio function
 * group is walked; every output pin whose configuration default says line
 * out / speaker / headphone and that is not "no connection" gets a path
 * pin -> (selector|mixer)* -> DAC found by depth-first search over the
 * connection lists, powered, unmuted and routed.  All chosen DACs listen on
 * the same stream tag, so one output stream feeds every output.
 *
 * Playback mirrors the AC'97 driver: /dev/dsp writes fill a byte ring, and a
 * kthread (khdad) moves the ring into a cyclic DMA buffer of NUM_BUFS x
 * BUF_BYTES described by one BDL.  HDA DMA never stops at the end of the
 * list, so the refill tracks absolute byte positions from LPIB, keeps the
 * whole cyclic buffer ahead of the play position, pads with silence and
 * stops the stream once the last real byte has played.  The ISR only acks
 * and wakes.  Fixed 48 kHz S16LE stereo, the stream /dev/dsp carries.
 *
 * MMIO comes from the shared kernel MMIO window (mm/mmio.c).
 */

/* ── controller registers ─────────────────────────────────────────────── */
#define GCAP      0x00   /* u16 */
#define VMIN      0x02
#define VMAJ      0x03
#define GCTL      0x08   /* u32, bit0 CRST */
#define STATESTS  0x0E   /* u16, codec present bits, write-1-clear */
#define INTCTL    0x20   /* u32 */
#define INTSTS    0x24   /* u32 */
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48   /* u16 */
#define CORBRP    0x4A   /* u16, bit15 reset */
#define CORBCTL   0x4C   /* u8, bit1 run */
#define CORBSIZE  0x4E   /* u8 */
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58   /* u16, bit15 reset */
#define RINTCNT   0x5A   /* u16 */
#define RIRBCTL   0x5C   /* u8, bit1 run */
#define RIRBSTS   0x5D   /* u8, write-1-clear */
#define RIRBSIZE  0x5E   /* u8 */
#define ICOI      0x60   /* immediate command out */
#define IRII      0x64   /* immediate response in */
#define ICS       0x68   /* u16: bit0 busy, bit1 result valid */

#define GCTL_CRST   0x1U
#define INTCTL_GIE  0x80000000U
#define RING_RUN    0x02

/* stream descriptor registers, relative to the descriptor base */
#define SD_CTL   0x00    /* u32 view: bits 0..23 CTL, 24..31 STS */
#define SD_STS   0x03    /* u8, write-1-clear */
#define SD_LPIB  0x04
#define SD_CBL   0x08
#define SD_LVI   0x0C    /* u16 */
#define SD_FMT   0x12    /* u16 */
#define SD_BDPL  0x18
#define SD_BDPU  0x1C

#define SD_CTL_SRST 0x01U
#define SD_CTL_RUN  0x02U
#define SD_CTL_IOCE 0x04U
#define SD_STS_BCIS 0x04
#define SD_STS_FIFOE 0x08
#define SD_STS_DESE 0x10

/* ── codec verbs / parameters ─────────────────────────────────────────── */
#define V_GET_PARAM      0xF00
#define V_GET_CONN_LIST  0xF02
#define V_SET_CONN_SEL   0x701
#define V_SET_POWER      0x705
#define V_SET_STREAM     0x706
#define V_SET_PIN_CTL    0x707
#define V_SET_EAPD       0x70C
#define V_GET_CFG_DEF    0xF1C
#define V4_SET_FORMAT    0x2      /* 4-bit verbs carry a 16-bit payload */
#define V4_SET_AMP       0x3

#define P_VENDOR_ID      0x00
#define P_SUB_NODES      0x04
#define P_FG_TYPE        0x05
#define P_WIDGET_CAPS    0x09
#define P_PIN_CAPS       0x0C
#define P_IN_AMP_CAPS    0x0D
#define P_CONN_LEN       0x0E
#define P_OUT_AMP_CAPS   0x12

#define WT_OUTPUT   0x0           /* audio output converter (DAC) */
#define WT_MIXER    0x2
#define WT_SELECTOR 0x3
#define WT_PIN      0x4

#define WCAP_IN_AMP   (1U << 1)
#define WCAP_OUT_AMP  (1U << 2)
#define WCAP_AMP_OVRD (1U << 3)
#define WCAP_CONN     (1U << 8)

#define PINCAP_OUT    (1U << 4)
#define PINCAP_HP     (1U << 3)
#define PINCAP_EAPD   (1U << 16)

#define PIN_OUT_EN 0x40
#define PIN_HP_EN  0x80

/* 48 kHz base, x1, /1, 16 bits, 2 channels */
#define STREAM_FMT 0x0011
#define STREAM_TAG 1

/* ── DMA memory (kernel .bss is physically contiguous: phys = virt - VMA) ─ */
#define NUM_BUFS  32
#define BUF_BYTES 4096
#define CYC_BYTES (NUM_BUFS * BUF_BYTES)

typedef struct {
    uint32_t addr_lo, addr_hi, len, flags;   /* flags bit0 = IOC */
} __attribute__((packed)) hda_bdl_t;

static uint32_t corb[256] __attribute__((aligned(1024)));
static volatile uint32_t rirb[512] __attribute__((aligned(2048)));  /* resp, ext */
static hda_bdl_t bdl[NUM_BUFS] __attribute__((aligned(128)));
static uint8_t cyc[CYC_BYTES] __attribute__((aligned(4096)));

#define RING_BYTES (256 * 1024)
static uint8_t ring[RING_BYTES];
static volatile uint32_t ring_head, ring_tail;   /* head=write, tail=read */
static int ring_waiters;
static int writer_busy;                           /* see ac97.c */

static volatile uint8_t *regs;
static int present;
static uint8_t codec;            /* codec address used */
static int use_immediate;
static uint16_t corb_entries, rirb_entries, rirb_rp;
static uint32_t sd;              /* output stream descriptor offset */
static uint32_t sd_index;        /* its bit in INTCTL/INTSTS */
static volatile uint32_t irq_count;
static spinlock_t verb_lock;

static volatile int playing;
static uint32_t play_abs, write_abs, last_lpib, underruns;
static volatile int drop_req;    /* hda_drop() -> pump(): discard queued PCM */
/* Positions are byte counts that wrap at 2^32 (6.2 h of 48 kHz stereo), so
 * they are only ever compared through their signed difference.  A stream's
 * positions start at AUDIO_POS_START; `make AUDIO_POS_START=0xFFFE0000U`
 * starts them 128 KiB before the wrap, so every sound crosses it (test). */
#ifndef AUDIO_POS_START
#define AUDIO_POS_START 0U
#endif
#define POS_LT(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) < 0)
_Static_assert(AUDIO_POS_START % CYC_BYTES == 0, "positions start at a cyclic-buffer boundary");
static int drop_chan;                             /* its sleep channel */

/* output paths: the node to put the volume on, and that node's step count */
#define MAX_OUTS 4
static struct { uint8_t nid; uint8_t steps; } vol_amp[MAX_OUTS];
static int n_vol_amps;
static int volume = 100;

static uint32_t virt_to_phys(const void *p) {
    return (uint32_t)((uintptr_t)p - KERNEL_VMA);
}

static inline uint8_t  r8 (uint32_t o) { return *(volatile uint8_t  *)(regs + o); }
static inline uint16_t r16(uint32_t o) { return *(volatile uint16_t *)(regs + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(regs + o); }
static inline void w8 (uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(regs + o) = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(regs + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }

/* ~1 us per io_wait; usable before the PIT runs. */
static void delay_us(uint32_t us) {
    while (us--) io_wait();
}

/* Wait until (reg & mask) == want; 1 on success. */
static int wait32(uint32_t o, uint32_t mask, uint32_t want, uint32_t us) {
    while (us--) {
        if ((r32(o) & mask) == want) return 1;
        io_wait();
    }
    return (r32(o) & mask) == want;
}

static int wait16(uint32_t o, uint16_t mask, uint16_t want, uint32_t us) {
    while (us--) {
        if ((r16(o) & mask) == want) return 1;
        io_wait();
    }
    return (r16(o) & mask) == want;
}

static int wait8(uint32_t o, uint8_t mask, uint8_t want, uint32_t us) {
    while (us--) {
        if ((r8(o) & mask) == want) return 1;
        io_wait();
    }
    return (r8(o) & mask) == want;
}

/* ── command transport ────────────────────────────────────────────────── */

static int corb_send(uint32_t verb, uint32_t *resp) {
    uint16_t wp = (uint16_t)((r16(CORBWP) + 1) % corb_entries);

    corb[wp] = verb;
    w16(CORBWP, wp);
    for (uint32_t us = 0; us < 10000; us++) {
        uint16_t hw = r16(RIRBWP) & 0xFF;
        while (rirb_rp != hw) {
            rirb_rp = (uint16_t)((rirb_rp + 1) % rirb_entries);
            uint32_t r = rirb[rirb_rp * 2], ext = rirb[rirb_rp * 2 + 1];
            w8(RIRBSTS, 0x05);            /* response interrupt + overrun */
            if (ext & 0x10) continue;     /* unsolicited */
            *resp = r;
            return 0;
        }
        io_wait();
    }
    return -1;
}

static int immediate_send(uint32_t verb, uint32_t *resp) {
    if (!wait16(ICS, 0x1, 0, 10000)) return -1;
    w16(ICS, 0x2);                        /* clear stale "valid" */
    w32(ICOI, verb);
    w16(ICS, 0x1);
    if (!wait16(ICS, 0x2, 0x2, 10000)) return -1;
    *resp = r32(IRII);
    w16(ICS, 0x2);
    return 0;
}

/* Send one verb to `nid` on the selected codec; returns the response, or
 * 0xFFFFFFFF on timeout. */
static uint32_t verb(uint8_t nid, uint32_t cmd, uint32_t payload) {
    uint32_t v = ((uint32_t)codec << 28) | ((uint32_t)nid << 20);
    uint32_t resp = 0xFFFFFFFFU;

    if (cmd < 0x10)                       /* 4-bit verb, 16-bit payload */
        v |= (cmd << 16) | (payload & 0xFFFF);
    else
        v |= (cmd << 8) | (payload & 0xFF);
    spin_lock(&verb_lock);
    if (!use_immediate && corb_send(v, &resp) != 0) {
        printk("[HDA] CORB/RIRB timeout, using immediate commands\n");
        use_immediate = 1;
    }
    if (use_immediate && immediate_send(v, &resp) != 0)
        resp = 0xFFFFFFFFU;
    spin_unlock(&verb_lock);
    return resp;
}

static uint32_t param(uint8_t nid, uint32_t p) {
    return verb(nid, V_GET_PARAM, p);
}

/* ── controller bring-up ──────────────────────────────────────────────── */

static int ctrl_reset(void) {
    w8(CORBCTL, 0);
    w8(RIRBCTL, 0);
    wait8(CORBCTL, RING_RUN, 0, 1000);
    wait8(RIRBCTL, RING_RUN, 0, 1000);

    w32(GCTL, r32(GCTL) & ~GCTL_CRST);
    if (!wait32(GCTL, GCTL_CRST, 0, 10000)) return -1;
    delay_us(100);
    w32(GCTL, r32(GCTL) | GCTL_CRST);
    if (!wait32(GCTL, GCTL_CRST, GCTL_CRST, 10000)) return -1;
    /* codecs request a state change within 521 us of reset de-assertion */
    for (int i = 0; i < 20 && !(r16(STATESTS) & 0x7FFF); i++)
        delay_us(500);
    return 0;
}

static uint16_t ring_size(uint32_t reg) {
    uint8_t cap = r8(reg) >> 4;
    uint8_t sel = (cap & 0x4) ? 2 : (cap & 0x2) ? 1 : 0;

    w8(reg, (uint8_t)((r8(reg) & ~0x3) | sel));
    return sel == 2 ? 256 : sel == 1 ? 16 : 2;
}

static void rings_init(void) {
    corb_entries = ring_size(CORBSIZE);
    w32(CORBLBASE, virt_to_phys(corb));
    w32(CORBUBASE, 0);
    w16(CORBRP, 0x8000);                  /* reset read pointer */
    wait16(CORBRP, 0x8000, 0x8000, 1000); /* not every controller echoes it */
    w16(CORBRP, 0);
    wait16(CORBRP, 0x8000, 0, 1000);
    w16(CORBWP, 0);

    rirb_entries = ring_size(RIRBSIZE);
    w32(RIRBLBASE, virt_to_phys((const void *)rirb));
    w32(RIRBUBASE, 0);
    w16(RIRBWP, 0x8000);                  /* reset write pointer */
    rirb_rp = 0;
    w16(RINTCNT, 0xFF);
    w8(RIRBSTS, 0x05);

    w8(CORBCTL, RING_RUN);
    w8(RIRBCTL, RING_RUN);
}

/* ── codec walk ───────────────────────────────────────────────────────── */

#define MAX_NID 128
#define MAX_CONN 16
typedef struct {
    uint32_t caps, pincaps, cfg;
    uint8_t type, nconn;
    uint8_t conn[MAX_CONN];
} widget_t;

static widget_t w[MAX_NID];
static uint8_t afg;
static uint32_t afg_in_amp, afg_out_amp;

static void read_conn_list(uint8_t nid) {
    uint32_t len = param(nid, P_CONN_LEN);
    int longform = (len >> 7) & 1;
    uint32_t n = len & 0x7F;
    uint8_t prev = 0;

    w[nid].nconn = 0;
    if (len == 0xFFFFFFFFU) return;
    for (uint32_t i = 0; i < n && w[nid].nconn < MAX_CONN;) {
        uint32_t e = verb(nid, V_GET_CONN_LIST, i);
        int per = longform ? 2 : 4;
        for (int k = 0; k < per && i < n && w[nid].nconn < MAX_CONN; k++, i++) {
            uint32_t ent = longform ? (e >> (16 * k)) & 0xFFFF
                                    : (e >> (8 * k)) & 0xFF;
            uint32_t range = longform ? (ent & 0x8000) : (ent & 0x80);
            uint8_t id = (uint8_t)(ent & (longform ? 0x7FFF : 0x7F));
            if (range && prev) {           /* prev..id inclusive */
                for (uint8_t x = (uint8_t)(prev + 1);
                     x <= id && w[nid].nconn < MAX_CONN; x++)
                    w[nid].conn[w[nid].nconn++] = x;
            } else {
                w[nid].conn[w[nid].nconn++] = id;
            }
            prev = id;
        }
    }
}

static uint32_t amp_caps(uint8_t nid, int out) {
    if (w[nid].caps & WCAP_AMP_OVRD)
        return param(nid, out ? P_OUT_AMP_CAPS : P_IN_AMP_CAPS);
    return out ? afg_out_amp : afg_in_amp;
}

/* Set an amplifier: out (1) or input `idx` (0); gain in steps, or mute. */
static void set_amp(uint8_t nid, int out, int idx, uint32_t gain, int mute) {
    uint32_t p = (out ? 0x8000U : 0x4000U) | 0x3000U |
                 ((uint32_t)(idx & 0xF) << 8) | (mute ? 0x80U : 0) |
                 (gain & 0x7F);
    verb(nid, V4_SET_AMP, p);
}

static uint32_t amp_0db(uint32_t caps) {
    uint32_t steps = (caps >> 8) & 0x7F, offset = caps & 0x7F;
    return offset > steps ? steps : offset;
}

/* Depth-first search pin -> DAC.  path[0] is the pin; sel[i] is the
 * connection index taken out of path[i].  Returns the path length. */
static int find_dac(uint8_t nid, int depth, uint8_t *path, uint8_t *sel) {
    if (nid == 0 || nid >= MAX_NID || depth >= 8) return 0;
    path[depth] = nid;
    if (w[nid].type == WT_OUTPUT) return depth + 1;
    if (depth > 0 && w[nid].type != WT_MIXER && w[nid].type != WT_SELECTOR)
        return 0;
    for (int i = 0; i < w[nid].nconn; i++) {
        int n = find_dac(w[nid].conn[i], depth + 1, path, sel);
        if (n) { sel[depth] = (uint8_t)i; return n; }
    }
    return 0;
}

static void apply_volume(void) {
    for (int i = 0; i < n_vol_amps; i++) {
        uint32_t g = (uint32_t)vol_amp[i].steps * (uint32_t)volume / 100U;
        set_amp(vol_amp[i].nid, 1, 0, g, volume == 0);
    }
}

static int setup_path(uint8_t pin, int is_hp) {
    uint8_t path[8], sel[8];
    int n = find_dac(pin, 0, path, sel);
    int vol_done = 0;

    if (!n) return 0;
    /* walk DAC first, so the volume amp is the one closest to the DAC */
    for (int i = n - 1; i >= 0; i--) {
        uint8_t nid = path[i];
        widget_t *x = &w[nid];

        verb(nid, V_SET_POWER, 0);                       /* D0 */
        if (i < n - 1 && x->nconn > 1 && x->type != WT_MIXER)
            verb(nid, V_SET_CONN_SEL, sel[i]);
        if (i < n - 1 && x->type == WT_MIXER && (x->caps & WCAP_IN_AMP))
            set_amp(nid, 0, sel[i], amp_0db(amp_caps(nid, 0)), 0);
        if (x->caps & WCAP_OUT_AMP) {
            uint32_t c = amp_caps(nid, 1);
            uint8_t steps = (uint8_t)((c >> 8) & 0x7F);
            if (!vol_done && steps && n_vol_amps < MAX_OUTS) {
                vol_amp[n_vol_amps].nid = nid;
                vol_amp[n_vol_amps].steps = (uint8_t)amp_0db(c);
                n_vol_amps++;
                vol_done = 1;
            } else {
                set_amp(nid, 1, 0, amp_0db(c), 0);
            }
        }
        if (x->type == WT_OUTPUT) {
            verb(nid, V_SET_STREAM, STREAM_TAG << 4);
            verb(nid, V4_SET_FORMAT, STREAM_FMT);
        }
    }
    verb(pin, V_SET_PIN_CTL,
         PIN_OUT_EN | ((is_hp && (w[pin].pincaps & PINCAP_HP)) ? PIN_HP_EN : 0));
    if (w[pin].pincaps & PINCAP_EAPD)
        verb(pin, V_SET_EAPD, 0x02);
    printk("[HDA] output pin %u -> dac %u (%d hops, cfg 0x%08x)\n",
           (unsigned)pin, (unsigned)path[n - 1], n - 1, (unsigned)w[pin].cfg);
    return 1;
}

static int codec_init(uint8_t cad) {
    uint32_t sub, start, count, outs = 0;

    codec = cad;
    printk("[HDA] codec %u: vendor 0x%08x\n", (unsigned)cad,
           (unsigned)param(0, P_VENDOR_ID));
    sub = param(0, P_SUB_NODES);
    if (sub == 0xFFFFFFFFU) return 0;
    afg = 0;
    for (uint32_t i = 0; i < (sub & 0xFF); i++) {
        uint8_t fg = (uint8_t)(((sub >> 16) & 0xFF) + i);
        if ((param(fg, P_FG_TYPE) & 0xFF) == 0x01) { afg = fg; break; }
    }
    if (!afg) return 0;
    verb(afg, V_SET_POWER, 0);
    delay_us(1000);
    afg_in_amp = param(afg, P_IN_AMP_CAPS);
    afg_out_amp = param(afg, P_OUT_AMP_CAPS);

    sub = param(afg, P_SUB_NODES);
    start = (sub >> 16) & 0xFF;
    count = sub & 0xFF;
    memset(w, 0, sizeof(w));
    for (uint32_t nid = start; nid < start + count && nid < MAX_NID; nid++) {
        widget_t *x = &w[nid];
        x->caps = param((uint8_t)nid, P_WIDGET_CAPS);
        x->type = (uint8_t)((x->caps >> 20) & 0xF);
        if (x->caps & WCAP_CONN) read_conn_list((uint8_t)nid);
        if (x->type == WT_PIN) {
            x->pincaps = param((uint8_t)nid, P_PIN_CAPS);
            x->cfg = verb((uint8_t)nid, V_GET_CFG_DEF, 0);
        }
    }

    /* line out, speaker, headphone pins that are physically connected */
    n_vol_amps = 0;
    for (uint32_t nid = start; nid < start + count && nid < MAX_NID; nid++) {
        widget_t *x = &w[nid];
        uint32_t dev = (x->cfg >> 20) & 0xF, conn = x->cfg >> 30;
        if (x->type != WT_PIN || !(x->pincaps & PINCAP_OUT)) continue;
        if (conn == 1 || dev > 2) continue;
        outs += (uint32_t)setup_path((uint8_t)nid, dev == 2);
    }
    if (!outs) {   /* no usable config defaults: any output-capable pin */
        for (uint32_t nid = start; nid < start + count && nid < MAX_NID; nid++)
            if (w[nid].type == WT_PIN && (w[nid].pincaps & PINCAP_OUT) &&
                setup_path((uint8_t)nid, 0))
                { outs = 1; break; }
    }
    apply_volume();
    return (int)outs;
}

/* ── stream ───────────────────────────────────────────────────────────── */

static void stream_stop(void) {
    w32(sd + SD_CTL, r32(sd + SD_CTL) & ~(SD_CTL_RUN | SD_CTL_IOCE) & 0x00FFFFFFU);
    wait32(sd + SD_CTL, SD_CTL_RUN, 0, 1000);
    w8(sd + SD_STS, SD_STS_BCIS | SD_STS_FIFOE | SD_STS_DESE);
}

static void stream_setup(void) {
    stream_stop();
    w32(sd + SD_CTL, SD_CTL_SRST);
    wait32(sd + SD_CTL, SD_CTL_SRST, SD_CTL_SRST, 1000);
    w32(sd + SD_CTL, 0);
    wait32(sd + SD_CTL, SD_CTL_SRST, 0, 1000);

    for (int i = 0; i < NUM_BUFS; i++) {
        bdl[i].addr_lo = virt_to_phys(cyc + i * BUF_BYTES);
        bdl[i].addr_hi = 0;
        bdl[i].len = BUF_BYTES;
        bdl[i].flags = 1;                 /* IOC */
    }
    w32(sd + SD_BDPL, virt_to_phys(bdl));
    w32(sd + SD_BDPU, 0);
    w32(sd + SD_CBL, CYC_BYTES);
    w16(sd + SD_LVI, NUM_BUFS - 1);
    w16(sd + SD_FMT, STREAM_FMT);
    w32(sd + SD_CTL, (uint32_t)STREAM_TAG << 20);
}

static uint32_t ring_used(void) {
    return ring_head - ring_tail;
}

/* Copy ring bytes into the cyclic buffer at write_abs, as far as
 * `limit` (absolute). */
static void copy_in(uint32_t limit) {
    uint32_t n = ring_used();

    if (!POS_LT(write_abs, limit)) return;
    if (n > limit - write_abs) n = limit - write_abs;
    __asm__ volatile("" ::: "memory");        /* ring data after ring_head */
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

/*
 * Positions are absolute byte counts since the stream started; the cyclic
 * buffer holds [play_abs, play_abs + CYC_BYTES).  Real PCM is written up to
 * write_abs and everything beyond it is silence: space is zeroed as soon as
 * the engine has played it, so stale samples never come round again.  Data
 * is never written into the WRITE_GUARD bytes just behind the play position
 * (the engine may already have fetched them); after an underrun, writing
 * resumes RESYNC_LEAD bytes ahead of the play position.
 */
#define WRITE_GUARD  1024
#define RESYNC_LEAD  4096
#define START_BYTES  (32 * 1024)

static void pump(void) {
    static uint32_t idle_head;

    if (!present) return;

    if (drop_req) {
        /* Everything queued goes: the ring, and the real PCM ahead of the
         * engine in the cyclic buffer (the guard bytes it may have fetched
         * already stay).  The end-of-data test below then stops the stream. */
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
        stream_setup();
        play_abs = AUDIO_POS_START;
        last_lpib = 0;
        write_abs = AUDIO_POS_START + (ring_tail & 3);   /* frame alignment */
        memset(cyc, 0, CYC_BYTES);
        copy_in(CYC_BYTES - WRITE_GUARD);
        w32(sd + SD_CTL, ((uint32_t)STREAM_TAG << 20) | SD_CTL_RUN | SD_CTL_IOCE);
        playing = 1;
        printk("[HDA] stream start\n");
        return;
    }

    {
        uint32_t lpib = r32(sd + SD_LPIB) % CYC_BYTES;
        uint32_t old = play_abs;
        play_abs += (lpib - last_lpib + CYC_BYTES) % CYC_BYTES;
        last_lpib = lpib;
        /* played space comes round again as the future: silence it */
        zero_range(old, play_abs);
    }
    if (POS_LT(write_abs, play_abs) && ring_used()) {
        underruns++;
        write_abs = play_abs + RESYNC_LEAD;
        write_abs += (ring_tail - write_abs) & 3;   /* keep frames aligned */
    }
    copy_in(play_abs + CYC_BYTES - WRITE_GUARD);

    if (ring_used() == 0 && !POS_LT(play_abs, write_abs)) {
        stream_stop();
        playing = 0;
        printk("[HDA] playback done (%u irqs, %u underruns)\n",
               (unsigned)irq_count, (unsigned)underruns);
    }
}

static void hda_irq(registers_t *r) {
    (void)r;
    uint32_t st = r32(INTSTS);

    if (!st || st == 0xFFFFFFFFU) return;          /* not ours */
    if (st & (1U << sd_index))
        w8(sd + SD_STS, SD_STS_BCIS | SD_STS_FIFOE | SD_STS_DESE);
    if (st & 0x40000000U)                          /* controller: RIRB */
        w8(RIRBSTS, 0x05);
    irq_count++;
    io_wake();
}

static void khdad(void) {
    for (;;) {
        preempt_disable();
        pump();
        preempt_enable();
        current_proc->wake_tick = pit_ticks() + 2;   /* 20ms safety tick */
        sleep_on(&io_activity);
    }
}

/* ── public interface ─────────────────────────────────────────────────── */

int hda_present(void) {
    return present;
}

void hda_start_thread(void) {
    if (present)
        proc_create_kthread(khdad, "khdad");
}

int hda_get_volume(void) {
    return present ? volume : -19;
}

int hda_set_volume(int percent) {
    if (!present) return -19;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    volume = percent;
    apply_volume();
    return 0;
}

/* PCM bytes accepted but not yet played: the ring plus the real data ahead
 * of the engine in the cyclic buffer (as of the last pump, <= 20 ms old). */
uint32_t hda_queued(void) {
    uint32_t q;

    if (!present) return 0;
    q = ring_used();
    if (playing && POS_LT(play_abs, write_abs))
        q += write_abs - play_abs;
    return q;
}

/* Throw away everything queued (ALSA drop); returns once khdad has done it. */
void hda_drop(void) {
    if (!present) return;
    drop_req = 1;
    io_wake();
    while (drop_req) {
        current_proc->wake_tick = pit_ticks() + 1;
        sleep_on(&drop_chan);
    }
}

/* Blocking PCM write (48kHz S16LE stereo); same contract as ac97_write. */
int hda_write(const uint8_t *data, uint32_t len) {
    uint32_t written = 0;

    if (!present) return -19;
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
            __asm__ volatile("" ::: "memory");   /* data before ring_head */
            ring_head += n;
            written += n;
        }
        io_wake();
    }

    writer_busy = 0;
    wake_up(&writer_busy);
    return written ? (int)written : (len ? -4 : 0);
}

void hda_init(void) {
    const pci_device_t *dev = pci_find_class(0x04, 0x03);   /* multimedia/HDA */
    uint32_t bar, gcap;
    uint16_t codecs;

    if (!dev) {
        printk("[HDA] not present\n");
        return;
    }
    bar = dev->bar[0];
    if ((bar & 0x1) || ((bar & 0x6) == 0x4 && dev->bar[1])) {
        printk("[HDA] BAR0 0x%08x unusable\n", (unsigned)bar);
        return;
    }
    {   /* memory space + bus mastering, INTx enabled */
        uint32_t cmd = pci_read_config32(dev->bus, dev->slot, dev->func, 0x04);
        cmd = (cmd | 0x6U) & ~0x400U;
        pci_write_config32(dev->bus, dev->slot, dev->func, 0x04, cmd);
    }
    regs = (volatile uint8_t *)mmio_map(bar & ~0xFU, 0x4000);
    if (!regs) return;
    spin_init(&verb_lock);

    gcap = r16(GCAP);
    printk("[HDA] controller %04x:%04x v%u.%u gcap=0x%04x iss=%u oss=%u\n",
           (unsigned)dev->vendor_id, (unsigned)dev->device_id,
           (unsigned)r8(VMAJ), (unsigned)r8(VMIN), (unsigned)gcap,
           (unsigned)((gcap >> 8) & 0xF), (unsigned)((gcap >> 12) & 0xF));
    if (((gcap >> 12) & 0xF) == 0) {
        printk("[HDA] no output streams\n");
        return;
    }
    if (ctrl_reset() != 0) {
        printk("[HDA] controller reset timed out\n");
        return;
    }
    codecs = r16(STATESTS) & 0x7FFF;
    w16(STATESTS, codecs);
    if (!codecs) {
        printk("[HDA] no codecs\n");
        return;
    }
    rings_init();

    sd_index = (gcap >> 8) & 0xF;                /* first output stream */
    sd = 0x80 + sd_index * 0x20;

    {
        int ok = 0;
        for (uint8_t cad = 0; cad < 15 && !ok; cad++)
            if (codecs & (1U << cad))
                ok = codec_init(cad);
        if (!ok) {
            printk("[HDA] no codec with an output path\n");
            return;
        }
    }

    stream_setup();
    if (dev->irq_line >= 1 && dev->irq_line <= 15) {
        irq_install_handler((int)dev->irq_line, (void *)hda_irq);
        pic_unmask(dev->irq_line);
    }
    w32(INTCTL, INTCTL_GIE | (1U << sd_index));

    present = 1;
    printk("[HDA] up: mmio=0x%08x irq=%u stream %u, %d output(s)\n",
           (unsigned)(bar & ~0xFU), (unsigned)dev->irq_line,
           (unsigned)sd_index, n_vol_amps);
}
