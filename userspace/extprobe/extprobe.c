#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Write a 6MB counter pattern, read it back, report the first mismatch. */
#define TOTAL (6 * 1024 * 1024)
#define CHUNK 8192

int main(void) {
    static unsigned char buf[CHUNK], rd[CHUNK];
    int fd = open("/disk/extprobe.bin", O_WRONLY | O_CREAT | O_TRUNC);
    unsigned off = 0;

    if (fd < 0) { printf("extprobe: cannot create\n"); return 1; }
    while (off < TOTAL) {
        for (unsigned i = 0; i < CHUNK; i++)
            buf[i] = (unsigned char)((off + i) * 2654435761u >> 24);
        if (write(fd, buf, CHUNK) != CHUNK) {
            printf("extprobe: write failed at %u\n", off);
            close(fd);
            return 1;
        }
        off += CHUNK;
    }
    close(fd);

    fd = open("/disk/extprobe.bin", O_RDONLY);
    off = 0;
    while (off < TOTAL) {
        int n = read(fd, rd, CHUNK);
        if (n != CHUNK) { printf("extprobe: short read at %u (%d)\n", off, n); break; }
        for (unsigned i = 0; i < CHUNK; i++) {
            unsigned char want = (unsigned char)((off + i) * 2654435761u >> 24);
            if (rd[i] != want) {
                printf("extprobe: MISMATCH at offset %u (got %02x want %02x)\n",
                       off + i, rd[i], want);
                close(fd);
                return 1;
            }
        }
        off += CHUNK;
    }
    close(fd);
    unlink("/disk/extprobe.bin");
    printf("extprobe ok (%u bytes verified)\n", TOTAL);
    return 0;
}
