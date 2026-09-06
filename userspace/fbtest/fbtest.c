#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

static uint32_t rgb(unsigned r, unsigned g, unsigned b) {
    return (r << 16) | (g << 8) | b;
}

int main(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        printf("fbtest: /dev/fb0 unavailable\n");
        return 1;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        printf("fbtest: ioctl failed\n");
        close(fd);
        return 1;
    }

    printf("fbtest: %ux%u@%u pitch=%u\n",
           var.xres, var.yres, var.bits_per_pixel, fix.line_length);

    if (var.bits_per_pixel != 32 || var.xres == 0 || var.yres == 0 ||
        fix.line_length > 4096) {
        printf("fbtest: unsupported mode\n");
        close(fd);
        return 1;
    }

    static uint32_t row[1024];
    unsigned width = var.xres;
    if (width > 1024) width = 1024;

    for (unsigned y = 0; y < var.yres; y++) {
        uint32_t color;
        if (y < var.yres / 4) {
            color = rgb(0x20, 0x80, 0xff);
        } else if (y < var.yres / 2) {
            color = rgb(0x20, 0xb0, 0x60);
        } else if (y < (var.yres * 3) / 4) {
            color = rgb(0xf0, 0xc0, 0x40);
        } else {
            color = rgb(0xd0, 0x40, 0x40);
        }

        for (unsigned x = 0; x < width; x++) {
            row[x] = (x < 24 || y < 24 || x + 24 >= width ||
                      y + 24 >= var.yres) ? rgb(0xff, 0xff, 0xff) : color;
        }

        lseek(fd, (int)(y * fix.line_length), 0);
        if (write(fd, row, (int)(width * 4)) < 0) {
            printf("fbtest: write failed\n");
            close(fd);
            return 1;
        }
    }

    printf("fbtest ok\n");
    close(fd);
    return 0;
}
