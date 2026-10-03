/* System V IPC: shm, sem, msg.  See sysvipc.h. */
#include "sysvipc.h"
#include "process.h"
#include "scheduler.h"
#include "signal.h"
#include "syscall.h"
#include "shm.h"
#include "../arch/i686/mm/paging.h"
#include "../arch/i686/cpu/pit.h"
#include "../drivers/rtc.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../kernel/printk.h"
#include <kernel/config.h>
#include <stddef.h>

#define E_PERM    1
#define E_NOENT   2
#define E_INTR    4
#define E_2BIG    7
#define E_AGAIN   11
#define E_NOMEM   12
#define E_ACCES   13
#define E_FAULT   14
#define E_EXIST   17
#define E_INVAL   22
#define E_FBIG    27
#define E_NOSPC   28
#define E_RANGE   34
#define E_NOSYS   38
#define E_NOMSG   42
#define E_IDRM    43

#define IPC_PRIVATE 0
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_NOWAIT  04000
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define IPC_INFO    3
#define IPC_64      0x100
#define SHM_RDONLY  010000
#define SHM_RND     020000
#define SHM_REMAP   040000
#define SHM_LOCK    11
#define SHM_UNLOCK  12
#define SHM_STAT    13
#define SHM_INFO    14
#define SHM_STAT_ANY 15
#define SHM_DEST    01000
#define SHM_LOCKED  02000
#define SEM_UNDO    0x1000
#define GETPID      11
#define GETVAL      12
#define GETALL      13
#define GETNCNT     14
#define GETZCNT     15
#define SETVAL      16
#define SETALL      17
#define SEM_STAT    18
#define SEM_INFO    19
#define SEM_STAT_ANY 20
#define MSG_STAT    11
#define MSG_INFO    12
#define MSG_STAT_ANY 13
#define MSG_NOERROR 010000
#define MSG_EXCEPT  020000
#define MSG_COPY    040000

/* struct ipc64_perm (i386: asm-generic/ipcbuf.h, 16-bit mode and seq). */
typedef struct {
    int32_t  key;
    uint32_t uid, gid, cuid, cgid;
    uint16_t mode, pad1;
    uint16_t seq, pad2;
    uint32_t unused1, unused2;
} k_ipc64_perm;

typedef struct {
    int      key;
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode;
    uint16_t seq;
} ipc_perm_t;

static uint32_t now_sec(void) {
    return rtc_boot_epoch() + pit_ticks() / 100;
}

static int in_group(struct proc *c, uint32_t gid) {
    if (c->egid == gid) return 1;
    for (uint32_t i = 0; i < c->ngroups && i < PROC_NGROUPS_MAX; i++)
        if (c->groups[i] == gid) return 1;
    return 0;
}

/* Linux ipcperms(): `flag` holds the wanted rwx bits in any class position
 * (S_IRUGO-style 0444/0222 or a get's 0600). */
static int ipcperms(const ipc_perm_t *p, int flag) {
    struct proc *c = current_proc;
    int requested = (flag >> 6) | (flag >> 3) | flag;
    int granted = (int)p->mode;
    if (c->euid == p->cuid || c->euid == p->uid) granted >>= 6;
    else if (in_group(c, p->cgid) || in_group(c, p->gid)) granted >>= 3;
    if ((requested & ~granted & 07) && c->euid != 0) return -E_ACCES;
    return 0;
}

static int ipc_owner(const ipc_perm_t *p) {
    struct proc *c = current_proc;
    return c->euid == 0 || c->euid == p->uid || c->euid == p->cuid;
}

static void perm_init(ipc_perm_t *p, int key, int flag) {
    p->key = key;
    p->uid = p->cuid = current_proc->euid;
    p->gid = p->cgid = current_proc->egid;
    p->mode = (uint32_t)flag & 0777;
}

static void perm_out(const ipc_perm_t *p, k_ipc64_perm *o) {
    memset(o, 0, sizeof(*o));
    o->key = p->key;
    o->uid = p->uid; o->gid = p->gid; o->cuid = p->cuid; o->cgid = p->cgid;
    o->mode = (uint16_t)p->mode;
    o->seq = p->seq;
}

/* IPC_SET: new owner ids and the low 9 mode bits. */
static void perm_set(ipc_perm_t *p, const k_ipc64_perm *in) {
    p->uid = in->uid;
    p->gid = in->gid;
    p->mode = (p->mode & ~0777U) | (in->mode & 0777U);
}

#define ID_OF(slot, seq, n) ((int)((uint32_t)(seq) * (n) + (uint32_t)(slot)))

/* Sleep on `chan` until woken, a signal, or the deadline (ticks, 0 = none).
 * Returns 0 woken, -EINTR, or -EAGAIN on timeout. */
static int ipc_sleep(void *chan, uint32_t deadline) {
    if (signal_interrupt_pending(current_proc)) return -E_INTR;
    if (deadline) {
        if ((int32_t)(pit_ticks() - deadline) >= 0) return -E_AGAIN;
        current_proc->wake_tick = deadline;
    }
    int to = sleep_on(chan);
    if (to) return -E_AGAIN;
    if (signal_interrupt_pending(current_proc)) return -E_INTR;
    return 0;
}

/* ══ Shared memory ════════════════════════════════════════════════════════ */

typedef struct {
    int        used;
    ipc_perm_t perm;
    uint32_t   size, npages;
    uint32_t  *frames;
    int        nattch;
    uint32_t   atime, dtime, ctime;
    int        cpid, lpid;
} sysv_shm_t;

typedef struct {
    uint32_t mm;          /* address space; 0 = free */
    int      slot;
    uint16_t seq;
    uint32_t addr, npages;
} sysv_shm_att_t;

#define SHM_ATT_MAX 512

static sysv_shm_t shms[SYSV_SHMMNI];
static sysv_shm_att_t shm_atts[SHM_ATT_MAX];
static uint16_t shm_seq_next = 1;
static uint32_t shm_pages_total;

uint32_t sysvipc_shm_pages(void) { return shm_pages_total; }

static sysv_shm_t *shm_by_id(int id) {
    if (id < 0) return NULL;
    uint32_t slot = (uint32_t)id % SYSV_SHMMNI, seq = (uint32_t)id / SYSV_SHMMNI;
    sysv_shm_t *s = &shms[slot];
    if (!s->used || s->perm.seq != seq) return NULL;
    return s;
}

static void shm_destroy(sysv_shm_t *s) {
    for (uint32_t i = 0; i < s->npages; i++)
        if (s->frames[i]) pmm_frame_decref(s->frames[i]);
    shm_pages_total -= s->npages;
    kfree(s->frames);
    s->frames = NULL;
    s->used = 0;
}

static void shm_maybe_destroy(sysv_shm_t *s) {
    if (s->used && (s->perm.mode & SHM_DEST) && s->nattch <= 0)
        shm_destroy(s);
}

static int do_shmget(int key, uint32_t size, int flag) {
    if (key != IPC_PRIVATE) {
        for (int i = 0; i < SYSV_SHMMNI; i++) {
            sysv_shm_t *s = &shms[i];
            if (!s->used || s->perm.key != key || (s->perm.mode & SHM_DEST)) continue;
            if ((flag & IPC_CREAT) && (flag & IPC_EXCL)) return -E_EXIST;
            if (ipcperms(&s->perm, flag)) return -E_ACCES;
            if (size > s->size) return -E_INVAL;
            return ID_OF(i, s->perm.seq, SYSV_SHMMNI);
        }
        if (!(flag & IPC_CREAT)) return -E_NOENT;
    }
    if (size < 1 || size > SYSV_SHMMAX) return -E_INVAL;
    uint32_t np = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (shm_pages_total + np > SYSV_SHMALL) return -E_NOSPC;
    int slot = -1;
    for (int i = 0; i < SYSV_SHMMNI; i++)
        if (!shms[i].used) { slot = i; break; }
    if (slot < 0) return -E_NOSPC;
    uint32_t *frames = (uint32_t *)kmalloc(np * sizeof(uint32_t));
    if (!frames) return -E_NOMEM;
    memset(frames, 0, np * sizeof(uint32_t));
    for (uint32_t i = 0; i < np; i++) {
        uint32_t f = pmm_alloc_frame();
        if (!f) {
            for (uint32_t j = 0; j < i; j++) pmm_frame_decref(frames[j]);
            kfree(frames);
            return -E_NOMEM;
        }
        pmm_frame_incref(f);                  /* the segment's own reference */
        /* Zeroed: a segment must not leak a frame's earlier contents.
         * Temp mappings require IF=0. */
        __asm__ volatile("cli");
        memset(paging_temp_map(f), 0, PAGE_SIZE);
        paging_temp_unmap();
        __asm__ volatile("sti");
        frames[i] = f;
    }
    sysv_shm_t *s = &shms[slot];
    memset(s, 0, sizeof(*s));
    s->used = 1;
    perm_init(&s->perm, key, flag);
    s->perm.seq = shm_seq_next++;
    if (shm_seq_next >= 0x7FFF) shm_seq_next = 1;
    s->size = size;
    s->npages = np;
    s->frames = frames;
    s->ctime = now_sec();
    s->cpid = current_proc->tgid;
    shm_pages_total += np;
    return ID_OF(slot, s->perm.seq, SYSV_SHMMNI);
}

static int do_shmat(int id, uint32_t addr, int flag, uint32_t *out) {
    sysv_shm_t *s = shm_by_id(id);
    if (!s) return -E_INVAL;
    if (addr) {
        if (flag & SHM_RND) addr &= ~(PAGE_SIZE - 1);
        else if (addr & (PAGE_SIZE - 1)) return -E_INVAL;
    }
    int rdonly = (flag & SHM_RDONLY) != 0;
    if (ipcperms(&s->perm, rdonly ? 0444 : 0666)) return -E_ACCES;
    uint32_t mm = current_proc->pgdir_phys;
    sysv_shm_att_t *a = NULL;
    for (int i = 0; i < SHM_ATT_MAX; i++)
        if (!shm_atts[i].mm) { a = &shm_atts[i]; break; }
    if (!a) return -E_NOMEM;
    uint32_t base = mm_shm_attach_at(s->frames, s->npages, addr, rdonly);
    if (!base) return addr ? -E_INVAL : -E_NOMEM;
    a->mm = mm;
    a->slot = (int)(s - shms);
    a->seq = s->perm.seq;
    a->addr = base;
    a->npages = s->npages;
    s->nattch++;
    s->atime = now_sec();
    s->lpid = current_proc->tgid;
    *out = base;
    return 0;
}

static int do_shmdt(uint32_t addr) {
    uint32_t mm = current_proc->pgdir_phys;
    for (int i = 0; i < SHM_ATT_MAX; i++) {
        sysv_shm_att_t *a = &shm_atts[i];
        if (a->mm != mm || a->addr != addr) continue;
        sysv_shm_t *s = &shms[a->slot];
        a->mm = 0;
        if (!s->used || s->perm.seq != a->seq) return 0;
        mm_shm_detach(addr, s->frames, a->npages);
        s->nattch--;
        s->dtime = now_sec();
        s->lpid = current_proc->tgid;
        shm_maybe_destroy(s);
        return 0;
    }
    return -E_INVAL;
}

void sysv_shm_fork(struct proc *parent, struct proc *child) {
    uint32_t pm = parent->pgdir_phys, cm = child->pgdir_phys;
    if (!pm || !cm || pm == cm) return;
    for (int i = 0; i < SHM_ATT_MAX; i++) {
        if (shm_atts[i].mm != pm) continue;
        sysv_shm_t *s = &shms[shm_atts[i].slot];
        if (!s->used || s->perm.seq != shm_atts[i].seq) continue;
        for (int j = 0; j < SHM_ATT_MAX; j++) {
            if (shm_atts[j].mm) continue;
            shm_atts[j] = shm_atts[i];
            shm_atts[j].mm = cm;
            s->nattch++;
            break;
        }
    }
}

void sysv_shm_mm_release(uint32_t mm) {
    if (!mm) return;
    for (int i = 0; i < SHM_ATT_MAX; i++) {
        sysv_shm_att_t *a = &shm_atts[i];
        if (a->mm != mm) continue;
        a->mm = 0;
        sysv_shm_t *s = &shms[a->slot];
        if (!s->used || s->perm.seq != a->seq) continue;
        s->nattch--;
        s->dtime = now_sec();
        shm_maybe_destroy(s);
    }
}

typedef struct {
    k_ipc64_perm shm_perm;
    uint32_t shm_segsz;
    uint32_t shm_atime, shm_atime_high;
    uint32_t shm_dtime, shm_dtime_high;
    uint32_t shm_ctime, shm_ctime_high;
    int32_t  shm_cpid, shm_lpid;
    uint32_t shm_nattch;
    uint32_t unused4, unused5;
} k_shmid64_ds;

static int do_shmctl(int id, int cmd, void *ubuf) {
    cmd &= ~IPC_64;
    if (cmd == IPC_INFO) {
        uint32_t info[9] = { SYSV_SHMMAX, 1, SYSV_SHMMNI, SYSV_SHMMNI, SYSV_SHMALL, 0, 0, 0, 0 };
        if (copy_to_user(ubuf, info, sizeof(info)) < 0) return -E_FAULT;
        int hi = 0;
        for (int i = 0; i < SYSV_SHMMNI; i++) if (shms[i].used) hi = i;
        return hi;
    }
    if (cmd == SHM_INFO) {
        uint32_t info[6] = { 0, shm_pages_total, shm_pages_total, 0, 0, 0 };
        int hi = 0;
        for (int i = 0; i < SYSV_SHMMNI; i++) if (shms[i].used) { info[0]++; hi = i; }
        if (copy_to_user(ubuf, info, sizeof(info)) < 0) return -E_FAULT;
        return hi;
    }
    sysv_shm_t *s;
    int ret = 0;
    if (cmd == SHM_STAT || cmd == SHM_STAT_ANY) {
        if (id < 0 || id >= SYSV_SHMMNI || !shms[id].used) return -E_INVAL;
        s = &shms[id];
        ret = ID_OF(id, s->perm.seq, SYSV_SHMMNI);
    } else {
        s = shm_by_id(id);
        if (!s) return -E_INVAL;
    }
    switch (cmd) {
    case IPC_STAT: case SHM_STAT: case SHM_STAT_ANY: {
        if (cmd != SHM_STAT_ANY && ipcperms(&s->perm, 0444)) return -E_ACCES;
        k_shmid64_ds d;
        memset(&d, 0, sizeof(d));
        perm_out(&s->perm, &d.shm_perm);
        d.shm_segsz = s->size;
        d.shm_atime = s->atime; d.shm_dtime = s->dtime; d.shm_ctime = s->ctime;
        d.shm_cpid = s->cpid; d.shm_lpid = s->lpid;
        d.shm_nattch = (uint32_t)s->nattch;
        if (copy_to_user(ubuf, &d, sizeof(d)) < 0) return -E_FAULT;
        return ret;
    }
    case IPC_SET: {
        k_shmid64_ds d;
        if (copy_from_user(&d, ubuf, sizeof(d)) < 0) return -E_FAULT;
        if (!ipc_owner(&s->perm)) return -E_PERM;
        perm_set(&s->perm, &d.shm_perm);
        s->ctime = now_sec();
        return 0;
    }
    case IPC_RMID:
        if (!ipc_owner(&s->perm)) return -E_PERM;
        s->perm.mode |= SHM_DEST;
        s->perm.key = IPC_PRIVATE;           /* no longer found by key */
        s->ctime = now_sec();
        shm_maybe_destroy(s);
        return 0;
    case SHM_LOCK: case SHM_UNLOCK:
        if (!ipc_owner(&s->perm)) return -E_PERM;
        if (cmd == SHM_LOCK) s->perm.mode |= SHM_LOCKED;
        else s->perm.mode &= ~(uint32_t)SHM_LOCKED;
        return 0;
    }
    return -E_INVAL;
}

/* ══ Semaphores ═══════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t val;
    int      pid;
    uint16_t ncnt, zcnt;
} sysv_sem_val_t;

typedef struct {
    int             used;
    ipc_perm_t      perm;
    int             nsems;
    sysv_sem_val_t *sems;
    uint32_t        otime, ctime;
} sysv_semset_t;

/* One SEM_UNDO adjustment: process tgid, semaphore (set slot+seq, num). */
typedef struct {
    int      tgid;        /* 0 = free */
    int      slot;
    uint16_t seq;
    uint16_t num;
    int      adj;
} sysv_undo_t;

#define SEM_UNDO_MAX 1024

static sysv_semset_t semsets[SYSV_SEMMNI];
static sysv_undo_t undos[SEM_UNDO_MAX];
static uint16_t sem_seq_next = 1;
static uint32_t sem_total;

static sysv_semset_t *sem_by_id(int id) {
    if (id < 0) return NULL;
    uint32_t slot = (uint32_t)id % SYSV_SEMMNI, seq = (uint32_t)id / SYSV_SEMMNI;
    sysv_semset_t *s = &semsets[slot];
    if (!s->used || s->perm.seq != seq) return NULL;
    return s;
}

static void undo_clear(int slot, int num) {   /* num < 0: whole set */
    for (int i = 0; i < SEM_UNDO_MAX; i++)
        if (undos[i].tgid && undos[i].slot == slot && (num < 0 || undos[i].num == num))
            undos[i].tgid = 0;
}

static int do_semget(int key, int nsems, int flag) {
    if (key != IPC_PRIVATE) {
        for (int i = 0; i < SYSV_SEMMNI; i++) {
            sysv_semset_t *s = &semsets[i];
            if (!s->used || s->perm.key != key) continue;
            if ((flag & IPC_CREAT) && (flag & IPC_EXCL)) return -E_EXIST;
            if (ipcperms(&s->perm, flag)) return -E_ACCES;
            if (nsems > s->nsems) return -E_INVAL;
            return ID_OF(i, s->perm.seq, SYSV_SEMMNI);
        }
        if (!(flag & IPC_CREAT)) return -E_NOENT;
    }
    if (nsems <= 0 || nsems > SYSV_SEMMSL) return -E_INVAL;
    if (sem_total + (uint32_t)nsems > SYSV_SEMMNS) return -E_NOSPC;
    int slot = -1;
    for (int i = 0; i < SYSV_SEMMNI; i++)
        if (!semsets[i].used) { slot = i; break; }
    if (slot < 0) return -E_NOSPC;
    sysv_sem_val_t *v = (sysv_sem_val_t *)kmalloc((uint32_t)nsems * sizeof(*v));
    if (!v) return -E_NOMEM;
    memset(v, 0, (uint32_t)nsems * sizeof(*v));
    sysv_semset_t *s = &semsets[slot];
    memset(s, 0, sizeof(*s));
    s->used = 1;
    perm_init(&s->perm, key, flag);
    s->perm.seq = sem_seq_next++;
    if (sem_seq_next >= 0x7FFF) sem_seq_next = 1;
    s->nsems = nsems;
    s->sems = v;
    s->ctime = now_sec();
    sem_total += (uint32_t)nsems;
    undo_clear(slot, -1);
    return ID_OF(slot, s->perm.seq, SYSV_SEMMNI);
}

static void sem_remove(sysv_semset_t *s) {
    int slot = (int)(s - semsets);
    undo_clear(slot, -1);
    sem_total -= (uint32_t)s->nsems;
    kfree(s->sems);
    s->sems = NULL;
    s->used = 0;
    wake_up(s);                               /* sleepers see EIDRM */
}

typedef struct { uint16_t num; int16_t op; int16_t flg; } k_sembuf;

/* Try every operation at once.  Returns 0 (applied), 1 (would block; *blk
 * gets the index of the blocking op), or -ERANGE. */
static int sem_try(sysv_semset_t *s, const k_sembuf *ops, int n, int *blk) {
    for (int i = 0; i < n; i++) {
        int v = s->sems[ops[i].num].val;
        /* Earlier ops of this call on the same semaphore count. */
        for (int j = 0; j < i; j++)
            if (ops[j].num == ops[i].num) v += ops[j].op;
        if (ops[i].op > 0) {
            if (v + ops[i].op > SYSV_SEMVMX) return -E_RANGE;
        } else if (ops[i].op == 0) {
            if (v != 0) { *blk = i; return 1; }
        } else if (v + ops[i].op < 0) {
            *blk = i;
            return 1;
        }
    }
    return 0;
}

static int undo_add(int tgid, int slot, uint16_t seq, uint16_t num, int delta) {
    sysv_undo_t *fr = NULL;
    for (int i = 0; i < SEM_UNDO_MAX; i++) {
        sysv_undo_t *u = &undos[i];
        if (u->tgid == tgid && u->slot == slot && u->seq == seq && u->num == num) {
            u->adj += delta;
            if (!u->adj) u->tgid = 0;
            return 0;
        }
        if (!u->tgid && !fr) fr = u;
    }
    if (!delta) return 0;
    if (!fr) return -E_NOSPC;
    fr->tgid = tgid; fr->slot = slot; fr->seq = seq; fr->num = num; fr->adj = delta;
    return 0;
}

static int do_semtimedop(int id, const k_sembuf *uops, uint32_t nops,
                         int has_timeout, uint32_t timeout_ticks) {
    if (nops < 1) return -E_INVAL;
    if (nops > SYSV_SEMOPM) return -E_2BIG;
    k_sembuf ops[SYSV_SEMOPM];
    if (copy_from_user(ops, uops, nops * sizeof(k_sembuf)) < 0) return -E_FAULT;
    uint32_t deadline = 0;
    if (has_timeout) {
        deadline = pit_ticks() + timeout_ticks;
        if (!deadline) deadline = 1;
    }
    sysv_semset_t *s = sem_by_id(id);
    if (!s) return -E_INVAL;
    int alter = 0, undo = 0;
    for (uint32_t i = 0; i < nops; i++) {
        if (ops[i].num >= s->nsems) return -E_FBIG;
        if (ops[i].op) alter = 1;
        if (ops[i].flg & SEM_UNDO) undo = 1;
    }
    (void)undo;
    if (ipcperms(&s->perm, alter ? 0222 : 0444)) return -E_ACCES;
    uint16_t seq = s->perm.seq;
    int slot = (int)(s - semsets);
    for (;;) {
        int blk = -1;
        int r = sem_try(s, ops, (int)nops, &blk);
        if (r < 0) return r;
        if (r == 0) {
            int tgid = current_proc->tgid;
            for (uint32_t i = 0; i < nops; i++) {
                if (ops[i].flg & SEM_UNDO)
                    if (undo_add(tgid, slot, seq, ops[i].num, -ops[i].op) < 0) {
                        /* Roll back the undo entries made so far. */
                        for (uint32_t j = 0; j < i; j++)
                            if (ops[j].flg & SEM_UNDO)
                                undo_add(tgid, slot, seq, ops[j].num, ops[j].op);
                        return -E_NOSPC;
                    }
            }
            for (uint32_t i = 0; i < nops; i++) {
                s->sems[ops[i].num].val = (uint16_t)(s->sems[ops[i].num].val + ops[i].op);
                s->sems[ops[i].num].pid = current_proc->tgid;
            }
            s->otime = now_sec();
            if (alter) wake_up(s);
            return 0;
        }
        if (ops[blk].flg & IPC_NOWAIT) return -E_AGAIN;
        sysv_sem_val_t *sv = &s->sems[ops[blk].num];
        int zero = ops[blk].op == 0;
        if (zero) sv->zcnt++; else sv->ncnt++;
        int w = ipc_sleep(s, deadline);
        s = &semsets[slot];
        if (!s->used || s->perm.seq != seq) return -E_IDRM;
        sv = &s->sems[ops[blk].num];
        if (zero) { if (sv->zcnt) sv->zcnt--; } else { if (sv->ncnt) sv->ncnt--; }
        if (w) return w;
    }
}

void sysv_sem_exit(struct proc *p) {
    if (!p) return;
    int tgid = p->tgid;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q == p || q->tgid != tgid) continue;
        if (q->state == PROC_UNUSED || q->state == PROC_ZOMBIE) continue;
        return;                               /* the process lives on */
    }
    for (int i = 0; i < SEM_UNDO_MAX; i++) {
        sysv_undo_t *u = &undos[i];
        if (u->tgid != tgid) continue;
        u->tgid = 0;
        sysv_semset_t *s = &semsets[u->slot];
        if (!s->used || s->perm.seq != u->seq || u->num >= s->nsems) continue;
        int v = s->sems[u->num].val + u->adj;
        if (v < 0) v = 0;                     /* Linux clamps at 0 */
        if (v > SYSV_SEMVMX) v = SYSV_SEMVMX;
        s->sems[u->num].val = (uint16_t)v;
        s->sems[u->num].pid = tgid;
        s->ctime = now_sec();
        wake_up(s);
    }
}

typedef struct {
    k_ipc64_perm sem_perm;
    uint32_t sem_otime, sem_otime_high;
    uint32_t sem_ctime, sem_ctime_high;
    uint32_t sem_nsems;
    uint32_t unused3, unused4;
} k_semid64_ds;

static int do_semctl(int id, int num, int cmd, uint32_t arg) {
    cmd &= ~IPC_64;
    void *ubuf = (void *)(uintptr_t)arg;
    if (cmd == IPC_INFO || cmd == SEM_INFO) {
        /* struct seminfo: semmap semmni semmns semmnu semmsl semopm semume
         * semusz semvmx semaem; SEM_INFO reports usage in semusz/semaem. */
        int32_t info[10] = { SYSV_SEMMNI, SYSV_SEMMNI, SYSV_SEMMNS, SYSV_SEMMNS,
                             SYSV_SEMMSL, SYSV_SEMOPM, SYSV_SEMOPM, 20, SYSV_SEMVMX,
                             SYSV_SEMVMX };
        int hi = 0, used = 0;
        for (int i = 0; i < SYSV_SEMMNI; i++) if (semsets[i].used) { hi = i; used++; }
        if (cmd == SEM_INFO) { info[7] = used; info[9] = (int32_t)sem_total; }
        if (copy_to_user(ubuf, info, sizeof(info)) < 0) return -E_FAULT;
        return hi;
    }
    sysv_semset_t *s;
    int ret = 0;
    if (cmd == SEM_STAT || cmd == SEM_STAT_ANY) {
        if (id < 0 || id >= SYSV_SEMMNI || !semsets[id].used) return -E_INVAL;
        s = &semsets[id];
        ret = ID_OF(id, s->perm.seq, SYSV_SEMMNI);
    } else {
        s = sem_by_id(id);
        if (!s) return -E_INVAL;
    }
    int slot = (int)(s - semsets);
    switch (cmd) {
    case IPC_STAT: case SEM_STAT: case SEM_STAT_ANY: {
        if (cmd != SEM_STAT_ANY && ipcperms(&s->perm, 0444)) return -E_ACCES;
        k_semid64_ds d;
        memset(&d, 0, sizeof(d));
        perm_out(&s->perm, &d.sem_perm);
        d.sem_otime = s->otime;
        d.sem_ctime = s->ctime;
        d.sem_nsems = (uint32_t)s->nsems;
        if (copy_to_user(ubuf, &d, sizeof(d)) < 0) return -E_FAULT;
        return ret;
    }
    case IPC_SET: {
        k_semid64_ds d;
        if (copy_from_user(&d, ubuf, sizeof(d)) < 0) return -E_FAULT;
        if (!ipc_owner(&s->perm)) return -E_PERM;
        perm_set(&s->perm, &d.sem_perm);
        s->ctime = now_sec();
        return 0;
    }
    case IPC_RMID:
        if (!ipc_owner(&s->perm)) return -E_PERM;
        sem_remove(s);
        return 0;
    case GETVAL: case GETPID: case GETNCNT: case GETZCNT:
        if (ipcperms(&s->perm, 0444)) return -E_ACCES;
        if (num < 0 || num >= s->nsems) return -E_INVAL;
        return cmd == GETVAL ? s->sems[num].val : cmd == GETPID ? s->sems[num].pid
             : cmd == GETNCNT ? s->sems[num].ncnt : s->sems[num].zcnt;
    case GETALL: {
        if (ipcperms(&s->perm, 0444)) return -E_ACCES;
        for (int i = 0; i < s->nsems; i++) {
            uint16_t v = s->sems[i].val;
            if (copy_to_user((uint16_t *)ubuf + i, &v, 2) < 0) return -E_FAULT;
        }
        return 0;
    }
    case SETVAL: {
        if (ipcperms(&s->perm, 0222)) return -E_ACCES;
        if (num < 0 || num >= s->nsems) return -E_INVAL;
        int v = (int)arg;
        if (v < 0 || v > SYSV_SEMVMX) return -E_RANGE;
        s->sems[num].val = (uint16_t)v;
        s->sems[num].pid = current_proc->tgid;
        undo_clear(slot, num);
        s->ctime = now_sec();
        wake_up(s);
        return 0;
    }
    case SETALL: {
        if (ipcperms(&s->perm, 0222)) return -E_ACCES;
        uint16_t vals[SYSV_SEMMSL];
        if (copy_from_user(vals, ubuf, (uint32_t)s->nsems * 2) < 0) return -E_FAULT;
        for (int i = 0; i < s->nsems; i++)
            if (vals[i] > SYSV_SEMVMX) return -E_RANGE;
        for (int i = 0; i < s->nsems; i++) {
            s->sems[i].val = vals[i];
            s->sems[i].pid = current_proc->tgid;
        }
        undo_clear(slot, -1);
        s->ctime = now_sec();
        wake_up(s);
        return 0;
    }
    }
    return -E_INVAL;
}

/* ══ Message queues ═══════════════════════════════════════════════════════ */

typedef struct sysv_msg {
    struct sysv_msg *next;
    int32_t  type;
    uint32_t len;
    uint8_t  data[];
} sysv_msg_t;

typedef struct {
    int         used;
    ipc_perm_t  perm;
    sysv_msg_t *head, *tail;
    uint32_t    cbytes, qnum, qbytes;
    int         lspid, lrpid;
    uint32_t    stime, rtime, ctime;
} sysv_msq_t;

static sysv_msq_t msqs[SYSV_MSGMNI];
static uint16_t msg_seq_next = 1;

static sysv_msq_t *msq_by_id(int id) {
    if (id < 0) return NULL;
    uint32_t slot = (uint32_t)id % SYSV_MSGMNI, seq = (uint32_t)id / SYSV_MSGMNI;
    sysv_msq_t *q = &msqs[slot];
    if (!q->used || q->perm.seq != seq) return NULL;
    return q;
}

static int do_msgget(int key, int flag) {
    if (key != IPC_PRIVATE) {
        for (int i = 0; i < SYSV_MSGMNI; i++) {
            sysv_msq_t *q = &msqs[i];
            if (!q->used || q->perm.key != key) continue;
            if ((flag & IPC_CREAT) && (flag & IPC_EXCL)) return -E_EXIST;
            if (ipcperms(&q->perm, flag)) return -E_ACCES;
            return ID_OF(i, q->perm.seq, SYSV_MSGMNI);
        }
        if (!(flag & IPC_CREAT)) return -E_NOENT;
    }
    int slot = -1;
    for (int i = 0; i < SYSV_MSGMNI; i++)
        if (!msqs[i].used) { slot = i; break; }
    if (slot < 0) return -E_NOSPC;
    sysv_msq_t *q = &msqs[slot];
    memset(q, 0, sizeof(*q));
    q->used = 1;
    perm_init(&q->perm, key, flag);
    q->perm.seq = msg_seq_next++;
    if (msg_seq_next >= 0x7FFF) msg_seq_next = 1;
    q->qbytes = SYSV_MSGMNB;
    q->ctime = now_sec();
    return ID_OF(slot, q->perm.seq, SYSV_MSGMNI);
}

static void msq_remove(sysv_msq_t *q) {
    sysv_msg_t *m = q->head;
    while (m) { sysv_msg_t *n = m->next; kfree(m); m = n; }
    q->head = q->tail = NULL;
    q->used = 0;
    wake_up(q);
}

static int do_msgsnd(int id, const void *umsg, uint32_t sz, int flag) {
    if ((int32_t)sz < 0 || sz > SYSV_MSGMAX) return -E_INVAL;
    int32_t type;
    if (copy_from_user(&type, umsg, 4) < 0) return -E_FAULT;
    if (type < 1) return -E_INVAL;
    sysv_msg_t *m = (sysv_msg_t *)kmalloc(sizeof(*m) + sz);
    if (!m) return -E_NOMEM;
    m->next = NULL;
    m->type = type;
    m->len = sz;
    if (sz && copy_from_user(m->data, (const uint8_t *)umsg + 4, sz) < 0) {
        kfree(m);
        return -E_FAULT;
    }
    sysv_msq_t *q = msq_by_id(id);
    if (!q) { kfree(m); return -E_INVAL; }
    if (ipcperms(&q->perm, 0222)) { kfree(m); return -E_ACCES; }
    int slot = (int)(q - msqs);
    uint16_t seq = q->perm.seq;
    while (q->cbytes + sz > q->qbytes || q->qnum + 1 > q->qbytes) {
        if (flag & IPC_NOWAIT) { kfree(m); return -E_AGAIN; }
        int w = ipc_sleep(q, 0);
        q = &msqs[slot];
        if (!q->used || q->perm.seq != seq) { kfree(m); return -E_IDRM; }
        if (w) { kfree(m); return w; }
    }
    if (q->tail) q->tail->next = m; else q->head = m;
    q->tail = m;
    q->cbytes += sz;
    q->qnum++;
    q->lspid = current_proc->tgid;
    q->stime = now_sec();
    wake_up(q);
    return 0;
}

static int do_msgrcv(int id, void *umsg, uint32_t sz, int32_t type, int flag) {
    if ((int32_t)sz < 0) return -E_INVAL;
    if (flag & MSG_COPY) return -E_NOSYS;
    sysv_msq_t *q = msq_by_id(id);
    if (!q) return -E_INVAL;
    if (ipcperms(&q->perm, 0444)) return -E_ACCES;
    int slot = (int)(q - msqs);
    uint16_t seq = q->perm.seq;
    for (;;) {
        sysv_msg_t *m = NULL, *prev = NULL, *mprev = NULL;
        for (sysv_msg_t *it = q->head; it; prev = it, it = it->next) {
            int ok;
            if (type == 0) ok = 1;
            else if (type > 0) ok = (flag & MSG_EXCEPT) ? it->type != type : it->type == type;
            else ok = it->type <= -type && (!m || it->type < m->type);
            if (!ok) continue;
            m = it;
            mprev = prev;
            if (type >= 0) break;
        }
        if (m) {
            if (m->len > sz && !(flag & MSG_NOERROR)) return -E_2BIG;
            uint32_t n = m->len < sz ? m->len : sz;
            if (copy_to_user(umsg, &m->type, 4) < 0 ||
                (n && copy_to_user((uint8_t *)umsg + 4, m->data, n) < 0))
                return -E_FAULT;
            if (mprev) mprev->next = m->next; else q->head = m->next;
            if (q->tail == m) q->tail = mprev;
            q->cbytes -= m->len;
            q->qnum--;
            q->lrpid = current_proc->tgid;
            q->rtime = now_sec();
            kfree(m);
            wake_up(q);
            return (int)n;
        }
        if (flag & IPC_NOWAIT) return -E_NOMSG;
        int w = ipc_sleep(q, 0);
        q = &msqs[slot];
        if (!q->used || q->perm.seq != seq) return -E_IDRM;
        if (w) return w;
    }
}

typedef struct {
    k_ipc64_perm msg_perm;
    uint32_t msg_stime, msg_stime_high;
    uint32_t msg_rtime, msg_rtime_high;
    uint32_t msg_ctime, msg_ctime_high;
    uint32_t msg_cbytes, msg_qnum, msg_qbytes;
    int32_t  msg_lspid, msg_lrpid;
    uint32_t unused4, unused5;
} k_msqid64_ds;

static int do_msgctl(int id, int cmd, void *ubuf) {
    cmd &= ~IPC_64;
    if (cmd == IPC_INFO || cmd == MSG_INFO) {
        /* struct msginfo: msgpool msgmap msgmax msgmnb msgmni msgssz msgtql
         * msgseg; MSG_INFO reports queues/messages/bytes in use instead. */
        int32_t info[8] = { SYSV_MSGMNI * SYSV_MSGMNB / 1024, SYSV_MSGMNB, SYSV_MSGMAX,
                            SYSV_MSGMNB, SYSV_MSGMNI, 16, SYSV_MSGMNB, 0xFFFF };
        int hi = 0;
        uint32_t used = 0, msgs = 0, bytes = 0;
        for (int i = 0; i < SYSV_MSGMNI; i++)
            if (msqs[i].used) { hi = i; used++; msgs += msqs[i].qnum; bytes += msqs[i].cbytes; }
        if (cmd == MSG_INFO) { info[0] = (int32_t)used; info[1] = (int32_t)msgs; info[6] = (int32_t)bytes; }
        if (copy_to_user(ubuf, info, 30) < 0) return -E_FAULT;
        return hi;
    }
    sysv_msq_t *q;
    int ret = 0;
    if (cmd == MSG_STAT || cmd == MSG_STAT_ANY) {
        if (id < 0 || id >= SYSV_MSGMNI || !msqs[id].used) return -E_INVAL;
        q = &msqs[id];
        ret = ID_OF(id, q->perm.seq, SYSV_MSGMNI);
    } else {
        q = msq_by_id(id);
        if (!q) return -E_INVAL;
    }
    switch (cmd) {
    case IPC_STAT: case MSG_STAT: case MSG_STAT_ANY: {
        if (cmd != MSG_STAT_ANY && ipcperms(&q->perm, 0444)) return -E_ACCES;
        k_msqid64_ds d;
        memset(&d, 0, sizeof(d));
        perm_out(&q->perm, &d.msg_perm);
        d.msg_stime = q->stime; d.msg_rtime = q->rtime; d.msg_ctime = q->ctime;
        d.msg_cbytes = q->cbytes; d.msg_qnum = q->qnum; d.msg_qbytes = q->qbytes;
        d.msg_lspid = q->lspid; d.msg_lrpid = q->lrpid;
        if (copy_to_user(ubuf, &d, sizeof(d)) < 0) return -E_FAULT;
        return ret;
    }
    case IPC_SET: {
        k_msqid64_ds d;
        if (copy_from_user(&d, ubuf, sizeof(d)) < 0) return -E_FAULT;
        if (!ipc_owner(&q->perm)) return -E_PERM;
        if (d.msg_qbytes > SYSV_MSGMNB && current_proc->euid != 0) return -E_PERM;
        perm_set(&q->perm, &d.msg_perm);
        q->qbytes = d.msg_qbytes;
        q->ctime = now_sec();
        wake_up(q);
        return 0;
    }
    case IPC_RMID:
        if (!ipc_owner(&q->perm)) return -E_PERM;
        msq_remove(q);
        return 0;
    }
    return -E_INVAL;
}

/* ══ Entry points ═════════════════════════════════════════════════════════ */

/* timespec (32- or 64-bit seconds) → ticks; -EINVAL for a bad one. */
static int ts_ticks(const void *uts, int is64, uint32_t *ticks) {
    int64_t sec;
    int32_t nsec;
    if (is64) {
        int64_t t[2];
        if (copy_from_user(t, uts, sizeof(t)) < 0) return -E_FAULT;
        sec = t[0];
        nsec = (int32_t)t[1];
    } else {
        int32_t t[2];
        if (copy_from_user(t, uts, sizeof(t)) < 0) return -E_FAULT;
        sec = t[0];
        nsec = t[1];
    }
    if (sec < 0 || nsec < 0 || nsec >= 1000000000) return -E_INVAL;
    if (sec > 10000000) sec = 10000000;
    uint32_t t = (uint32_t)sec * 100U + ((uint32_t)nsec + 9999999U) / 10000000U;
    *ticks = t ? t : 1;
    return 0;
}

static int semtimedop_common(int id, uint32_t uops, uint32_t nops, uint32_t uts, int is64) {
    uint32_t ticks = 0;
    if (uts) {
        int r = ts_ticks((const void *)(uintptr_t)uts, is64, &ticks);
        if (r < 0) return r;
    }
    return do_semtimedop(id, (const k_sembuf *)(uintptr_t)uops, nops, uts != 0, ticks);
}

static int shmat_ret(int id, uint32_t addr, int flag) {
    uint32_t base;
    int r = do_shmat(id, addr, flag, &base);
    return r < 0 ? r : (int)base;
}

int sys_ipc(registers_t *regs) {
    uint32_t call = regs->ebx;
    int first = (int)regs->ecx;
    uint32_t second = regs->edx, third = regs->esi, ptr = regs->edi, fifth = regs->ebp;
    uint32_t version = call >> 16;
    switch (call & 0xFFFF) {
    case 1:  /* SEMOP */
        return semtimedop_common(first, ptr, second, 0, 0);
    case 4:  /* SEMTIMEDOP */
        return semtimedop_common(first, ptr, second, fifth, 0);
    case 2:  /* SEMGET */
        return do_semget(first, (int)second, (int)third);
    case 3: { /* SEMCTL: ptr points at the union semun */
        uint32_t arg = 0;
        if (ptr && copy_from_user(&arg, (const void *)(uintptr_t)ptr, 4) < 0) return -E_FAULT;
        if (!ptr) return -E_INVAL;
        return do_semctl(first, (int)second, (int)third, arg);
    }
    case 11: /* MSGSND */
        return do_msgsnd(first, (const void *)(uintptr_t)ptr, second, (int)third);
    case 12: { /* MSGRCV */
        if (version == 0) {
            uint32_t kl[2];                       /* struct ipc_kludge */
            if (!ptr) return -E_INVAL;
            if (copy_from_user(kl, (const void *)(uintptr_t)ptr, sizeof(kl)) < 0) return -E_FAULT;
            return do_msgrcv(first, (void *)(uintptr_t)kl[0], second, (int32_t)kl[1], (int)third);
        }
        return do_msgrcv(first, (void *)(uintptr_t)ptr, second, (int32_t)fifth, (int)third);
    }
    case 13: /* MSGGET */
        return do_msgget(first, (int)second);
    case 14: /* MSGCTL */
        return do_msgctl(first, (int)second, (void *)(uintptr_t)ptr);
    case 21: { /* SHMAT: the address goes to *third */
        if (version == 1) return -E_INVAL;
        uint32_t base;
        int r = do_shmat(first, ptr, (int)second, &base);
        if (r < 0) return r;
        if (copy_to_user((void *)(uintptr_t)third, &base, 4) < 0) return -E_FAULT;
        return 0;
    }
    case 22: /* SHMDT */
        return do_shmdt(ptr);
    case 23: /* SHMGET */
        return do_shmget(first, second, (int)third);
    case 24: /* SHMCTL */
        return do_shmctl(first, (int)second, (void *)(uintptr_t)ptr);
    }
    return -E_NOSYS;
}

int sysv_direct(registers_t *regs, int num) {
    uint32_t a = regs->ebx, b = regs->ecx, c = regs->edx, d = regs->esi, e = regs->edi;
    switch (num) {
    case 393: return do_semget((int)a, (int)b, (int)c);
    case 394: return do_semctl((int)a, (int)b, (int)c, d);
    case 395: return do_shmget((int)a, b, (int)c);
    case 396: return do_shmctl((int)a, (int)b, (void *)(uintptr_t)c);
    case 397: return shmat_ret((int)a, b, (int)c);
    case 398: return do_shmdt(a);
    case 399: return do_msgget((int)a, (int)b);
    case 400: return do_msgsnd((int)a, (const void *)(uintptr_t)b, c, (int)d);
    case 401: return do_msgrcv((int)a, (void *)(uintptr_t)b, c, (int32_t)d, (int)e);
    case 402: return do_msgctl((int)a, (int)b, (void *)(uintptr_t)c);
    case 420: return semtimedop_common((int)a, b, c, d, 1);   /* semtimedop_time64 */
    }
    return -E_NOSYS;
}

/* ══ /proc/sysvipc ════════════════════════════════════════════════════════ */

uint32_t sysvipc_proc_format(const char *which, char *buf, uint32_t cap) {
    uint32_t pos = 0;
#define OUT(...) do {                                                   \
        if (pos + 1 < cap) {                                            \
            int n_ = snprintf(buf + pos, cap - pos, __VA_ARGS__);       \
            if (n_ > 0) pos += (uint32_t)n_;                            \
            if (pos >= cap) pos = cap - 1;                              \
        }                                                               \
    } while (0)
    if (!strcmp(which, "shm")) {
        OUT("       key      shmid perms                  size  cpid  lpid nattch   uid   gid  cuid  cgid      atime      dtime      ctime                   rss                  swap\n");
        for (int i = 0; i < SYSV_SHMMNI; i++) {
            sysv_shm_t *s = &shms[i];
            if (!s->used) continue;
            OUT("%10d %10d  %4o %21u %5d %5d  %5d %5u %5u %5u %5u %10u %10u %10u %21u %21u\n",
                s->perm.key, ID_OF(i, s->perm.seq, SYSV_SHMMNI), s->perm.mode, s->size,
                s->cpid, s->lpid, s->nattch, s->perm.uid, s->perm.gid, s->perm.cuid,
                s->perm.cgid, s->atime, s->dtime, s->ctime, s->npages * PAGE_SIZE, 0U);
        }
    } else if (!strcmp(which, "sem")) {
        OUT("       key      semid perms      nsems   uid   gid  cuid  cgid      otime      ctime\n");
        for (int i = 0; i < SYSV_SEMMNI; i++) {
            sysv_semset_t *s = &semsets[i];
            if (!s->used) continue;
            OUT("%10d %10d  %4o %10d %5u %5u %5u %5u %10u %10u\n",
                s->perm.key, ID_OF(i, s->perm.seq, SYSV_SEMMNI), s->perm.mode, s->nsems,
                s->perm.uid, s->perm.gid, s->perm.cuid, s->perm.cgid, s->otime, s->ctime);
        }
    } else {
        OUT("       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid  cuid  cgid      stime      rtime      ctime\n");
        for (int i = 0; i < SYSV_MSGMNI; i++) {
            sysv_msq_t *q = &msqs[i];
            if (!q->used) continue;
            OUT("%10d %10d  %4o  %10u %10u %5d %5d %5u %5u %5u %5u %10u %10u %10u\n",
                q->perm.key, ID_OF(i, q->perm.seq, SYSV_MSGMNI), q->perm.mode, q->cbytes,
                q->qnum, q->lspid, q->lrpid, q->perm.uid, q->perm.gid, q->perm.cuid,
                q->perm.cgid, q->stime, q->rtime, q->ctime);
        }
    }
#undef OUT
    return pos;
}
