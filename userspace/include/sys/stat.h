#pragma once
#include <stdint.h>

#ifndef __MAEROS_TIMESPEC_DEFINED
#define __MAEROS_TIMESPEC_DEFINED
struct timespec { long tv_sec; long tv_nsec; };
#endif

/* File type bits in st_mode */
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* stat64 — what glibc/musl use on i386 via sys_stat64(195) */
struct stat {
    unsigned long long st_dev;
    unsigned char      __pad0[4];
    unsigned long      __st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned long      st_uid;
    unsigned long      st_gid;
    unsigned long long st_rdev;
    unsigned char      __pad3[4];
    long long          st_size;
    unsigned long      st_blksize;
    unsigned long long st_blocks;
    struct timespec    st_atim;
    struct timespec    st_mtim;
    struct timespec    st_ctim;
    unsigned long long st_ino;
} __attribute__((packed));

#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int lstat(const char *path, struct stat *buf);
int fstatat(int dirfd, const char *path, struct stat *buf, int flags);
int fchmodat(int dirfd, const char *path, int mode, int flags);
int chmod(const char *path, int mode);
int fchmod(int fd, int mode);
int fchown(int fd, int owner, int group);

int mkdir(const char *path, int mode);
int mkfifo(const char *path, int mode);
