#include "../include/dirent.h"
#include "../include/unistd.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include <stddef.h>

DIR *opendir(const char *path) {
    int fd = open(path, 0);   /* O_RDONLY = 0 */
    if (fd < 0) return (DIR *)0;
    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) { close(fd); return (DIR *)0; }
    d->fd      = fd;
    d->pos     = 0;
    d->buf_pos = 0;
    d->buf_len = 0;
    return d;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) return (struct dirent *)0;

    static struct dirent result;

    /* Refill buffer when empty */
    if (dirp->buf_pos >= dirp->buf_len) {
        int n = getdents64(dirp->fd, dirp->buf, sizeof(dirp->buf));
        if (n <= 0) return (struct dirent *)0;
        dirp->buf_len = n;
        dirp->buf_pos = 0;
    }

    /* Parse next linux_dirent64 entry from buffer */
    char *p = dirp->buf + dirp->buf_pos;
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;

    /* Hand-decode the packed struct (avoid alignment issues) */
    __builtin_memcpy(&d_ino,    p,      8);
    __builtin_memcpy(&d_off,    p + 8,  8);
    __builtin_memcpy(&d_reclen, p + 16, 2);
    __builtin_memcpy(&d_type,   p + 18, 1);

    result.d_ino    = d_ino;
    result.d_off    = d_off;
    result.d_reclen = d_reclen;
    result.d_type   = d_type;
    strncpy(result.d_name, p + 19, 255);
    result.d_name[255] = '\0';

    dirp->buf_pos += d_reclen;
    dirp->pos++;
    return &result;
}

int closedir(DIR *dirp) {
    if (!dirp) return -1;
    int r = close(dirp->fd);
    free(dirp);
    return r;
}
