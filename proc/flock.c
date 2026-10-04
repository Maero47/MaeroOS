#include "flock.h"
#include "process.h"
#include "scheduler.h"
#include "signal.h"
#include "syscall.h"       /* copy_to_user / copy_from_user */
#include "../fs/vfs.h"
#include "../mm/heap.h"
#include "../lib/string.h"

/* Advisory locking: flock(2), POSIX record locks and OFD locks (flock.h).
 * The lists and counts are under `flock_lock` (below); waiters sleep on
 * `lock_wq` with sleep_locked and are all woken whenever a lock goes away.
 *
 * Locks are hashed by file, so a close or a lock call walks only the locks
 * that can be on its file, not every lock in the system.  And they are
 * bounded: LOCKS_PER_UID per real uid (root is exempt) and LOCKS_MAX in all,
 * past which F_SETLK answers -ENOLCK.  Unbounded, one unprivileged loop of
 * non-mergeable byte-range locks ate the kernel heap and made every close()
 * in the system walk the whole list under the lock. */

enum { LK_FLOCK, LK_POSIX, LK_OFD };

#define F_RDLCK_K 0
#define F_WRLCK_K 1
#define F_UNLCK_K 2
#define OFF_MAX   0x7FFFFFFFFFFFFFFFLL

typedef struct flk {
    struct flk *next;
    /* The file: (st_dev, st_ino, filesystem) or, for a node without an
     * inode number, the node itself. */
    uint32_t    dev, ino;
    uintptr_t   fs;
    int         kind;
    int         type;          /* F_RDLCK / F_WRLCK */
    int64_t     start, end;    /* inclusive byte range; flock: whole file */
    uintptr_t   owner;         /* fid (flock, OFD) or fd table (POSIX) */
    int         pid;           /* reported by F_GETLK */
    uint32_t    uid;           /* real uid charged for it */
} flk_t;

#define LOCK_HASH      64
#define LOCKS_MAX      16384
#define LOCKS_PER_UID  4096
#define LOCK_UID_SLOTS 64

static flk_t *lock_hash[LOCK_HASH];
static int nlocks;
static int lock_wq;
/* Guards the lock table, the counts and the waiters' condition: set_lock
 * looks for a conflict and sleeps on lock_wq under it (sleep_locked), and
 * every release wakes lock_wq under it (docs/smp-plan.md stage 2a).  Taken
 * in thread context only; kmalloc/kfree run under it (flock -> heap). */
static kspinlock_t flock_lock = KSPINLOCK_INIT("flock");

/* Locks held per non-root uid.  A uid that finds no slot gets -ENOLCK too:
 * 64 distinct users holding locks at once is far beyond this system. */
static struct { uint32_t uid; int n; } lock_uids[LOCK_UID_SLOTS];

static int lock_charge(uint32_t uid) {
    if (nlocks >= LOCKS_MAX) return -37;                /* -ENOLCK */
    if (uid != 0) {
        int slot = -1;
        for (int i = 0; i < LOCK_UID_SLOTS && slot < 0; i++)
            if (lock_uids[i].n > 0 && lock_uids[i].uid == uid) slot = i;
        for (int i = 0; i < LOCK_UID_SLOTS && slot < 0; i++)
            if (lock_uids[i].n == 0) slot = i;
        if (slot < 0 || lock_uids[slot].n >= LOCKS_PER_UID) return -37;
        lock_uids[slot].uid = uid;
        lock_uids[slot].n++;
    }
    nlocks++;
    return 0;
}

static void lock_uncharge(uint32_t uid) {
    nlocks--;
    if (uid == 0) return;
    for (int i = 0; i < LOCK_UID_SLOTS; i++)
        if (lock_uids[i].n > 0 && lock_uids[i].uid == uid) { lock_uids[i].n--; return; }
}

/* A new lock record charged to `uid`, or NULL (-ENOLCK). */
static flk_t *flk_alloc(uint32_t uid) {
    if (lock_charge(uid) < 0) return NULL;
    flk_t *l = (flk_t *)kmalloc(sizeof(*l));
    if (!l) { lock_uncharge(uid); return NULL; }
    l->uid = uid;
    return l;
}

static void flk_free(flk_t *l) {
    lock_uncharge(l->uid);
    kfree(l);
}

typedef struct { uint32_t dev, ino; uintptr_t fs; } fkey_t;

static fkey_t key_of(vfs_node_t *n) {
    fkey_t k;
    if (n->inode) {
        k.dev = n->dev;
        k.ino = n->inode;
        /* ext2, tmpfs and the initrd all report st_dev 0: the driver's
         * read function tells their inode numbers apart. */
        k.fs  = (uintptr_t)n->read_fn;
    } else {
        k.dev = 0xFFFFFFFFu;
        k.ino = 0;
        k.fs  = (uintptr_t)n;
    }
    return k;
}

static int same_file(const flk_t *l, fkey_t k) {
    return l->dev == k.dev && l->ino == k.ino && l->fs == k.fs;
}

/* The list every lock on file `k` lives in. */
static flk_t **bucket(fkey_t k) {
    uint32_t h = k.dev * 2654435761u ^ k.ino * 40503u ^ (uint32_t)k.fs;
    h ^= h >> 16;
    return &lock_hash[h % LOCK_HASH];
}

static int bsd_kind(int kind) { return kind == LK_FLOCK; }

/* The lock `l` stands in the way of a `type` lock over [start, end] of
 * `kind` held by `owner`. */
static int conflicts(const flk_t *l, fkey_t k, int kind, uintptr_t owner,
                     int type, int64_t start, int64_t end) {
    if (!same_file(l, k) || bsd_kind(l->kind) != bsd_kind(kind))
        return 0;
    if (l->kind == kind && l->owner == owner)
        return 0;
    if (l->type == F_RDLCK_K && type == F_RDLCK_K)
        return 0;
    return l->start <= end && start <= l->end;
}

static flk_t *find_conflict(fkey_t k, int kind, uintptr_t owner, int type,
                            int64_t start, int64_t end) {
    for (flk_t *l = *bucket(k); l; l = l->next)
        if (conflicts(l, k, kind, owner, type, start, end))
            return l;
    return NULL;
}

/* Remove `owner`'s locks of `kind` on the file over [start, end], splitting
 * a lock that sticks out on both sides.  -ENOLCK when the split needs memory
 * there is none of. */
static int unlock_range(fkey_t k, int kind, uintptr_t owner,
                        int64_t start, int64_t end) {
    int woke = 0;
    for (flk_t **pp = bucket(k); *pp; ) {
        flk_t *l = *pp;
        if (!same_file(l, k) || l->kind != kind || l->owner != owner ||
            l->end < start || l->start > end) {
            pp = &l->next;
            continue;
        }
        woke = 1;
        if (l->start < start && l->end > end) {        /* punch a hole */
            flk_t *tail = flk_alloc(l->uid);
            if (!tail) return -37;                     /* -ENOLCK */
            *tail = *l;
            tail->start = end + 1;
            l->end = start - 1;
            tail->next = l->next;
            l->next = tail;
            break;
        }
        if (l->start < start) { l->end = start - 1; pp = &l->next; continue; }
        if (l->end > end)     { l->start = end + 1; pp = &l->next; continue; }
        *pp = l->next;
        flk_free(l);
    }
    if (woke) wake_up(&lock_wq);
    return 0;
}

/* Drop every lock `match` selects in one hash list (`only`), or in all. */
static void unlock_all(flk_t **only, int (*match)(const flk_t *, const void *),
                       const void *arg) {
    int woke = 0;
    for (int b = 0; b < LOCK_HASH; b++) {
        flk_t **head = only ? only : &lock_hash[b];
        for (flk_t **pp = head; *pp; ) {
            flk_t *l = *pp;
            if (match(l, arg)) {
                *pp = l->next;
                flk_free(l);
                woke = 1;
            } else {
                pp = &l->next;
            }
        }
        if (only) break;
    }
    if (woke) wake_up(&lock_wq);
}

/* Take a `type` lock (F_UNLCK releases) for `owner`, waiting for conflicting
 * locks to go unless `nb`. */
/* The descriptor `f` still holds the open file it held before a wait: a
 * thread sharing the table may have closed it (or closed it and opened
 * something else in the slot) while we slept. */
static int fd_unchanged(const proc_file_t *f, const vfs_node_t *node, uint32_t fid) {
    return f->type == FD_FILE && f->node == node && f->fid == fid;
}

/* Take a `type` lock (F_UNLCK releases) for `owner` on the file `f` holds,
 * waiting for conflicting locks to go unless `nb`.  A descriptor closed
 * during the wait gets no lock (-EBADF, as Linux fcntl_setlk answers that
 * race): inserted, it would belong to an owner nothing can release. */
static int set_lock_locked(proc_file_t *f, fkey_t k, int kind, uintptr_t owner, int type,
                           int64_t start, int64_t end, int nb);
static int set_lock(proc_file_t *f, fkey_t k, int kind, uintptr_t owner, int type,
                    int64_t start, int64_t end, int nb) {
    uint32_t fl = kspin_lock_irqsave(&flock_lock);
    int r = set_lock_locked(f, k, kind, owner, type, start, end, nb);
    kspin_unlock_irqrestore(&flock_lock, fl);
    return r;
}

static int set_lock_locked(proc_file_t *f, fkey_t k, int kind, uintptr_t owner, int type,
                           int64_t start, int64_t end, int nb) {
    if (type == F_UNLCK_K)
        return unlock_range(k, kind, owner, start, end);
    const vfs_node_t *node = f->node;
    uint32_t fid = f->fid;
    for (;;) {
        if (!find_conflict(k, kind, owner, type, start, end))
            break;
        if (nb) return -11;                            /* -EAGAIN */
        if (signal_interrupt_pending(current_proc)) return -4;
        sleep_locked(&lock_wq, &flock_lock);
        if (!fd_unchanged(f, node, fid)) return -9;    /* -EBADF */
    }
    flk_t *n = flk_alloc(current_proc ? current_proc->uid : 0);
    if (!n) return -37;                                /* -ENOLCK */
    /* The new lock replaces whatever the owner held over the range. */
    int r = unlock_range(k, kind, owner, start, end);
    if (r < 0) { flk_free(n); return r; }
    n->dev = k.dev; n->ino = k.ino; n->fs = k.fs;
    n->kind = kind;
    n->type = type;
    n->start = start;
    n->end = end;
    n->owner = owner;
    n->pid = current_proc ? current_proc->tgid : 0;
    flk_t **head = bucket(k);
    n->next = *head;
    *head = n;
    return 0;
}

int flock_bsd(proc_file_t *f, int op) {
    if (op & ~(1 | 2 | 4 | 8)) return -22;
    if (f->type != FD_FILE || !f->node)
        return f->type == FD_NONE ? -9 : -22;          /* sockets, pipes */
    int type;
    switch (op & ~4) {
    case 1: type = F_RDLCK_K; break;
    case 2: type = F_WRLCK_K; break;
    case 8: type = F_UNLCK_K; break;
    default: return -22;
    }
    /* The lock belongs to the open file description, which every copy of
     * the descriptor shares through its fid. */
    if (!f->fid) {
        if (type == F_UNLCK_K) return 0;
        f->fid = fd_new_fid();
    }
    fkey_t k = key_of(f->node);
    uint32_t fl = kspin_lock_irqsave(&flock_lock);
    /* Converting a lock drops the old one first (Linux flock_lock_inode):
     * a waiting converter does not keep its old lock. */
    for (flk_t *l = *bucket(k); l; l = l->next)
        if (same_file(l, k) && l->kind == LK_FLOCK && l->owner == f->fid) {
            if (l->type == type) { kspin_unlock_irqrestore(&flock_lock, fl); return 0; }
            unlock_range(k, LK_FLOCK, f->fid, 0, OFF_MAX);
            break;
        }
    int r = set_lock_locked(f, k, LK_FLOCK, f->fid, type, 0, OFF_MAX, (op & 4) != 0);
    kspin_unlock_irqrestore(&flock_lock, fl);
    return r;
}

/* struct flock on i386: short l_type, l_whence; then off_t (32-bit) or
 * loff_t (64-bit, 4-byte aligned) l_start, l_len; pid_t l_pid. */
typedef struct { int16_t type, whence; int32_t start, len; int32_t pid; } kflock32_t;
typedef struct __attribute__((packed)) {
    int16_t type, whence; int64_t start, len; int32_t pid;
} kflock64_t;

int flock_fcntl(proc_file_t *f, int cmd, void *uarg) {
    int wide = cmd >= 12;
    int ofd = cmd >= 36;
    int get = cmd == 5 || cmd == 12 || cmd == 36;
    int wait = cmd == 7 || cmd == 14 || cmd == 38;
    kflock64_t fl;
    if (!uarg) return -14;
    if (wide) {
        if (copy_from_user(&fl, uarg, sizeof(fl)) < 0) return -14;
    } else {
        kflock32_t s;
        if (copy_from_user(&s, uarg, sizeof(s)) < 0) return -14;
        fl.type = s.type; fl.whence = s.whence;
        fl.start = s.start; fl.len = s.len; fl.pid = s.pid;
    }
    if (f->type != FD_FILE || !f->node)
        return f->type == FD_NONE ? -9 : -22;
    if (fl.type != F_RDLCK_K && fl.type != F_WRLCK_K && fl.type != F_UNLCK_K)
        return -22;
    /* OFD locks take l_pid 0 (Linux: EINVAL otherwise). */
    if (ofd && fl.pid != 0) return -22;

    int64_t base;
    switch (fl.whence) {
    case 0: base = 0; break;
    case 1: base = (int64_t)f->offset; break;
    case 2: base = (int64_t)f->node->size; break;
    default: return -22;
    }
    /* Linux flock64_to_posix_lock: past OFFSET_MAX is -EOVERFLOW, before
     * offset 0 is -EINVAL.  Checked before adding: int64_t must not wrap. */
    if (fl.start > 0 && base > OFF_MAX - fl.start) return -75;   /* -EOVERFLOW */
    int64_t start = base + fl.start, end;
    if (start < 0) return -22;
    if (fl.len > 0) {
        if (fl.len - 1 > OFF_MAX - start) return -75;           /* -EOVERFLOW */
        end = start + fl.len - 1;
    } else if (fl.len < 0) {
        if (fl.len < -start) return -22;      /* would start before 0 */
        end = start - 1;
        start += fl.len;
    } else {
        end = OFF_MAX;
    }
    if (start < 0) return -22;

    int kind = ofd ? LK_OFD : LK_POSIX;
    uintptr_t owner;
    if (ofd) {
        if (!f->fid) f->fid = fd_new_fid();
        owner = f->fid;
    } else {
        owner = (uintptr_t)current_proc->fdt;
    }
    fkey_t k = key_of(f->node);

    if (get) {
        if (fl.type == F_UNLCK_K) return -22;
        uint32_t lfl = kspin_lock_irqsave(&flock_lock);
        flk_t *l = find_conflict(k, kind, owner, fl.type, start, end);
        kflock64_t out = fl;
        if (!l) {
            out.type = F_UNLCK_K;
        } else {
            out.type = (int16_t)l->type;
            out.whence = 0;
            out.start = l->start;
            out.len = l->end == OFF_MAX ? 0 : l->end - l->start + 1;
            out.pid = l->kind == LK_OFD ? -1 : l->pid;
        }
        kspin_unlock_irqrestore(&flock_lock, lfl);
        if (wide)
            return copy_to_user(uarg, &out, sizeof(out)) < 0 ? -14 : 0;
        if (out.type != F_UNLCK_K &&
            (out.start > 0x7FFFFFFF || out.len > 0x7FFFFFFF))
            return -75;                                /* -EOVERFLOW */
        kflock32_t s = { out.type, out.whence, (int32_t)out.start,
                         (int32_t)out.len, out.pid };
        return copy_to_user(uarg, &s, sizeof(s)) < 0 ? -14 : 0;
    }

    /* A read lock needs a descriptor open for reading, a write lock one
     * open for writing. */
    int acc = f->flags & O_ACCMODE;
    if (fl.type == F_RDLCK_K && acc == O_WRONLY) return -9;
    if (fl.type == F_WRLCK_K && acc == O_RDONLY) return -9;
    /* An identity for the descriptor, so a wait can tell it was closed. */
    if (wait && !f->fid) f->fid = fd_new_fid();
    return set_lock(f, k, kind, owner, fl.type, start, end, !wait);
}

/* Another descriptor of the open file description `fid` is still open
 * somewhere (`except` is the one being closed). */
static int fid_in_use(uint32_t fid, const proc_file_t *except) {
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_UNUSED || !p->ofile) continue;
        for (int fd = 0; fd < MAX_FD; fd++) {
            const proc_file_t *o = &p->ofile[fd];
            if (o != except && o->type != FD_NONE && o->fid == fid)
                return 1;
        }
    }
    return 0;
}

struct close_arg { fkey_t k; uintptr_t owner; int bsd_fid; };

static int match_posix_close(const flk_t *l, const void *a) {
    const struct close_arg *c = (const struct close_arg *)a;
    return l->kind == LK_POSIX && l->owner == c->owner && same_file(l, c->k);
}

static int match_fid(const flk_t *l, const void *a) {
    const struct close_arg *c = (const struct close_arg *)a;
    return l->kind != LK_POSIX && l->owner == c->owner && same_file(l, c->k);
}

static int match_table(const flk_t *l, const void *a) {
    return l->kind == LK_POSIX && l->owner == (uintptr_t)a;
}

void flock_fd_closed(proc_file_t *f) {
    if (!nlocks || f->type != FD_FILE || !f->node)
        return;
    struct close_arg c;
    c.k = key_of(f->node);
    flk_t **head = bucket(c.k);
    if (!*head) return;
    /* POSIX: closing any descriptor of the file drops the closing table's
     * locks on it.  Only a descriptor in the caller's own table counts;
     * one in flight over SCM_RIGHTS has no table. */
    struct fdtable *t = current_proc ? current_proc->fdt : NULL;
    uint32_t fl = kspin_lock_irqsave(&flock_lock);
    if (t && f >= t->f && f < t->f + MAX_FD) {
        c.owner = (uintptr_t)t;
        unlock_all(head, match_posix_close, &c);
    }
    /* flock and OFD: once the open file description's last descriptor
     * closes.  Its locks are all on this file, so on this hash list. */
    int held = 0;
    for (flk_t *l = *head; l && f->fid; l = l->next)
        if (l->kind != LK_POSIX && l->owner == f->fid && same_file(l, c.k)) {
            held = 1;
            break;
        }
    kspin_unlock_irqrestore(&flock_lock, fl);
    /* (The scan of every descriptor table runs only for a description
     * that holds such a lock, outside the lock: the tables are not its.) */
    if (held && !fid_in_use(f->fid, f)) {
        c.owner = f->fid;
        fl = kspin_lock_irqsave(&flock_lock);
        unlock_all(head, match_fid, &c);
        kspin_unlock_irqrestore(&flock_lock, fl);
    }
}

void flock_owner_gone(void *owner) {
    if (!nlocks) return;
    uint32_t fl = kspin_lock_irqsave(&flock_lock);
    unlock_all(NULL, match_table, owner);
    kspin_unlock_irqrestore(&flock_lock, fl);
}
