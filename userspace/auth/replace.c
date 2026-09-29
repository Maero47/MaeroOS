#include "replace.h"
#include "../include/fcntl.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/unistd.h"

int maero_lock_down(int fd, int mode) {
    return fchown(fd, 0, 0) == 0 && fchmod(fd, mode) == 0;
}

static int write_all(int fd, const char *buf, int len) {
    while (len > 0) {
        int n = write(fd, buf, len);
        if (n <= 0)
            return 0;
        buf += n;
        len -= n;
    }
    return 1;
}

/* 1 if PATH holds exactly DATA. */
static int file_equals(const char *path, const char *data, int len) {
    char *buf = (char *)malloc(len + 1);
    int fd, got = 0, n = 0, same;

    if (!buf)
        return 0;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(buf);
        return 0;
    }
    /* Read one byte past LEN so a longer file does not compare equal. */
    while (got <= len && (n = read(fd, buf + got, len + 1 - got)) > 0)
        got += n;
    close(fd);
    same = n >= 0 && got == len && memcmp(buf, data, len) == 0;
    /* The copy may hold password hashes. */
    for (volatile char *v = buf; v < buf + len + 1; v++)
        *v = 0;
    free(buf);
    return same;
}

/* Create PATH afresh as root:MODE, write DATA, flush, and read it back. */
static int write_verified(const char *path, const char *data, int len, int mode) {
    int fd, ok;

    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC);
    if (fd < 0)
        return 0;
    ok = maero_lock_down(fd, mode) && write_all(fd, data, len) && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = 0;
    return ok && file_equals(path, data, len);
}

/* Re-check PATH after a rename attempt: lock it down, then compare. */
static int check_target(const char *path, int mode, const char *data, int len) {
    int fd = open(path, O_RDONLY);
    int locked;

    if (fd < 0)
        return 0;
    locked = maero_lock_down(fd, mode);
    close(fd);
    return locked && file_equals(path, data, len);
}

int maero_replace_file(const char *path, const char *tmp, const char *backup,
                       const char *old_data, int old_len,
                       const char *new_data, int new_len, int mode) {
    if (backup && !write_verified(backup, old_data, old_len, mode))
        return MAERO_UNCHANGED;
    if (!write_verified(tmp, new_data, new_len, mode)) {
        unlink(tmp);
        return MAERO_UNCHANGED;
    }

    /* The result of rename() is not trusted either way: a non-atomic rename
     * can fail after removing PATH, or succeed with the wrong mode.  What is
     * on disk afterwards decides. */
    rename(tmp, path);

    if (check_target(path, mode, new_data, new_len)) {
        unlink(tmp);
        return MAERO_REPLACED;
    }
    if (check_target(path, mode, old_data, old_len) ||
        write_verified(path, old_data, old_len, mode)) {
        unlink(tmp);
        return MAERO_UNCHANGED;
    }
    return MAERO_LOST;
}
