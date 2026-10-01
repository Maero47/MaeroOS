#pragma once
#include <sys/types.h>

/* Linux i386 open() flags */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_ACCMODE  3
#define O_CREAT    0x040
#define O_EXCL     0x080
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800
#define O_CLOEXEC  0x80000

#define F_DUPFD    0
#define F_GETFD    1
#define F_SETFD    2
#define F_GETFL    3
#define F_SETFL    4
#define F_GETLK    5
#define F_SETLK    6
#define F_SETLKW   7
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

struct flock {
    short l_type;
    short l_whence;
    int l_start;
    int l_len;
    int l_pid;
};

/* These alias to open() for now */
#define creat(path, mode)  open((path), O_WRONLY | O_CREAT | O_TRUNC, (mode))

#define O_NOCTTY    0x100
#define O_DSYNC     0x1000
#define O_SYNC      0x101000
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#define O_LARGEFILE 0
#define O_NDELAY    O_NONBLOCK

#ifndef AT_FDCWD
#define AT_FDCWD            (-100)
#endif
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EACCESS          0x200

int posix_fallocate(int fd, off_t offset, off_t len);
int posix_fadvise(int fd, off_t offset, off_t len, int advice);
#define POSIX_FADV_NORMAL     0
#define POSIX_FADV_RANDOM     1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED   3
#define POSIX_FADV_DONTNEED   4
#define POSIX_FADV_NOREUSE    5
