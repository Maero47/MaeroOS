/* System V IPC through the direct i386 syscalls (393-402, 420); the
 * structures in sys/{ipc,shm,sem,msg}.h are the kernel's IPC_64 layouts. */
#include "../include/sys/ipc.h"
#include "../include/sys/shm.h"
#include "../include/sys/sem.h"
#include "../include/sys/msg.h"
#include "../include/sys/stat.h"
#include "../include/errno.h"
#include "../include/syscall.h"
#include <stdarg.h>

static int chk(int r) {
    if (r < 0 && r > -4096) { errno = -r; return -1; }
    return r;
}

key_t ftok(const char *path, int id) {
    struct stat st;
    if (stat(path, &st) < 0) return -1;
    return (key_t)((st.st_ino & 0xffff) | ((st.st_dev & 0xff) << 16) | ((unsigned)(id & 0xff) << 24));
}

int shmget(key_t key, size_t size, int flag) { return chk(syscall3(395, key, (int)size, flag)); }
void *shmat(int id, const void *addr, int flag) {
    int r = syscall3(397, id, (int)addr, flag);
    if (r < 0 && r > -4096) { errno = -r; return (void *)-1; }
    return (void *)r;
}
int shmdt(const void *addr) { return chk(syscall1(398, (int)addr)); }
int shmctl(int id, int cmd, struct shmid_ds *buf) { return chk(syscall3(396, id, cmd | IPC_64, (int)buf)); }

int semget(key_t key, int nsems, int flag) { return chk(syscall3(393, key, nsems, flag)); }
int semctl(int id, int num, int cmd, ...) {
    int arg = 0;
    va_list ap;
    va_start(ap, cmd);
    switch (cmd & ~IPC_64) {
    case SETVAL: case GETALL: case SETALL: case IPC_STAT: case IPC_SET:
    case IPC_INFO: case SEM_INFO: case SEM_STAT: case SEM_STAT_ANY:
        arg = va_arg(ap, int);
        break;
    }
    va_end(ap);
    return chk(syscall4(394, id, num, cmd | IPC_64, arg));
}
int semtimedop(int id, struct sembuf *ops, size_t n, const struct timespec *ts) {
    long long t[2];
    if (ts) { t[0] = ts->tv_sec; t[1] = ts->tv_nsec; }
    return chk(syscall4(420, id, (int)ops, (int)n, ts ? (int)t : 0));
}
int semop(int id, struct sembuf *ops, size_t n) { return semtimedop(id, ops, n, 0); }

int msgget(key_t key, int flag) { return chk(syscall2(399, key, flag)); }
int msgsnd(int id, const void *msg, size_t size, int flag) {
    return chk(syscall4(400, id, (int)msg, (int)size, flag));
}
ssize_t msgrcv(int id, void *msg, size_t size, long type, int flag) {
    return chk(syscall5(401, id, (int)msg, (int)size, (int)type, flag));
}
int msgctl(int id, int cmd, struct msqid_ds *buf) { return chk(syscall3(402, id, cmd | IPC_64, (int)buf)); }
