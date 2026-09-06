#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * wavplay — stream a WAV file to /dev/dsp (48 kHz S16LE stereo).
 * Files at other rates play at the wrong pitch; keep assets at 48k.
 */

int main(int argc, char **argv) {
    char hdr[44];
    static char buf[16384];
    int in, out, n;

    if (argc < 2) {
        printf("usage: wavplay <file.wav>\n");
        return 1;
    }
    in = open(argv[1], O_RDONLY);
    if (in < 0) {
        printf("wavplay: cannot open %s\n", argv[1]);
        return 1;
    }
    if (read(in, hdr, 44) != 44 || memcmp(hdr, "RIFF", 4) ||
        memcmp(hdr + 8, "WAVE", 4)) {
        printf("wavplay: not a WAV file\n");
        close(in);
        return 1;
    }
    out = open("/dev/dsp", O_WRONLY);
    if (out < 0) {
        printf("wavplay: /dev/dsp unavailable (no sound device?)\n");
        close(in);
        return 1;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        int off = 0;
        while (off < n) {
            int w = write(out, buf + off, n - off);
            if (w <= 0) goto done;
            off += w;
        }
    }
done:
    close(out);
    close(in);
    return 0;
}
