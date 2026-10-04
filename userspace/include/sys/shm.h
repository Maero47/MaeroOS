#pragma once
#include <sys/ipc.h>
#include <stddef.h>

#define SHM_RDONLY 010000
#define SHM_RND    020000
#define SHM_REMAP  040000
#define SHM_EXEC   0100000
#define SHM_LOCK   11
#define SHM_UNLOCK 12
#define SHM_STAT   13
#define SHM_INFO   14
#define SHM_STAT_ANY 15
#define SHM_DEST   01000
#define SHM_LOCKED 02000
#define SHMLBA     4096

typedef unsigned long shmatt_t;

struct shmid_ds {
    struct ipc_perm shm_perm;
    size_t shm_segsz;
    unsigned long shm_atime, __shm_atime_hi;
    unsigned long shm_dtime, __shm_dtime_hi;
    unsigned long shm_ctime, __shm_ctime_hi;
    pid_t shm_cpid, shm_lpid;
    unsigned long shm_nattch;
    unsigned long __unused4, __unused5;
};

struct shminfo {
    unsigned long shmmax, shmmin, shmmni, shmseg, shmall, __unused[4];
};

struct shm_info {
    int used_ids;
    unsigned long shm_tot, shm_rss, shm_swp, swap_attempts, swap_successes;
};

int shmget(key_t key, size_t size, int flag);
void *shmat(int id, const void *addr, int flag);
int shmdt(const void *addr);
int shmctl(int id, int cmd, struct shmid_ds *buf);
