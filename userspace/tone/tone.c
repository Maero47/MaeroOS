#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * tone — play a sine wave on /dev/dsp (48 kHz S16LE stereo).
 *   tone <hz> <ms> [volume%]
 * With a volume, sets the mixer volume first (OSS SOUND_MIXER_WRITE_VOLUME).
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

    if (argc < 3) {
        printf("usage: tone <hz> <ms> [volume%%]\n");
        return 1;
    }
    hz = atoi(argv[1]);
    ms = atoi(argv[2]);
    if (hz <= 0 || hz >= RATE / 2 || ms <= 0) {
        printf("tone: bad arguments\n");
        return 1;
    }
    fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) {
        printf("tone: /dev/dsp unavailable (no sound device?)\n");
        return 1;
    }
    if (argc > 3) {
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
        {
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
    close(fd);
    printf("tone: done\n");
    return 0;
}
