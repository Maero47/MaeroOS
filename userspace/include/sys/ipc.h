#pragma once
#include <sys/types.h>

/* System V IPC (kernel: proc/sysvipc.c).  The structures are the kernel's
 * IPC_64 layouts for i386, so libc/sysvipc.c passes them straight through. */
typedef int key_t;

struct ipc_perm {
    key_t __key;
    uid_t uid;
    gid_t gid;
    uid_t cuid;
    gid_t cgid;
    unsigned int mode;        /* 16-bit mode + padding */
    int __seq;                /* 16-bit seq + padding */
    long __unused1, __unused2;
};
#define __ipc_perm_key __key

#define IPC_PRIVATE ((key_t)0)
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_NOWAIT  04000
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define IPC_INFO    3
#define IPC_64      0x100

key_t ftok(const char *path, int id);
