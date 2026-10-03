#pragma once
#include <sys/ipc.h>
#include <stddef.h>
#include <time.h>

#define SEM_UNDO 0x1000
#define GETPID   11
#define GETVAL   12
#define GETALL   13
#define GETNCNT  14
#define GETZCNT  15
#define SETVAL   16
#define SETALL   17
#define SEM_STAT 18
#define SEM_INFO 19
#define SEM_STAT_ANY 20

struct semid_ds {
    struct ipc_perm sem_perm;
    unsigned long sem_otime, __sem_otime_hi;
    unsigned long sem_ctime, __sem_ctime_hi;
    unsigned long sem_nsems;
    unsigned long __unused3, __unused4;
};

struct seminfo {
    int semmap, semmni, semmns, semmnu, semmsl, semopm, semume, semusz, semvmx, semaem;
};

struct sembuf {
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

int semget(key_t key, int nsems, int flag);
int semctl(int id, int num, int cmd, ...);
int semop(int id, struct sembuf *ops, size_t n);
int semtimedop(int id, struct sembuf *ops, size_t n, const struct timespec *ts);
