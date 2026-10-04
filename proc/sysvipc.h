#pragma once
#include <stdint.h>
#include <registers.h>

struct proc;

/*
 * System V IPC: shared memory segments, semaphore sets and message queues
 * (Linux ipc/shm.c, ipc/sem.c, ipc/msg.c semantics; this implementation is
 * written from the man pages and the Linux UAPI structure layouts).
 *
 * Reached through the i386 ipc(2) multiplexer (117), which musl uses, and
 * through the direct syscalls 393-402 and semtimedop_time64 (420), which
 * glibc uses on kernels that have them.
 *
 * Objects are named by a key (IPC_PRIVATE always makes a new one) and an id
 * = seq * SLOTS + slot, so a removed object's id does not name its slot's
 * next occupant.  Each carries an ipc_perm (owner uid/gid, creator
 * cuid/cgid, mode) checked like Linux ipcperms(): owner bits for the owner or
 * creator, group bits for either group, root bypasses.  IPC_RMID by the
 * owner, creator or root: a shared memory segment that is still attached
 * lives on, marked SHM_DEST and no longer findable by key, until the last
 * detach; semaphore and message waiters wake with EIDRM.
 *
 * Shared memory is eager: a segment's frames are allocated (zeroed) at
 * shmget; every attachment maps them shared (proc/shm.c's address-space
 * helpers) and is recorded per address space, so fork copies attachments,
 * exec and exit drop them, and shm_nattch counts address spaces.
 *
 * SEM_UNDO adjustments are kept per process (thread group) and applied when
 * its last thread exits; SETVAL/SETALL clear them for the semaphores set.
 */

#define SYSV_SHMMNI   128
#define SYSV_SHMMAX   (32U * 1024 * 1024)        /* bytes per segment */
#define SYSV_SHMALL   16384U                     /* pages in all segments */
#define SYSV_SEMMNI   128
#define SYSV_SEMMSL   250
#define SYSV_SEMMNS   (SYSV_SEMMNI * 64)
#define SYSV_SEMOPM   100
#define SYSV_SEMVMX   32767
#define SYSV_MSGMNI   64
#define SYSV_MSGMAX   8192                       /* bytes per message */
#define SYSV_MSGMNB   16384                      /* bytes per queue */
#define SYSV_MSG_PER_QUEUE 1024                  /* messages per queue */
/* Pages of segments one non-root user may have created (by cuid; segments
 * outlive their creator, so this is what one user can pin). */
#define SYSV_SHM_USER_PAGES 4096U                /* 16 MiB */

/* ipc(2) and the direct syscalls; `num` is the syscall number. */
int sys_ipc(registers_t *regs);
int sysv_direct(registers_t *regs, int num);

/* Process lifetime hooks. */
void sysv_shm_fork(struct proc *parent, struct proc *child);
void sysv_shm_mm_release(uint32_t pgdir_phys);
void sysv_sem_exit(struct proc *p);

/* Pages held by all segments (/proc/meminfo Shmem). */
uint32_t sysvipc_shm_pages(void);

/* /proc/sysvipc/<which> ("shm", "sem", "msg"); returns the length. */
uint32_t sysvipc_proc_format(const char *which, char *buf, uint32_t cap);
