#pragma once
/*
 * Raw ALSA control and PCM ioctls for MaeroOS's own tools (no alsa-lib):
 * the Master Playback Volume / Switch mixer elements of /dev/snd/controlC<n>
 * and RW-interleaved playback on /dev/snd/pcmC<n>D0p.  The structure layouts
 * are the i386 ALSA UAPI ones (sound/asound.h); see drivers/alsa.c.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ALSACTL_ELEM_LIST   0xC0485510UL   /* _IOWR('U', 0x10, 72) */
#define ALSACTL_ELEM_INFO   0xC1105511UL   /* _IOWR('U', 0x11, 272) */
#define ALSACTL_ELEM_READ   0xC2C45512UL   /* _IOWR('U', 0x12, 708) */
#define ALSACTL_ELEM_WRITE  0xC2C45513UL   /* _IOWR('U', 0x13, 708) */
#define ALSAPCM_HW_PARAMS   0xC25C4111UL   /* _IOWR('A', 0x11, 604) */
#define ALSAPCM_PREPARE     0x00004140UL   /* _IO('A', 0x40) */
#define ALSAPCM_DRAIN       0x00004144UL   /* _IO('A', 0x44) */
#define ALSAPCM_WRITEI      0x400C4150UL   /* _IOW('A', 0x50, 12) */

static inline int alsactl_open(int card) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);
    return open(path, O_RDWR);
}

/* Read (set < 0) or write element `name` of the mixer; returns its value
 * or -1. */
static inline int alsactl_elem(int fd, const char *name, int set) {
    static unsigned char v[708];
    unsigned int val;
    memset(v, 0, sizeof(v));
    v[4] = 2;                                      /* iface: MIXER */
    strncpy((char *)v + 16, name, 43);
    if (ioctl(fd, ALSACTL_ELEM_READ, v) < 0) return -1;
    if (set >= 0) {
        val = (unsigned int)set;
        memcpy(v + 68, &val, 4);
        if (ioctl(fd, ALSACTL_ELEM_WRITE, v) < 0) return -1;
        if (ioctl(fd, ALSACTL_ELEM_READ, v) < 0) return -1;
    }
    memcpy(&val, v + 68, 4);
    return (int)val;
}

/* hw_params for S16_LE, `ch` channels at `rate`; the kernel picks the
 * period and buffer.  0 or -1. */
static inline int alsapcm_setup(int fd, unsigned int ch, unsigned int rate) {
    static unsigned char p[604];
    unsigned int all = 0xFFFFFFFFU;
    memset(p, 0, sizeof(p));
    for (int m = 0; m < 3; m++)                    /* masks: everything */
        for (int w = 0; w < 8; w++) memcpy(p + 4 + m * 32 + w * 4, &all, 4);
    memset(p + 4, 0, 32);
    p[4] = 1 << 3;                                 /* ACCESS_RW_INTERLEAVED */
    memset(p + 36, 0, 32);
    p[36] = 1 << 2;                                /* FORMAT_S16_LE */
    for (int i = 0; i < 12; i++) {                 /* intervals: [0, ~0] */
        unsigned char *iv = p + 260 + i * 12;
        unsigned int lo = 0, hi = all;
        if (i == 2) lo = hi = ch;                  /* CHANNELS */
        if (i == 3) lo = hi = rate;                /* RATE */
        if (i == 5) lo = hi = 1024;                /* PERIOD_SIZE */
        if (i == 7) lo = hi = 8;                   /* PERIODS */
        memcpy(iv, &lo, 4);
        memcpy(iv + 4, &hi, 4);
    }
    memcpy(p + 512, &all, 4);                      /* rmask */
    if (ioctl(fd, ALSAPCM_HW_PARAMS, p) < 0) return -1;
    return ioctl(fd, ALSAPCM_PREPARE, 0) < 0 ? -1 : 0;
}

/* Write `frames` interleaved frames; returns frames written or -1. */
static inline int alsapcm_write(int fd, const void *buf, unsigned int frames) {
    struct { int result; const void *buf; unsigned int frames; } x;
    x.result = 0;
    x.buf = buf;
    x.frames = frames;
    if (ioctl(fd, ALSAPCM_WRITEI, &x) < 0) return -1;
    return x.result;
}
