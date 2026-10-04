/*
 * USB Audio Class playback: a UAC1 (or UAC2) device's PCM output as ALSA
 * card 1 (/dev/snd/controlC1, /dev/snd/pcmC1D0p) while it is plugged in.
 *
 * Written from the USB Device Class Definition for Audio Devices 1.0
 * (AudioControl descriptors 4.3, AudioStreaming 4.5/4.6, requests 5.2) and
 * 2.0 (clock sources, the CUR/RANGE requests, 5.2), and the Audio Data
 * Formats 1.0 (Type I format descriptor, 2.2.5).  No code was copied from
 * any other implementation.
 *
 * Setup.  The configuration is walked once: the AudioControl interface's
 * terminals and units, and every alternate setting of the AudioStreaming
 * interfaces with an isochronous OUT endpoint carrying Type I PCM.  The
 * driver takes 16-bit (or 24/32-bit) stereo (or mono) at 48 kHz, else
 * 44.1 kHz, else the setting's first rate, preferring stereo 16-bit; selects
 * that alternate setting (SET_INTERFACE), configures the endpoint and sets
 * the sampling frequency (UAC1: SET_CUR on the endpoint when it has the
 * control or several rates; UAC2: SET_CUR on the clock source).  A Feature
 * Unit on the path from the stream's input terminal to an output terminal
 * with master volume and/or mute becomes the card's Master Playback
 * Volume / Switch.
 *
 * Data path.  ALSA hands over S16LE stereo at the device's rate; write()
 * converts it to the device's format into a byte FIFO (256 KiB, about 1.3 s
 * at 48 kHz) and feeds TDs from it: each TD is one service interval's
 * frames (rate x interval with the remainder carried, so 44.1 kHz on a
 * 1 ms interval sends 44 frames nine times and 45 the tenth), copied into
 * one of SLOTS 512-byte DMA slots and queued as an isochronous TD
 * (xhci.c's usb_iso_*).  Completions, run by whoever drains the event ring
 * (kusbd on the controller's interrupt), free the slot and queue more.
 * The stream runs while there is data: when the FIFO runs dry the ring
 * underruns and the next write starts it again ("as soon as possible"
 * scheduling).  queued() (what ALSA's hw pointer is derived from) is the
 * FIFO plus the TDs in flight.
 *
 * Synchronisation: adaptive and synchronous endpoints take the nominal
 * rate, which is all they need.  An asynchronous endpoint's feedback is not
 * read (the stream plays at the nominal rate, so the device's clock may
 * drift against it: an occasional repeated or dropped packet on its side).
 *
 * Unplugging: kusbd calls usb_audio_detach() with the USB lock held; the
 * card leaves /dev/snd, writers blocked on a full FIFO are woken and get
 * ENODEV, and an open PCM reports DISCONNECTED until it is closed.
 */
#include "usb.h"
#include "../alsa.h"
#include "../../arch/i686/cpu/pit.h"
#include "../../kernel/printk.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../proc/process.h"
#include "../../proc/scheduler.h"
#include "../../proc/signal.h"
#include <stdint.h>

#define AC_SUBCLASS_CONTROL   1
#define AC_SUBCLASS_STREAMING 2
#define UAC2_PROTOCOL         0x20

/* class-specific AudioControl descriptor subtypes */
#define AC_HEADER          1
#define AC_INPUT_TERMINAL  2
#define AC_OUTPUT_TERMINAL 3
#define AC_MIXER_UNIT      4
#define AC_SELECTOR_UNIT   5
#define AC_FEATURE_UNIT    6
#define AC2_CLOCK_SOURCE   0x0A
/* AudioStreaming */
#define AS_GENERAL         1
#define AS_FORMAT_TYPE     2
#define FORMAT_TYPE_I      1
#define UAC1_FORMAT_PCM    1

/* requests (UAC1 5.2.1, UAC2 5.2.1) */
#define UAC1_SET_CUR  0x01
#define UAC1_GET_CUR  0x81
#define UAC1_GET_MIN  0x82
#define UAC1_GET_MAX  0x83
#define UAC2_CUR      0x01
#define UAC2_RANGE    0x02
#define CS_MUTE       0x01
#define CS_VOLUME     0x02
#define CS_SAM_FREQ   0x01      /* UAC1 endpoint / UAC2 clock source */

#define FIFO_SIZE     (256U * 1024U)
#define SLOTS         128
#define SLOT_SIZE     512U
#define MAX_ENTITIES  32
#define MAX_ALTS      8
#define MAX_RATES     8

/* ── descriptor walk ────────────────────────────────────────────────────── */

typedef struct {
    uint8_t id, type, source;      /* type: AC_* subtype */
    uint8_t vol, mute;             /* feature unit: master controls */
    uint8_t chvol;                 /* channels 1..7 with their own volume
                                    * (bit c), when the master has none */
    uint16_t term;                 /* terminal type */
} entity_t;

typedef struct {
    uint8_t ifnum, alt;
    uint8_t link;                  /* terminal the stream feeds */
    uint8_t channels, subframe, bits;
    uint8_t nrates, continuous;
    uint32_t rates[MAX_RATES];     /* discrete, or [min, max] */
    const usb_endpoint_desc_t *ep;
    const uint8_t *comp;           /* SuperSpeed companion */
    uint8_t ep_attr;               /* class-specific endpoint bmAttributes */
    int pcm;
} as_alt_t;

typedef struct {
    int uac2;
    uint8_t ac_ifnum;
    entity_t ent[MAX_ENTITIES];
    int nent;
    as_alt_t alt[MAX_ALTS];
    int nalt;
} uac_desc_t;

static uint32_t rd24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint32_t rd32le(const uint8_t *p) {
    return rd24(p) | ((uint32_t)p[3] << 24);
}

static void parse(const uint8_t *cfg, uint32_t len, uac_desc_t *u) {
    memset(u, 0, sizeof(*u));
    int cur_class = -1, cur_sub = -1;
    as_alt_t *a = 0;
    for (uint32_t off = 0; off + 2 <= len;) {
        const uint8_t *p = cfg + off;
        uint8_t blen = p[0], type = p[1];
        if (blen < 2 || off + blen > len) break;
        if (type == USB_DT_INTERFACE && blen >= 9) {
            const usb_interface_desc_t *i = (const usb_interface_desc_t *)p;
            cur_class = i->bInterfaceClass;
            cur_sub = i->bInterfaceSubClass;
            a = 0;
            if (cur_class == USB_CLASS_AUDIO &&
                cur_sub == AC_SUBCLASS_CONTROL) {
                u->ac_ifnum = i->bInterfaceNumber;
                u->uac2 = i->bInterfaceProtocol == UAC2_PROTOCOL;
            } else if (cur_class == USB_CLASS_AUDIO &&
                       cur_sub == AC_SUBCLASS_STREAMING &&
                       i->bAlternateSetting && u->nalt < MAX_ALTS) {
                a = &u->alt[u->nalt++];
                a->ifnum = i->bInterfaceNumber;
                a->alt = i->bAlternateSetting;
            }
        } else if (type == USB_DT_CS_INTERFACE && blen >= 3 &&
                   cur_class == USB_CLASS_AUDIO &&
                   cur_sub == AC_SUBCLASS_CONTROL && u->nent < MAX_ENTITIES) {
            entity_t *e = &u->ent[u->nent];
            uint8_t st = p[2];
            memset(e, 0, sizeof(*e));
            e->type = st;
            if (st == AC_INPUT_TERMINAL && blen >= 8) {
                e->id = p[3];
                e->term = (uint16_t)(p[4] | (p[5] << 8));
                u->nent++;
            } else if (st == AC_OUTPUT_TERMINAL && blen >= 8) {
                e->id = p[3];
                e->term = (uint16_t)(p[4] | (p[5] << 8));
                e->source = p[7];
                u->nent++;
            } else if (st == AC_FEATURE_UNIT && !u->uac2 && blen >= 7 &&
                       p[5] >= 1) {
                /* bControlSize at 5, bmaControls(0) (master) at 6, then
                 * one per channel */
                uint8_t cs = p[5];
                e->id = p[3];
                e->source = p[4];
                e->mute = (p[6] & 1) != 0;
                e->vol = (p[6] & 2) != 0;
                for (int c = 1; c < 8 && 6 + (c + 1) * cs <= blen - 1; c++)
                    if (p[6 + c * cs] & 2) e->chvol |= (uint8_t)(1U << c);
                u->nent++;
            } else if (st == AC_FEATURE_UNIT && u->uac2 && blen >= 9) {
                /* bmaControls(0) at 5, 2 bits per control: 3 = settable */
                uint32_t m = rd32le(p + 5);
                e->id = p[3];
                e->source = p[4];
                e->mute = (m & 3) == 3;
                e->vol = ((m >> 2) & 3) == 3;
                for (int c = 1; c < 8 && 5 + 4 * c + 4 <= blen - 1; c++)
                    if (((rd32le(p + 5 + 4 * c) >> 2) & 3) == 3)
                        e->chvol |= (uint8_t)(1U << c);
                u->nent++;
            } else if ((st == AC_MIXER_UNIT || st == AC_SELECTOR_UNIT) &&
                       blen >= 6) {
                e->id = p[3];
                e->source = p[5];          /* the first input */
                u->nent++;
            } else if (st == AC2_CLOCK_SOURCE && u->uac2 && blen >= 8) {
                e->id = p[3];
                u->nent++;
            }
        } else if (type == USB_DT_CS_INTERFACE && blen >= 3 && a) {
            uint8_t st = p[2];
            if (st == AS_GENERAL && !u->uac2 && blen >= 7) {
                a->link = p[3];
                a->pcm = (p[5] | (p[6] << 8)) == UAC1_FORMAT_PCM;
            } else if (st == AS_GENERAL && u->uac2 && blen >= 16) {
                a->link = p[3];
                a->pcm = p[5] == FORMAT_TYPE_I && (rd32le(p + 6) & 1);
                a->channels = p[10];
            } else if (st == AS_FORMAT_TYPE && !u->uac2 && blen >= 8 &&
                       p[3] == FORMAT_TYPE_I) {
                a->channels = p[4];
                a->subframe = p[5];
                a->bits = p[6];
                uint8_t n = p[7];
                if (n == 0 && blen >= 14) {
                    a->continuous = 1;
                    a->rates[0] = rd24(p + 8);
                    a->rates[1] = rd24(p + 11);
                    a->nrates = 2;
                } else {
                    for (uint8_t k = 0; k < n && k < MAX_RATES &&
                                        8U + 3U * k + 3U <= blen; k++)
                        a->rates[a->nrates++] = rd24(p + 8 + 3 * k);
                }
            } else if (st == AS_FORMAT_TYPE && u->uac2 && blen >= 6 &&
                       p[3] == FORMAT_TYPE_I) {
                a->subframe = p[4];
                a->bits = p[5];
            }
        } else if (type == USB_DT_ENDPOINT && blen >= 7 && a && !a->ep) {
            const usb_endpoint_desc_t *ep = (const usb_endpoint_desc_t *)p;
            if ((ep->bmAttributes & 3) == 1 && !(ep->bEndpointAddress & 0x80))
                a->ep = ep;
        } else if (type == USB_DT_SS_EP_COMP && blen >= 6 && a && a->ep &&
                   !a->comp) {
            a->comp = p;
        } else if (type == USB_DT_CS_ENDPOINT && blen >= 4 && a && a->ep &&
                   p[2] == 1 /* EP_GENERAL */) {
            a->ep_attr = p[3];
        }
        off += blen;
    }
}

static int alt_usable(const as_alt_t *a) {
    return a->ep && a->pcm && (a->channels == 1 || a->channels == 2) &&
           ((a->subframe == 2 && a->bits == 16) ||
            (a->subframe == 3 && a->bits >= 16) ||
            (a->subframe == 4 && a->bits >= 16));
}

/* Does alternate setting `a` play `rate`?  UAC2 rates come from the clock
 * source and are tried, not listed. */
static int alt_has_rate(const uac_desc_t *u, const as_alt_t *a,
                        uint32_t rate) {
    if (u->uac2) return 1;
    if (a->continuous) return rate >= a->rates[0] && rate <= a->rates[1];
    for (int k = 0; k < a->nrates; k++)
        if (a->rates[k] == rate) return 1;
    return 0;
}

int usb_audio_match(const uint8_t *cfg, uint32_t len) {
    static uac_desc_t u;      /* kusbd only (lock held) */
    parse(cfg, len, &u);
    for (int k = 0; k < u.nalt; k++)
        if (alt_usable(&u.alt[k])) return 1;
    return 0;
}

/* ── device state ───────────────────────────────────────────────────────── */

static struct {
    struct usb_device *dev;        /* NULL: no card */
    int uac2;
    uint8_t ac_ifnum, as_ifnum, alt;
    uint8_t channels, subframe;
    uint32_t rate;
    uint32_t interval_us;
    uint32_t frame_bytes;          /* device frame */
    uint32_t acc;                  /* rate x interval remainder, in us */
    /* the Feature Unit: master mute / volume, and the volume range in
     * 1/256 dB */
    uint8_t fu, has_vol, has_mute, clock;
    uint8_t vol_chans;             /* 0: master; else channels (bit c) */
    int16_t vmin, vmax;
    int volume, muted;
    /* FIFO of device-format bytes: wr (writer), rd (fed into TDs) */
    uint8_t *fifo;
    volatile uint32_t wr, rd;
    volatile uint32_t inflight;    /* bytes in queued TDs */
    uint32_t slot_next, nqueued;
    int streaming;
    int draining;                  /* the stream ends: pad its last TD */
    char longname[64];
} ua;

static uint8_t slots[SLOTS][SLOT_SIZE] __attribute__((aligned(4096)));
static int writer_chan;

/* frames in the next TD (does not advance the remainder) */
static uint32_t next_frames(uint32_t *acc_out) {
    uint64_t acc = ua.acc + (uint64_t)ua.rate * ua.interval_us;
    uint32_t frames = (uint32_t)(acc / 1000000U);
    *acc_out = (uint32_t)(acc % 1000000U);
    return frames;
}

static void stream_trace(const char *what) {
    uint32_t tds, missed, under;
    usb_iso_stats(ua.dev, 0, &tds, &missed, &under);
    printk("[USB-AUDIO] stream %s: %u TDs, %u missed, %u underruns\n", what,
           (unsigned)tds, (unsigned)missed, (unsigned)under);
}

/* Queue TDs while there is a whole one in the FIFO and room on the ring.
 * USB lock held. */
static void feed(void) {
    int queued = 0;
    if (!ua.dev || usb_device_gone(ua.dev)) return;
    int room = usb_iso_room(ua.dev, 0);
    if (room > SLOTS - (int)ua.nqueued) room = SLOTS - (int)ua.nqueued;
    while (room-- > 0) {
        uint32_t acc, frames = next_frames(&acc);
        uint32_t bytes = frames * ua.frame_bytes;
        uint32_t have = ua.wr - ua.rd;
        if (ua.draining && have && have < bytes && FIFO_SIZE - have >= bytes) {
            /* the last partial TD of a stream that ends: silence after it */
            for (uint32_t k = have; k < bytes; k++)
                ua.fifo[(ua.wr + (k - have)) % FIFO_SIZE] = 0;
            __sync_synchronize();
            ua.wr += bytes - have;
        }
        if (!bytes || ua.wr - ua.rd < bytes) break;
        uint8_t *slot = slots[ua.slot_next];
        uint32_t at = ua.rd % FIFO_SIZE, first = FIFO_SIZE - at;
        if (first > bytes) first = bytes;
        memcpy(slot, ua.fifo + at, first);
        if (first < bytes) memcpy(slot + first, ua.fifo, bytes - first);
        /* an interrupt every 8th TD and on the last one of the batch */
        int last = room == 0 || ua.wr - ua.rd < 2 * bytes;
        if (usb_iso_queue(ua.dev, 0, usb_phys(slot), bytes, 0, 0,
                          last || (ua.slot_next & 7) == 7) != 0)
            break;
        ua.slot_next = (ua.slot_next + 1) % SLOTS;
        ua.nqueued++;
        ua.acc = acc;
        __sync_synchronize();
        ua.inflight += bytes;
        ua.rd += bytes;
        queued++;
    }
    if (queued) {
        if (!ua.streaming) {
            ua.streaming = 1;
            printk("[USB-AUDIO] stream started (%u Hz, %u ch, %u us "
                   "interval)\n", (unsigned)ua.rate, (unsigned)ua.channels,
                   (unsigned)ua.interval_us);
        }
        usb_iso_kick(ua.dev, 0);
    }
}

/* A TD finished (played, or skipped): free its slot, queue more. */
static void td_done(void *ctx, uint32_t len, int ok) {
    (void)ctx;
    (void)ok;
    if (ua.nqueued) ua.nqueued--;
    ua.inflight = ua.inflight >= len ? ua.inflight - len : 0;
    feed();
    if (!ua.nqueued && ua.streaming && ua.wr == ua.rd) {
        ua.streaming = 0;
        stream_trace("ran out of data");
    }
    wake_up(&writer_chan);
    /* the hw pointer moved: poll()/select() on the PCM (aplay) looks again */
    io_wake_poll();
}

/* ── ALSA card operations (process context, USB lock not held) ──────────── */

static uint32_t fifo_used(void) {
    return ua.wr - ua.rd;
}

/* One writer at a time (ALSA's PCM and /dev/dsp1 may both write). */
static volatile int writer_busy;

static int writer_enter(void) {
    while (__sync_lock_test_and_set(&writer_busy, 1)) {
        if (signal_interrupt_pending(current_proc)) return -1;
        current_proc->wake_tick = pit_ticks() + 1;
        sleep_on(&writer_chan);
    }
    return 0;
}

static void writer_leave(void) {
    __sync_lock_release(&writer_busy);
    wake_up(&writer_chan);
}

/* S16LE stereo at ua.rate in; the device's format into the FIFO.  The FIFO
 * is filled under the USB lock, where feed() (and its end-of-stream
 * padding) also runs. */
static int ua_write(const uint8_t *data, uint32_t len) {
    uint32_t done = 0;
    uint8_t conv[128 * 8];
    len &= ~3U;
    if (writer_enter() < 0) return 0;
    while (done < len) {
        if (!ua.dev) {
            writer_leave();
            return done ? (int)done : -19;                   /* -ENODEV */
        }
        uint32_t space = FIFO_SIZE - fifo_used();   /* only grows meanwhile */
        uint32_t frames = space / ua.frame_bytes;
        if (frames > (len - done) / 4) frames = (len - done) / 4;
        if (frames > 128) frames = 128;
        if (!frames) {
            if (signal_interrupt_pending(current_proc)) break;
            current_proc->wake_tick = pit_ticks() + 2;
            sleep_on(&writer_chan);
            continue;
        }
        uint8_t *o = conv;
        for (uint32_t f = 0; f < frames; f++) {
            const uint8_t *in = data + done + f * 4;
            int16_t l = (int16_t)(in[0] | (in[1] << 8));
            int16_t r = (int16_t)(in[2] | (in[3] << 8));
            int16_t sm[2] = { l, r };
            if (ua.channels == 1) sm[0] = (int16_t)(((int32_t)l + r) / 2);
            for (uint32_t ch = 0; ch < ua.channels; ch++) {
                /* left-justified in the subframe (little endian) */
                for (uint32_t z = 0; z + 2 < ua.subframe; z++) *o++ = 0;
                *o++ = (uint8_t)sm[ch];
                *o++ = (uint8_t)((uint16_t)sm[ch] >> 8);
            }
        }
        uint32_t bytes = (uint32_t)(o - conv);
        usb_lock();
        ua.draining = 0;
        if (ua.dev) {
            uint32_t at = ua.wr % FIFO_SIZE, first = FIFO_SIZE - at;
            if (first > bytes) first = bytes;
            memcpy(ua.fifo + at, conv, first);
            if (first < bytes) memcpy(ua.fifo, conv + first, bytes - first);
            __sync_synchronize();
            ua.wr += bytes;
            feed();
        }
        usb_unlock();
        done += frames * 4;
    }
    writer_leave();
    return (int)done;
}

/* Bytes not yet played, in ALSA's S16 stereo terms. */
static uint32_t ua_queued(void) {
    uint32_t dev_bytes = fifo_used() + ua.inflight;
    if (!ua.frame_bytes) return 0;
    return dev_bytes / ua.frame_bytes * 4;
}

static void ua_drop(void) {
    usb_lock();
    if (ua.dev) {
        usb_iso_stop(ua.dev, 0);
        if (ua.streaming) stream_trace("dropped");
    }
    ua.rd = ua.wr;
    ua.inflight = 0;
    ua.nqueued = 0;
    ua.acc = 0;
    ua.streaming = 0;
    ua.draining = 0;
    usb_unlock();
    wake_up(&writer_chan);
}

/* The stream ends (ALSA DRAIN): from now until the next write, a partial
 * TD left at the end of the FIFO is padded with silence and played. */
static void ua_flush(void) {
    usb_lock();
    ua.draining = 1;
    if (ua.dev) feed();
    usb_unlock();
}

/* Feature Unit requests: UAC1 SET_CUR/GET_* or UAC2 CUR/RANGE on the master
 * channel.  `len` bytes in or out of `buf`. */
static int fu_request_ch(int in, uint8_t req, uint8_t cs, uint8_t ch,
                         void *buf, uint16_t len) {
    usb_setup_t s = { (uint8_t)((in ? USB_DIR_IN : 0) | USB_TYPE_CLASS |
                                USB_RECIP_INTERFACE),
                      req, (uint16_t)((cs << 8) | ch),
                      (uint16_t)((ua.fu << 8) | ua.ac_ifnum), len };
    return usb_control(ua.dev, &s, buf);
}

static int fu_request(int in, uint8_t req, uint8_t cs, void *buf,
                      uint16_t len) {
    return fu_request_ch(in, req, cs, 0, buf, len);
}

/* The channel the volume is read from: master, or the first one that has
 * its own control. */
static uint8_t vol_channel(void) {
    for (uint8_t c = 1; c < 8; c++)
        if (ua.vol_chans & (1U << c)) return c;
    return 0;
}

static int16_t pct_to_vol(int pct) {
    int lo = ua.vmin, hi = ua.vmax;
    /* the top 48 dB of the range; 0 % is the device's minimum */
    if (pct <= 0) return (int16_t)lo;
    if (hi - lo > 48 * 256) lo = hi - 48 * 256;
    return (int16_t)(lo + (hi - lo) * pct / 100);
}

static int vol_to_pct(int16_t v) {
    int lo = ua.vmin, hi = ua.vmax;
    if (hi - lo > 48 * 256) lo = hi - 48 * 256;
    if (v <= lo || hi <= lo) return v <= ua.vmin ? 0 : (hi <= lo ? 100 : 0);
    if (v >= hi) return 100;
    return (v - lo) * 100 / (hi - lo);
}

static int ua_get_volume(void) {
    return ua.dev ? ua.volume : -19;
}

static int ua_set_volume(int pct) {
    int r = -19;
    usb_lock();
    if (ua.dev) {
        int16_t v = pct_to_vol(pct);
        uint8_t b[2] = { (uint8_t)v, (uint8_t)((uint16_t)v >> 8) };
        r = 0;
        /* the master, or every channel that has a volume of its own */
        for (uint8_t c = 0; c < 8 && r == 0; c++) {
            if (c ? !(ua.vol_chans & (1U << c)) : ua.vol_chans != 0)
                continue;
            if (fu_request_ch(0, ua.uac2 ? UAC2_CUR : UAC1_SET_CUR,
                              CS_VOLUME, c, b, 2) < 0)
                r = -5;
        }
        if (r == 0) ua.volume = pct;
    }
    usb_unlock();
    return r;
}

static int ua_get_mute(void) {
    return ua.dev ? ua.muted : -19;
}

static int ua_set_mute(int on) {
    int r = -19;
    usb_lock();
    if (ua.dev) {
        uint8_t b = on ? 1 : 0;
        r = fu_request(0, ua.uac2 ? UAC2_CUR : UAC1_SET_CUR, CS_MUTE, &b,
                       1) < 0 ? -5 : 0;
        if (r == 0) ua.muted = on != 0;
    }
    usb_unlock();
    return r;
}

static alsa_out_t ua_out = {
    "USB Audio", ua.longname, 48000, 20000,
    ua_write, ua_queued, ua_drop, ua_flush,
    0, 0, 0, 0,
};

/* ── attach / detach (kusbd, lock held) ─────────────────────────────────── */

static const entity_t *entity(const uac_desc_t *u, uint8_t id) {
    for (int k = 0; k < u->nent; k++)
        if (u->ent[k].id == id && u->ent[k].type != AC2_CLOCK_SOURCE)
            return &u->ent[k];
    return 0;
}

/* The Feature Unit between the stream's input terminal `it` and an output
 * terminal: walk back from each output terminal along the sources. */
static const entity_t *find_fu(const uac_desc_t *u, uint8_t it) {
    for (int k = 0; k < u->nent; k++) {
        if (u->ent[k].type != AC_OUTPUT_TERMINAL) continue;
        const entity_t *fu = 0, *e = &u->ent[k];
        for (int hops = 0; e && hops < 8; hops++) {
            if (e->type == AC_FEATURE_UNIT && !fu &&
                (e->vol || e->mute || e->chvol))
                fu = e;
            if (e->id == it && e->type == AC_INPUT_TERMINAL) return fu;
            if (e->type == AC_INPUT_TERMINAL) break;
            e = entity(u, e->source);
        }
    }
    return 0;
}

/* UAC2: is `rate` in the clock source's RANGE? (Without an answer, try.) */
static int clock_has_rate(uint8_t clock, uint8_t ac_ifnum, uint32_t rate) {
    static uint8_t b[2 + 12 * 8];
    usb_setup_t s = { USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                      UAC2_RANGE, CS_SAM_FREQ << 8,
                      (uint16_t)((clock << 8) | ac_ifnum), sizeof(b) };
    int got = usb_control(ua.dev, &s, b);
    if (got < 2) return 1;
    int n = b[0] | (b[1] << 8);
    for (int k = 0; k < n && 2 + 12 * k + 12 <= got; k++) {
        uint32_t lo = rd32le(b + 2 + 12 * k), hi = rd32le(b + 6 + 12 * k);
        if (rate >= lo && rate <= hi) return 1;
    }
    return 0;
}

int usb_audio_attach(struct usb_device *d, const uint8_t *cfg, uint32_t len) {
    static uac_desc_t u;
    static const uint32_t want[] = { 48000, 44100 };
    if (ua.dev) {
        printk("[USB-AUDIO] slot %d: a USB audio card is already in use\n",
               usb_device_slot(d));
        return -1;
    }
    if (!ua.fifo) ua.fifo = kmalloc(FIFO_SIZE);
    if (!ua.fifo) return -1;
    parse(cfg, len, &u);
    uint8_t clock = 0;
    for (int k = 0; k < u.nent; k++)
        if (u.ent[k].type == AC2_CLOCK_SOURCE) { clock = u.ent[k].id; break; }

    /* stereo 16-bit at 48 kHz first; then the other formats; then 44.1 */
    const as_alt_t *best = 0;
    uint32_t rate = 0;
    int best_score = -1;
    ua.dev = d;                                /* for clock_has_rate() */
    for (int k = 0; k < u.nalt; k++) {
        const as_alt_t *a = &u.alt[k];
        if (!alt_usable(a)) continue;
        for (int w = 0; w < 2; w++) {
            if (!alt_has_rate(&u, a, want[w])) continue;
            if (u.uac2 && clock && !clock_has_rate(clock, u.ac_ifnum, want[w]))
                continue;
            int score = (2 - w) * 4 + (a->channels == 2) * 2 +
                        (a->subframe == 2);
            if (score > best_score) {
                best_score = score;
                best = a;
                rate = want[w];
            }
            break;
        }
        if (!best && !u.uac2 && a->nrates && !a->continuous) {
            best = a;                         /* whatever it plays */
            rate = a->rates[0];
        }
    }
    ua.dev = 0;
    if (!best || !rate || rate > 192000) {
        printk("[USB-AUDIO] slot %d: no usable playback format\n",
               usb_device_slot(d));
        return -1;
    }

    if (usb_set_interface(d, best->ifnum, best->alt) != 0) {
        printk("[USB-AUDIO] slot %d: SET_INTERFACE %u/%u failed\n",
               usb_device_slot(d), best->ifnum, best->alt);
        return -1;
    }
    usb_ep_cfg_t ec = { best->ep,
                        usb_device_speed(d) >= USB_SPEED_SUPER ? best->comp : 0,
                        0 };
    if (usb_configure_eps_x(d, &ec, 1) != 0) return -1;

    uint8_t *fifo = ua.fifo;
    memset(&ua, 0, sizeof(ua));
    ua.fifo = fifo;
    ua.dev = d;
    ua.uac2 = u.uac2;
    ua.ac_ifnum = u.ac_ifnum;
    ua.as_ifnum = best->ifnum;
    ua.alt = best->alt;
    ua.channels = best->channels;
    ua.subframe = best->subframe;
    ua.rate = rate;
    ua.clock = clock;
    ua.frame_bytes = (uint32_t)best->channels * best->subframe;
    ua.interval_us = usb_iso_interval_us(d, 0);
    uint32_t peak = (uint32_t)(((uint64_t)rate * ua.interval_us + 999999U) /
                               1000000U);
    /* what one TD may carry is what xhci.c configured (the companion
     * counts only at SuperSpeed), not what the descriptors claim */
    if (!ua.interval_us || peak * ua.frame_bytes > SLOT_SIZE ||
        peak * ua.frame_bytes > usb_iso_esit_bytes(d, 0)) {
        printk("[USB-AUDIO] slot %d: %u-byte packets do not fit\n",
               usb_device_slot(d), (unsigned)(peak * ua.frame_bytes));
        ua.dev = 0;
        return -1;
    }

    /* Sampling frequency.  UAC1: on the endpoint, when it has the control
     * (or more than one rate); a STALL is not fatal.  UAC2: the clock. */
    uint8_t f[4] = { (uint8_t)rate, (uint8_t)(rate >> 8),
                     (uint8_t)(rate >> 16), (uint8_t)(rate >> 24) };
    if (!u.uac2 && ((best->ep_attr & 1) || best->nrates > 1 ||
                    best->continuous)) {
        usb_setup_t s = { USB_TYPE_CLASS | USB_RECIP_ENDPOINT, UAC1_SET_CUR,
                          CS_SAM_FREQ << 8, best->ep->bEndpointAddress, 3 };
        if (usb_control(d, &s, f) < 0)
            printk("[USB-AUDIO] slot %d: SET_CUR sampling frequency "
                   "refused\n", usb_device_slot(d));
    } else if (u.uac2 && clock) {
        usb_setup_t s = { USB_TYPE_CLASS | USB_RECIP_INTERFACE, UAC2_CUR,
                          CS_SAM_FREQ << 8,
                          (uint16_t)((clock << 8) | u.ac_ifnum), 4 };
        if (usb_control(d, &s, f) < 0)
            printk("[USB-AUDIO] slot %d: clock %u refused %u Hz\n",
                   usb_device_slot(d), clock, (unsigned)rate);
    }

    /* Master volume / mute. */
    const entity_t *fu = find_fu(&u, best->link);
    ua_out.get_volume = 0;
    ua_out.set_volume = 0;
    ua_out.get_mute = 0;
    ua_out.set_mute = 0;
    if (fu) {
        ua.fu = fu->id;
        if (fu->vol || fu->chvol) {
            uint8_t b[14];
            int ok;
            ua.vol_chans = fu->vol ? 0 : fu->chvol;
            uint8_t ch = vol_channel();
            if (u.uac2) {
                ok = fu_request_ch(1, UAC2_RANGE, CS_VOLUME, ch, b, 8) >= 8;
                ua.vmin = (int16_t)(b[2] | (b[3] << 8));
                ua.vmax = (int16_t)(b[4] | (b[5] << 8));
            } else {
                ok = fu_request_ch(1, UAC1_GET_MIN, CS_VOLUME, ch, b, 2) == 2;
                ua.vmin = (int16_t)(b[0] | (b[1] << 8));
                ok = ok && fu_request_ch(1, UAC1_GET_MAX, CS_VOLUME, ch, b,
                                         2) == 2;
                ua.vmax = (int16_t)(b[0] | (b[1] << 8));
            }
            if (ok && ua.vmax > ua.vmin &&
                fu_request_ch(1, u.uac2 ? UAC2_CUR : UAC1_GET_CUR, CS_VOLUME,
                              ch, b, 2) == 2) {
                ua.volume = vol_to_pct((int16_t)(b[0] | (b[1] << 8)));
                ua.has_vol = 1;
                ua_out.get_volume = ua_get_volume;
                ua_out.set_volume = ua_set_volume;
            }
        }
        if (fu->mute) {
            uint8_t b = 0;
            if (fu_request(1, u.uac2 ? UAC2_CUR : UAC1_GET_CUR, CS_MUTE, &b,
                           1) == 1) {
                ua.muted = b != 0;
                ua.has_mute = 1;
                ua_out.get_mute = ua_get_mute;
                ua_out.set_mute = ua_set_mute;
            }
        }
    }

    usb_iso_start(d, 0, td_done, 0);
    ua_out.rate = rate;
    {
        /* "USB Audio <uac>, <rate> Hz <bits>-bit <ch> ch" */
        char *p = ua.longname;
        const char *t = u.uac2 ? "USB Audio 2.0 device" : "USB Audio 1.0 device";
        while (*t) *p++ = *t++;
        *p = 0;
    }
    printk("[USB-AUDIO] slot %d: UAC%d, interface %u alt %u, endpoint %02x, "
           "%u Hz, %u-bit %u ch, %u us interval, %s endpoint%s%s\n",
           usb_device_slot(d), u.uac2 ? 2 : 1, best->ifnum, best->alt,
           best->ep->bEndpointAddress, (unsigned)rate,
           (unsigned)best->subframe * 8, (unsigned)best->channels,
           (unsigned)ua.interval_us,
           ((best->ep->bmAttributes >> 2) & 3) == 1 ?
               "asynchronous (feedback not used)" :
           ((best->ep->bmAttributes >> 2) & 3) == 2 ? "adaptive" :
           ((best->ep->bmAttributes >> 2) & 3) == 3 ? "synchronous" : "no",
           ua.has_vol ? ", volume" : "", ua.has_mute ? ", mute" : "");
    if (ua.has_vol)
        printk("[USB-AUDIO] feature unit %u: volume (%s) %d..%d/256 dB, now "
               "%d%%%s\n", ua.fu, ua.vol_chans ? "per channel" : "master",
               ua.vmin, ua.vmax, ua.volume, ua.muted ? ", muted" : "");
    alsa_card_add(1, &ua_out);
    return 0;
}

void usb_audio_detach(struct usb_device *d) {
    if (ua.dev != d) return;
    if (ua.streaming) stream_trace("cut off (unplugged)");
    usb_iso_stop(d, 0);
    ua.dev = 0;
    ua.rd = ua.wr;
    ua.inflight = 0;
    ua.nqueued = 0;
    ua.streaming = 0;
    alsa_card_remove(1);
    wake_up(&writer_chan);
    printk("[USB-AUDIO] slot %d: removed\n", usb_device_slot(d));
}
