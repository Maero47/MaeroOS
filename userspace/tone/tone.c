#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "alsactl.h"

/*
 * tone — play a sine wave on /dev/dsp (48 kHz S16LE stereo).
 *   tone [-c card | -d dsp] <hz> <ms> [volume%]
 * With a volume, sets the mixer volume first (OSS SOUND_MIXER_WRITE_VOLUME).
 * With -c, plays on ALSA card `card` instead (/dev/snd/pcmC<card>D0p through
 * the raw PCM ioctls, 48 kHz S16 stereo, converted by the kernel to the
 * card's rate), the volume going to its Master Playback Volume.  With -d,
 * plays on another OSS-style device (/dev/dsp1: the USB audio card).
 */

#define RATE 48000
#define SOUND_MIXER_WRITE_VOLUME 0xC0044D00UL

/* sin/cos of a small angle by Taylor series (no libm here) */
static void sincos_small(double a, double *s, double *c) {
    double a2 = a * a;
    *s = a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72))));
    *c = 1 - a2 / 2 * (1 - a2 / 12 * (1 - a2 / 30 * (1 - a2 / 56)));
}

int main(int argc, char **argv) {
    static short buf[2 * 1024];
    int hz, ms, fd;
    long frames;
    double sw, cw, x = 0, y = 1;   /* rotating unit vector */

    int card = -1;
    const char *dsp = "/dev/dsp";
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        card = atoi(argv[2]);
        argv += 2;
        argc -= 2;
    } else if (argc > 2 && strcmp(argv[1], "-d") == 0) {
        dsp = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 3) {
        printf("usage: tone [-c card | -d dsp] <hz> <ms> [volume%%]\n");
        return 1;
    }
    hz = atoi(argv[1]);
    ms = atoi(argv[2]);
    if (hz <= 0 || hz >= RATE / 2 || ms <= 0) {
        printf("tone: bad arguments\n");
        return 1;
    }
    if (card >= 0) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0p", card);
        fd = open(path, O_RDWR);
        if (fd < 0) {
            printf("tone: %s unavailable\n", path);
            return 1;
        }
        if (argc > 3) {
            int c = alsactl_open(card);
            if (c < 0 || alsactl_elem(c, "Master Playback Volume",
                                      atoi(argv[3])) < 0)
                printf("tone: cannot set volume\n");
            if (c >= 0) close(c);
        }
        if (alsapcm_setup(fd, 2, RATE) < 0) {
            printf("tone: cannot set up the PCM\n");
            return 1;
        }
    } else {
        fd = open(dsp, O_WRONLY);
    }
    if (fd < 0) {
        printf("tone: %s unavailable (no sound device?)\n", dsp);
        return 1;
    }
    if (argc > 3 && card < 0) {
        int v = atoi(argv[3]);
        v = v | (v << 8);
        if (ioctl(fd, SOUND_MIXER_WRITE_VOLUME, &v) < 0)
            printf("tone: cannot set volume\n");
    }
    sincos_small(2 * 3.14159265358979323846 * hz / RATE, &sw, &cw);
    frames = (long)RATE * ms / 1000;
    while (frames > 0) {
        int n = frames > 1024 ? 1024 : (int)frames;
        for (int i = 0; i < n; i++) {
            double nx = x * cw + y * sw, ny = y * cw - x * sw;
            short v = (short)(int)(nx * 16000);
            x = nx;
            y = ny;
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        if (card >= 0) {
            int off = 0;
            while (off < n) {
                int w = alsapcm_write(fd, buf + 2 * off, (unsigned)(n - off));
                if (w <= 0) {
                    printf("tone: write failed\n");
                    close(fd);
                    return 1;
                }
                off += w;
            }
        } else {
            int len = n * 4, off = 0;
            while (off < len) {
                int w = write(fd, (char *)buf + off, len - off);
                if (w <= 0) goto done;
                off += w;
            }
        }
        frames -= n;
    }
done:
    if (card >= 0 && ioctl(fd, ALSAPCM_DRAIN, 0) < 0) {
        printf("tone: drain failed\n");
        close(fd);
        return 1;
    }
    close(fd);
    printf("tone: done\n");
    return 0;
}
