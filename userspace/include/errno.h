#pragma once

/* errno is a global int set by syscall wrappers on error */
extern int errno;

/* POSIX error codes (Linux i386 numbers) */
#define EPERM    1   /* Operation not permitted */
#define ENOENT   2   /* No such file or directory */
#define ESRCH    3   /* No such process */
#define EINTR    4   /* Interrupted system call */
#define EIO      5   /* I/O error */
#define ENXIO    6   /* No such device or address */
#define E2BIG    7   /* Argument list too long */
#define ENOEXEC  8   /* Exec format error */
#define EBADF    9   /* Bad file number */
#define ECHILD  10   /* No child processes */
#define EAGAIN  11   /* Try again */
#define ENOMEM  12   /* Out of memory */
#define EACCES  13   /* Permission denied */
#define EFAULT  14   /* Bad address */
#define EBUSY   16   /* Device or resource busy */
#define EEXIST  17   /* File exists */
#define EXDEV   18   /* Cross-device link */
#define ENODEV  19   /* No such device */
#define ENOTDIR 20   /* Not a directory */
#define EISDIR  21   /* Is a directory */
#define EINVAL  22   /* Invalid argument */
#define ELOOP   40   /* Too many symbolic links */
#define ENFILE  23   /* File table overflow */
#define EMFILE  24   /* Too many open files */
#define ENOTTY  25   /* Not a typewriter */
#define EFBIG   27   /* File too large */
#define ENOSPC  28   /* No space left on device */
#define ESPIPE  29   /* Illegal seek */
#define EROFS   30   /* Read-only file system */
#define EMLINK  31   /* Too many links */
#define EPIPE   32   /* Broken pipe */
#define ERANGE  34   /* Math result not representable */
#define ENOSYS  38   /* Function not implemented */
#define ENOTEMPTY 39 /* Directory not empty */
#define ENOTSOCK 88  /* Socket operation on non-socket */
#define EDESTADDRREQ 89 /* Destination address required */
#define EMSGSIZE 90  /* Message too long */
#define EPROTONOSUPPORT 93 /* Protocol not supported */
#define ESOCKTNOSUPPORT 94 /* Socket type not supported */
#define EOPNOTSUPP 95 /* Operation not supported */
#define EAFNOSUPPORT 97 /* Address family not supported */
#define EADDRINUSE 98 /* Address already in use */
#define ENETDOWN 100 /* Network is down */
#define ENOTCONN 107 /* Transport endpoint is not connected */
