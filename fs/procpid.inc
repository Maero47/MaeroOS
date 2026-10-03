/* ── /proc/<pid>, /proc/self, /proc/thread-self ──────────────────────────────
 * Included by fs/procfs.c (it shares that file's text helpers).
 *
 * /proc/self is a symlink to "<tgid>" and /proc/thread-self to
 * "<tgid>/task/<tid>", as on Linux, so both name the caller's own directory
 * and every per-process file exists exactly once, under /proc/<pid>.
 *
 * Nodes.  A lookup hands back a vfs node that must stay valid for as long as
 * the walker uses it (stat and readlink never close what they looked up),
 * and an open file keeps it longer.  Every per-process node is a pnode_t:
 * one per (pid, kind, fd, thread view), made on first lookup and reused by
 * later ones.  Its contents are computed at read time from whatever process
 * holds that pid then; a pid is never reused, so a node never shows another
 * process.  A node cannot be freed while it is open (refs) or while a
 * syscall that looked it up is still running (walks: the thread records it
 * and lets go at the syscall's return, procfs_walk_done); any other node is
 * only a cache entry, made again by the next lookup.  See "node cache" below
 * for how many are kept and who pays for them.
 *
 * Permissions follow Linux's: a process's directory and most files belong
 * to its effective uid/gid (root when it is not dumpable); environ, auxv, io,
 * fd/, fdinfo/ and the exe/cwd/root links are 0400/0500 and also require
 * procfs_may_read() (ptrace read access), which maps checks at read time. */

enum {
    PK_DIR = 1, PK_TASKDIR, PK_FDDIR, PK_FDINFODIR,
    PK_STAT, PK_STATM, PK_STATUS, PK_CMDLINE, PK_ENVIRON, PK_AUXV, PK_COMM,
    PK_MAPS, PK_LIMITS, PK_IO, PK_MOUNTINFO, PK_MOUNTS, PK_CGROUP,
    PK_OOM_SCORE, PK_OOM_ADJ, PK_WCHAN, PK_LOGINUID, PK_SCHED,
    PK_EXE, PK_CWD, PK_ROOT,
    PK_FD, PK_FDINFO,
};

typedef struct pnode {
    vfs_node_t     v;          /* first: a vfs_node_t * is a pnode_t * */
    int            pid;        /* the task this node describes */
    int            arg;        /* fd number (PK_FD, PK_FDINFO) */
    uint8_t        kind;
    uint8_t        thread;     /* under task/<tid>: the thread's own view */
    uint8_t        bucket;     /* accounting bucket it is charged to */
    uint16_t       walks;      /* running syscalls that looked it up */
    int            refs;       /* open references (retain/close) */
    uint32_t       used_tick;  /* PIT tick of its last lookup or close */
    struct pnode  *hnext;      /* hash chain */
    /* With refs == 0 a node is on two LRU lists, most recent first: all such
     * nodes, and those of its bucket. */
    struct pnode  *gprev, *gnext, *bprev, *bnext;
} pnode_t;

/* Nodes cached now (/proc/sys/kernel/procfs_nodes).  Limits: */
static uint32_t pnode_count;
#define PNODE_USER_MAX 1024   /* charged to one non-root user */
#define PNODE_MAX      8192   /* in all, for lookups by non-root users */
#define PNODE_ROOT_MAX 9216   /* in all, for root's (the last 1024 are its); ~4 MiB */
#define PNODE_IDLE     300    /* ticks unused before the sweep frees a node */

/* Entries of a /proc/<pid> (and task/<tid>) directory. */
static const struct pent {
    const char *name;
    uint8_t     kind;
    uint8_t     type;      /* VFS_FLAG_* */
    uint16_t    mode;
    uint8_t     priv;      /* needs procfs_may_read */
    uint8_t     proc_only; /* not under task/<tid> */
} pid_ents[] = {
    { "stat",          PK_STAT,      VFS_FLAG_FILE,    0444, 0, 0 },
    { "statm",         PK_STATM,     VFS_FLAG_FILE,    0444, 0, 0 },
    { "status",        PK_STATUS,    VFS_FLAG_FILE,    0444, 0, 0 },
    { "cmdline",       PK_CMDLINE,   VFS_FLAG_FILE,    0444, 0, 0 },
    { "comm",          PK_COMM,      VFS_FLAG_FILE,    0644, 0, 0 },
    { "environ",       PK_ENVIRON,   VFS_FLAG_FILE,    0400, 1, 0 },
    { "auxv",          PK_AUXV,      VFS_FLAG_FILE,    0400, 1, 0 },
    { "maps",          PK_MAPS,      VFS_FLAG_FILE,    0444, 1, 0 },
    { "limits",        PK_LIMITS,    VFS_FLAG_FILE,    0444, 0, 0 },
    { "io",            PK_IO,        VFS_FLAG_FILE,    0400, 1, 0 },
    { "mountinfo",     PK_MOUNTINFO, VFS_FLAG_FILE,    0444, 0, 0 },
    { "mounts",        PK_MOUNTS,    VFS_FLAG_FILE,    0444, 0, 0 },
    { "cgroup",        PK_CGROUP,    VFS_FLAG_FILE,    0444, 0, 0 },
    { "oom_score",     PK_OOM_SCORE, VFS_FLAG_FILE,    0444, 0, 0 },
    { "oom_score_adj", PK_OOM_ADJ,   VFS_FLAG_FILE,    0644, 0, 0 },
    { "wchan",         PK_WCHAN,     VFS_FLAG_FILE,    0444, 0, 0 },
    { "loginuid",      PK_LOGINUID,  VFS_FLAG_FILE,    0644, 0, 0 },
    { "sched",         PK_SCHED,     VFS_FLAG_FILE,    0644, 0, 0 },
    { "exe",           PK_EXE,       VFS_FLAG_SYMLINK, 0777, 1, 0 },
    { "cwd",           PK_CWD,       VFS_FLAG_SYMLINK, 0777, 1, 0 },
    { "root",          PK_ROOT,      VFS_FLAG_SYMLINK, 0777, 1, 0 },
    { "fd",            PK_FDDIR,     VFS_FLAG_DIR,     0500, 1, 0 },
    { "fdinfo",        PK_FDINFODIR, VFS_FLAG_DIR,     0500, 1, 0 },
    { "task",          PK_TASKDIR,   VFS_FLAG_DIR,     0555, 0, 1 },
};
#define N_PID_ENTS (sizeof(pid_ents) / sizeof(pid_ents[0]))

/* The live (or zombie) task with this pid/tid. */
static struct proc *pp_task(int pid) {
    for (int i = 0; i < ptable_hwm && i < MAX_PROCS; i++)
        if (ptable[i].state != PROC_UNUSED && ptable[i].pid == pid)
            return &ptable[i];
    return NULL;
}

/* The thread-group leader of t (t itself when it has gone). */
static struct proc *pp_leader(struct proc *t) {
    struct proc *l = pp_task(t->tgid);
    return l ? l : t;
}

/* The process a node shows: the thread for a task/<tid> view, else the
 * thread-group leader of the task. */
static struct proc *pp_of(vfs_node_t *n) {
    pnode_t *pn = (pnode_t *)n;
    struct proc *t = pp_task(pn->pid);
    if (!t) return NULL;
    return pn->thread ? t : pp_leader(t);
}

int procfs_may_read(struct proc *t) {
    struct proc *c = current_proc;
    if (!c || !t) return 1;
    if (c->tgid == t->tgid) return 1;
    if (c->euid == 0) return 1;
    struct proc *l = pp_leader(t);
    if (t->nondumpable || l->nondumpable) return 0;
    uint32_t u = c->euid, g = c->egid;
    return u == t->uid && u == t->euid && u == t->suid &&
           g == t->gid && g == t->egid && g == t->sgid;
}

/* ── node cache ──
 *
 * Linux keeps no count: proc inodes and dentries are reclaimable cache that
 * the shrinkers free under memory pressure.  Here the heap is bounded by
 * count instead, and that bound must not let one user deny /proc to the
 * others.  So:
 *
 *  - nothing is kept for its recency alone.  A node nobody holds open and no
 *    running syscall has looked up can be freed at any time, oldest first,
 *    to make room; a lookup that needs room never fails while such a node
 *    exists.
 *  - every node is charged to one user: the last to look it up while it was
 *    not open, or whoever first opened, watched or chroot'ed to it (refused
 *    with EMFILE/ENOSPC when that user's share is all held; see
 *    pnode_may_pin).  A non-root user with PNODE_USER_MAX
 *    nodes charged frees one of its own before it gets another (and its
 *    lookup fails only when all of them are open or in use by its own
 *    syscalls), so one user can hold at most that many and the rest of
 *    PNODE_MAX is always there for the others.  Root is not limited per
 *    user and has PNODE_ROOT_MAX - PNODE_MAX to itself.
 *  - the sweep frees nodes unused for PNODE_IDLE ticks, so the count falls
 *    back once the lookups stop (dead processes' nodes and closed fd/N go
 *    the same way).
 *
 * Buckets: one per user with nodes charged, up to PN_USERS of them; users
 * past that share the spill bucket (and its limit), and root has its own,
 * unlimited.  A node's bucket is the one it is uncharged from, and a bucket
 * is given to another user only once nothing is charged to it. */
#define PN_USERS 64
#define PN_SPILL PN_USERS
#define PN_ROOT  (PN_USERS + 1)
static struct { uint32_t uid, n; pnode_t *head, *tail; } pn_acct[PN_USERS + 2];
static pnode_t *pn_ghead, *pn_gtail;
#define PN_HASH 512
static pnode_t *pn_hash[PN_HASH];

static uint32_t pn_hashof(int pid, int kind, int arg, int thread) {
    uint32_t h = (uint32_t)pid * 2654435761U ^ (uint32_t)arg * 40503U ^
                 (uint32_t)kind << 3 ^ (uint32_t)thread;
    return (h ^ h >> 15) % PN_HASH;
}

static void pn_lru_add(pnode_t *pn) {
    pn->gprev = NULL;
    pn->gnext = pn_ghead;
    if (pn_ghead) pn_ghead->gprev = pn; else pn_gtail = pn;
    pn_ghead = pn;
    __typeof__(pn_acct[0]) *a = &pn_acct[pn->bucket];
    pn->bprev = NULL;
    pn->bnext = a->head;
    if (a->head) a->head->bprev = pn; else a->tail = pn;
    a->head = pn;
}

static void pn_lru_del(pnode_t *pn) {
    if (pn->gprev) pn->gprev->gnext = pn->gnext; else pn_ghead = pn->gnext;
    if (pn->gnext) pn->gnext->gprev = pn->gprev; else pn_gtail = pn->gprev;
    __typeof__(pn_acct[0]) *a = &pn_acct[pn->bucket];
    if (pn->bprev) pn->bprev->bnext = pn->bnext; else a->head = pn->bnext;
    if (pn->bnext) pn->bnext->bprev = pn->bprev; else a->tail = pn->bprev;
    pn->gprev = pn->gnext = pn->bprev = pn->bnext = NULL;
}

/* The bucket uid's nodes are charged to. */
static int pn_bucket_of(uint32_t uid) {
    if (uid == 0) return PN_ROOT;
    int fr = -1;
    for (int i = 0; i < PN_USERS; i++) {
        if (pn_acct[i].n && pn_acct[i].uid == uid) return i;
        if (!pn_acct[i].n && fr < 0) fr = i;
    }
    return fr >= 0 ? fr : PN_SPILL;
}

/* Charge pn to bucket b (for uid), uncharging it from its old one; while it
 * is not open it moves to the head of b's list. */
static void pn_charge(pnode_t *pn, int b, uint32_t uid) {
    if (pn->bucket == b) return;
    int listed = pn->refs <= 0;
    if (listed) pn_lru_del(pn);
    if (pn_acct[pn->bucket].n) pn_acct[pn->bucket].n--;
    pn->bucket = (uint8_t)b;
    if (!pn_acct[b].n++) pn_acct[b].uid = uid;
    if (listed) pn_lru_add(pn);
}

static void pn_free(pnode_t *pn) {
    pnode_t **pp = &pn_hash[pn_hashof(pn->pid, pn->kind, pn->arg, pn->thread)];
    while (*pp && *pp != pn) pp = &(*pp)->hnext;
    if (*pp) *pp = pn->hnext;
    pn_lru_del(pn);
    if (pn_acct[pn->bucket].n) pn_acct[pn->bucket].n--;
    vfs_forget_last_lookup();             /* it may name this node */
    kfree(pn);
    pnode_count--;
}

/* Free the least recently used node of bucket b (b < 0: of all) that no
 * running syscall holds.  0 if there is none. */
static int pn_evict(int b) {
    pnode_t *pn = b < 0 ? pn_gtail : pn_acct[b].tail;
    while (pn && pn->walks) pn = b < 0 ? pn->gprev : pn->bprev;
    if (!pn) return 0;
    pn_free(pn);
    return 1;
}

/* Free the nodes unused for PNODE_IDLE ticks. */
static void pnode_sweep(uint32_t now) {
    pnode_t *pn = pn_gtail;
    while (pn && now - pn->used_tick > PNODE_IDLE) {
        pnode_t *prev = pn->gprev;
        if (!pn->walks) pn_free(pn);
        pn = prev;
    }
}

/* may_pin_fn: before an open, a watch or a chroot takes the first
 * reference, the node is charged to that user, who must have room for it
 * like for a lookup: one of its own unheld nodes goes to make it, and with
 * none left the pin is refused.  So the nodes held open count against the
 * share of whoever holds them, and nobody can hold nodes in another user's
 * share.  Mounts keep raw pointers, so a node that can be freed is never
 * one of their ends. */
static int pnode_may_pin(vfs_node_t *n, int how) {
    pnode_t *pn = (pnode_t *)n;
    if (how == VFS_PIN_MOUNT) return -22;                     /* -EINVAL */
    if (pn->refs > 0) return 0;              /* already charged to a holder */
    uint32_t uid = current_proc ? current_proc->euid : 0;
    int b = pn_bucket_of(uid);
    if (b == pn->bucket) return 0;
    if (b != PN_ROOT) {
        /* pn is in another bucket: making room in b cannot free it. */
        while (pn_acct[b].n >= PNODE_USER_MAX && pn_evict(b)) {}
        b = pn_bucket_of(uid);
        if (b != pn->bucket && pn_acct[b].n >= PNODE_USER_MAX)
            return how == VFS_PIN_WATCH ? -28 : -24;          /* ENOSPC, EMFILE */
    }
    pn_charge(pn, b, uid);
    return 0;
}

/* The first reference takes it off the LRU lists; it stays charged where
 * pnode_may_pin (or the last lookup) put it. */
static void pnode_retain(vfs_node_t *n) {
    pnode_t *pn = (pnode_t *)n;
    if (pn->refs++ > 0) return;
    pn_lru_del(pn);
}

static void pnode_close(vfs_node_t *n) {
    pnode_t *pn = (pnode_t *)n;
    if (pn->refs <= 0 || --pn->refs > 0) return;
    pn->used_tick = pit_ticks();
    pn_lru_add(pn);
}

/* Hold pn until the caller's syscall returns.  0 if the caller already
 * holds as many nodes as it can (a path that crosses that many per-process
 * nodes then fails). */
static int pn_walk_hold(pnode_t *pn) {
    struct proc *c = current_proc;
    if (!c) return 1;
    for (uint32_t i = 0; i < c->pn_nwalk; i++)
        if (c->pn_walk[i] == &pn->v) return 1;
    if (c->pn_nwalk >= sizeof(c->pn_walk) / sizeof(c->pn_walk[0])) return 0;
    c->pn_walk[c->pn_nwalk++] = &pn->v;
    pn->walks++;
    return 1;
}

void procfs_walk_done(struct proc *p) {
    for (uint32_t i = 0; i < p->pn_nwalk; i++) {
        pnode_t *pn = (pnode_t *)p->pn_walk[i];
        if (pn->walks) pn->walks--;
        p->pn_walk[i] = NULL;
    }
    p->pn_nwalk = 0;
}

/* Room for one more node looked up by uid: 0 if there is none. */
static int pn_make_room(uint32_t uid) {
    if (uid != 0) {
        int b = pn_bucket_of(uid);
        while (pn_acct[b].n >= PNODE_USER_MAX)
            if (!pn_evict(b)) return 0;
    }
    uint32_t max = uid ? PNODE_MAX : PNODE_ROOT_MAX;
    while (pnode_count >= max)
        if (!pn_evict(-1)) return 0;
    return 1;
}

static uint32_t pp_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf);
static uint32_t pp_write(vfs_node_t *n, uint32_t off, uint32_t len, const uint8_t *buf);
static int pp_dir_readdir(vfs_node_t *n, uint32_t idx, vfs_dirent_t *out);
static vfs_node_t *pp_dir_finddir(vfs_node_t *n, const char *name);
static int pp_task_readdir(vfs_node_t *n, uint32_t idx, vfs_dirent_t *out);
static vfs_node_t *pp_task_finddir(vfs_node_t *n, const char *name);
static int pp_fd_readdir(vfs_node_t *n, uint32_t idx, vfs_dirent_t *out);
static vfs_node_t *pp_fd_finddir(vfs_node_t *n, const char *name);

/* Owner of a task's /proc files: its effective ids while it is dumpable,
 * root otherwise (Linux task_dump_owner). */
static void pp_owner(struct proc *t, uint32_t *uid, uint32_t *gid) {
    struct proc *l = t ? pp_leader(t) : NULL;
    if (!t || t->nondumpable || (l && l->nondumpable)) { *uid = 0; *gid = 0; return; }
    *uid = t->euid;
    *gid = t->egid;
}

static uint32_t pp_inode(int pid, int kind, int arg, int thread) {
    return 0x40000000U | ((uint32_t)pid << 12) | ((uint32_t)thread << 11) |
           (((uint32_t)kind << 6) ^ (uint32_t)arg);
}

static vfs_node_t *pnode_get(int pid, int kind, int arg, int thread,
                             const char *name, uint32_t type, uint32_t mode) {
    static uint32_t sweep_tick;
    uint32_t now = pit_ticks();
    if (now - sweep_tick > 100) { sweep_tick = now; pnode_sweep(now); }

    struct proc *t = pp_task(pid);
    if (!t) return NULL;
    uint32_t uid = current_proc ? current_proc->euid : 0;
    uint32_t h = pn_hashof(pid, kind, arg, thread);
    pnode_t *pn;
    for (pn = pn_hash[h]; pn; pn = pn->hnext)
        if (pn->pid == pid && pn->kind == kind && pn->arg == arg &&
            pn->thread == thread)
            break;
    if (pn) {
        if (!pn_walk_hold(pn)) return NULL;
        if (pn->refs <= 0) {
            /* Charged to this user if it has room (pn is in another
             * bucket, so making room cannot free it), and to the front. */
            int b = pn_bucket_of(uid);
            if (b != pn->bucket) {
                while (b != PN_ROOT && pn_acct[b].n >= PNODE_USER_MAX && pn_evict(b)) {}
                b = pn_bucket_of(uid);
                if (b != pn->bucket && (b == PN_ROOT || pn_acct[b].n < PNODE_USER_MAX))
                    pn_charge(pn, b, uid);
            }
            pn_lru_del(pn);
            pn_lru_add(pn);
        }
    } else {
        struct proc *c = current_proc;
        if (c && c->pn_nwalk >= sizeof(c->pn_walk) / sizeof(c->pn_walk[0]))
            return NULL;
        if (!pn_make_room(uid)) return NULL;
        pn = (pnode_t *)kmalloc(sizeof(*pn));
        if (!pn) return NULL;
        pnode_count++;
        memset(pn, 0, sizeof(*pn));
        pn->pid = pid;
        pn->kind = (uint8_t)kind;
        pn->arg = arg;
        pn->thread = (uint8_t)thread;
        strncpy(pn->v.name, name, 255);
        pn->v.flags = type;
        pn->v.inode = pp_inode(pid, kind, arg, thread);
        pn->v.retain_fn = pnode_retain;
        pn->v.close_fn = pnode_close;
        pn->v.may_pin_fn = pnode_may_pin;
        pn->v.private = pn;
        switch (kind) {
        case PK_DIR:
            pn->v.readdir_fn = pp_dir_readdir;
            pn->v.finddir_fn = pp_dir_finddir;
            break;
        case PK_TASKDIR:
            pn->v.readdir_fn = pp_task_readdir;
            pn->v.finddir_fn = pp_task_finddir;
            break;
        case PK_FDDIR: case PK_FDINFODIR:
            pn->v.readdir_fn = pp_fd_readdir;
            pn->v.finddir_fn = pp_fd_finddir;
            break;
        default:
            pn->v.read_fn = pp_read;
            if (kind == PK_COMM || kind == PK_OOM_ADJ || kind == PK_LOGINUID ||
                kind == PK_SCHED)
                pn->v.write_fn = pp_write;
            break;
        }
        int b = pn_bucket_of(uid);
        pn->bucket = (uint8_t)b;
        if (!pn_acct[b].n++) pn_acct[b].uid = uid;
        pn->hnext = pn_hash[h];
        pn_hash[h] = pn;
        pn_lru_add(pn);
        pn_walk_hold(pn);
    }
    pn->used_tick = now;
    /* Ownership follows the task's credentials as they are now. */
    uint32_t ouid, ogid;
    pp_owner(thread ? t : pp_leader(t), &ouid, &ogid);
    pn->v.uid = ouid;
    pn->v.gid = ogid;
    pn->v.mask = mode;
    pn->v.nlink = type == VFS_FLAG_DIR ? 2 : 1;
    pn->v.mtime = pn->v.ctime = pn->v.atime = rtc_boot_epoch() + t->start_tick / 100;
    return &pn->v;
}

/* /proc/<pid> itself (thread = 0) or /proc/<pid>/task/<tid> (thread = 1). */
static vfs_node_t *procfs_pid_node(int pid, int thread) {
    char name[16];
    snprintf(name, sizeof(name), "%d", pid);
    return pnode_get(pid, PK_DIR, 0, thread, name, VFS_FLAG_DIR, 0555);
}

static int pp_dir_readdir(vfs_node_t *n, uint32_t idx, vfs_dirent_t *out) {
    pnode_t *pn = (pnode_t *)n;
    uint32_t k = 0;
    for (uint32_t i = 0; i < N_PID_ENTS; i++) {
        if (pn->thread && pid_ents[i].proc_only) continue;
        if (k++ != idx) continue;
        out->ino = pp_inode(pn->pid, pid_ents[i].kind, 0, pn->thread);
        out->type = pid_ents[i].type;
        strncpy(out->name, pid_ents[i].name, 255);
        out->name[255] = '\0';
        return 0;
    }
    return -1;
}

static vfs_node_t *pp_dir_finddir(vfs_node_t *n, const char *name) {
    pnode_t *pn = (pnode_t *)n;
    for (uint32_t i = 0; i < N_PID_ENTS; i++) {
        if (pn->thread && pid_ents[i].proc_only) continue;
        if (strcmp(name, pid_ents[i].name) != 0) continue;
        return pnode_get(pn->pid, pid_ents[i].kind, 0, pn->thread,
                         pid_ents[i].name, pid_ents[i].type, pid_ents[i].mode);
    }
    return NULL;
}

/* task/: one directory per live thread of the group. */
static int pp_task_readdir(vfs_node_t *n, uint32_t idx, vfs_dirent_t *out) {
    struct proc *t = pp_task(((pnode_t *)n)->pid);
    if (!t) return -1;
    uint32_t k = 0;
    for (int i = 0; i < ptable_hwm && i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q->state == PROC_UNUSED || q->tgid != t->tgid) continue;
        if (q->state == PROC_ZOMBIE && q->pid != q->tgid) continue;
        if (k++ != idx) continue;
        out->ino = pp_inode(q->pid, PK_DIR, 0, 1);
        out->type = VFS_FLAG_DIR;
        snprintf(out->name, sizeof(out->name), "%d", q->pid);
        return 0;
    }
    return -1;
}

static vfs_node_t *pp_task_finddir(vfs_node_t *n, const char *name) {
    struct proc *t = pp_task(((pnode_t *)n)->pid);
    int tid = procfs_parse_pid(name);
    if (!t || tid <= 0) return NULL;
    struct proc *q = pp_task(tid);
    if (!q || q->tgid != t->tgid) return NULL;
    return procfs_pid_node(tid, 1);
}

/* fd/ and fdinfo/: the open descriptors, for those allowed to see them. */
static int pp_fd_readdir(vfs_node_t *n, uint32_t idx, vfs_dirent_t *out) {
    pnode_t *pn = (pnode_t *)n;
    struct proc *t = pp_of(n);
    if (!t || !t->ofile || !procfs_may_read(t)) return -1;
    uint32_t k = 0;
    for (int i = 0; i < MAX_FD; i++) {
        if (t->ofile[i].type == FD_NONE) continue;
        if (k++ != idx) continue;
        out->ino = pp_inode(pn->pid, pn->kind == PK_FDDIR ? PK_FD : PK_FDINFO, i, pn->thread);
        out->type = pn->kind == PK_FDDIR ? VFS_FLAG_SYMLINK : VFS_FLAG_FILE;
        snprintf(out->name, sizeof(out->name), "%d", i);
        return 0;
    }
    return -1;
}

static vfs_node_t *pp_fd_finddir(vfs_node_t *n, const char *name) {
    pnode_t *pn = (pnode_t *)n;
    struct proc *t = pp_of(n);
    if (!t || !t->ofile || !procfs_may_read(t)) return NULL;
    int fd = 0;
    const char *p = name;
    if (*p < '0' || *p > '9') return NULL;
    while (*p >= '0' && *p <= '9') {
        fd = fd * 10 + (*p++ - '0');
        if (fd >= MAX_FD) return NULL;
    }
    if (*p) return NULL;
    proc_file_t *f = &t->ofile[fd];
    if (f->type == FD_NONE) return NULL;
    if (pn->kind == PK_FDINFODIR)
        return pnode_get(pn->pid, PK_FDINFO, fd, pn->thread, name, VFS_FLAG_FILE, 0400);

    /* For a regular file (incl. memfd/tmpfs), the REAL underlying node, so
     * that opening /proc/self/fd/N yields the SAME file object.  Firefox's
     * shared-memory Freeze() reopens /proc/self/fd/N read-only (DupReadOnly)
     * and maps it MAP_SHARED — it MUST share frames with the original fd.
     * readlink() of the link is answered in proc/syscall.c
     * (proc_fd_readlink) from the descriptor's path. */
    if (f->type == FD_FILE && f->node) {
        /* A chrooted process gets no directory through here: /proc is passed
         * through into the chroot, and a directory opened before chroot()
         * would let a walk like /proc/self/fd/3/etc/shadow continue in the
         * old tree (Linux allows that escape; MaeroOS does not).  Nor does
         * anyone get into another chroot's directories this way. */
        if (f->node->flags == VFS_FLAG_DIR &&
            (current_proc->root_node || t->root_node))
            return NULL;
        return f->node;
    }
    /* Pipes, sockets, epoll, eventfd: a placeholder (cannot be reopened). */
    return pnode_get(pn->pid, PK_FD, fd, pn->thread, name, VFS_FLAG_FILE, 0700);
}

/* ── text builder ── */

typedef struct { char *b; uint32_t pos, cap; } pbuf_t;

static void pb_printf(pbuf_t *pb, const char *fmt, ...) {
    if (pb->pos + 1 >= pb->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(pb->b + pb->pos, pb->cap - pb->pos, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    pb->pos += (uint32_t)n;
    if (pb->pos >= pb->cap) pb->pos = pb->cap - 1;
}

static void pb_bytes(pbuf_t *pb, const void *src, uint32_t len) {
    if (pb->pos + len > pb->cap) len = pb->cap - pb->pos;
    memcpy(pb->b + pb->pos, src, len);
    pb->pos += len;
}

/* Linux encodes tty_nr as (major << 8) | minor with the high minor bits
 * above; this kernel's terminals are tty1.., ttyS0.., console and pts/N. */
static uint32_t pp_tty_nr(struct proc *p) {
    if (!p || !p->ctty) return 0;
    const char *nm = p->ctty->name;
    if (nm[0] >= '0' && nm[0] <= '9') {
        uint32_t n = 0;
        while (*nm >= '0' && *nm <= '9') n = n * 10 + (uint32_t)(*nm++ - '0');
        return (136U << 8) | (n & 0xFF) | ((n & ~0xFFU) << 12);
    }
    if (!strncmp(nm, "ttyS", 4)) return (4U << 8) | (64U + (uint32_t)(nm[4] - '0'));
    if (!strncmp(nm, "tty", 3) && nm[3] >= '0' && nm[3] <= '9') {
        uint32_t n = 0;
        nm += 3;
        while (*nm >= '0' && *nm <= '9') n = n * 10 + (uint32_t)(*nm++ - '0');
        return (4U << 8) | (n & 0xFF);
    }
    if (!strcmp(nm, "console")) return (5U << 8) | 1U;
    return (4U << 8) | 1U;
}

/* Threads of t's group that have not exited. */
static int pp_nthreads(struct proc *t) {
    int n = 0;
    for (int i = 0; i < ptable_hwm && i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q->state == PROC_UNUSED || q->tgid != t->tgid) continue;
        if (q->state == PROC_ZOMBIE && q->pid != q->tgid) continue;
        n++;
    }
    return n ? n : 1;
}

/* Address-space figures, in pages: total size, resident, shared, text,
 * data+stack. */
typedef struct { uint32_t size, rss, shared, text, data, stack, heap; } pp_mem_t;

static void pp_mem(struct proc *t, pp_mem_t *m) {
    memset(m, 0, sizeof(*m));
    if (!t || !t->pgdir_phys) return;
    struct proc *own = t->vm_owner ? t->vm_owner : pp_leader(t);
    m->text = own->image_end > own->image_start
            ? (own->image_end - own->image_start) / PAGE_SIZE : 0;
    m->heap = own->heap_end > own->brk_base
            ? (own->heap_end - own->brk_base + PAGE_SIZE - 1) / PAGE_SIZE : 0;
    m->stack = USER_STACK_PAGES;
    uint32_t vs, ve, vp, anon = 0, total = 0;
    int vsh;
    const char *vname;
    for (int i = 0; proc_vma_iter_ex(own, i, &vs, &ve, &vp, &vsh, &vname) == 0; i++) {
        total += (ve - vs) / PAGE_SIZE;
        if (!vsh && (!vname || !vname[0])) anon += (ve - vs) / PAGE_SIZE;
    }
    m->size = m->text + m->heap + m->stack + total;
    m->data = m->heap + anon;
    pgdir_count_resident(t->pgdir_phys, &m->rss, &m->shared);
}

/* CPU time in clock ticks (USER_HZ 100): the thread's, or the whole group's;
 * split into user and system by the tick samples. */
static void pp_times(struct proc *t, int group, uint32_t *ut, uint32_t *st) {
    uint64_t ns = 0;
    uint32_t ticks = 0, sticks = 0;
    for (int i = 0; i < ptable_hwm && i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q->state == PROC_UNUSED) continue;
        if (group ? q->tgid != t->tgid : q != t) continue;
        ns += q->run_ns_total;
        ticks += q->utime_ticks;
        sticks += q->stime_ticks;
    }
    uint32_t total = (uint32_t)(ns / 10000000ULL);
    uint32_t sys = ticks ? (uint32_t)((uint64_t)total * sticks / ticks) : 0;
    *ut = total - sys;
    *st = sys;
}

static char pp_state_char(struct proc *p) {
    return proc_state_short(p->state)[0];
}

static void pp_build_stat(pbuf_t *pb, struct proc *p, int thread) {
    struct proc *l = pp_leader(p);
    int ppid = l->parent ? l->parent->tgid : 0;
    uint32_t ut, st;
    pp_times(p, !thread, &ut, &st);
    pp_mem_t m;
    pp_mem(p, &m);
    int tpgid = -1;
    if (p->ctty) {
        /* The terminal's foreground group: what the session leader set. */
        int pg = 0;
        if (p->ctty->ioctl_fn && p->ctty->ioctl_fn(p->ctty, 0x540F, &pg) == 0 && pg > 0)
            tpgid = pg;
    }
    uint32_t sigign = 0, sigcgt = 0;
    if (p->sighand)
        for (int s = 1; s < NSIGS; s++) {
            if (p->sighand->handlers[s] == SIG_IGN) sigign |= 1U << (s - 1);
            else if (p->sighand->handlers[s] != SIG_DFL) sigcgt |= 1U << (s - 1);
        }
    uint32_t pend = p->pending_sigs | (p->sigshared ? p->sigshared->pending : 0);
    int cpu = 0;
    for (uint32_t c = 0; c < MAX_CPUS; c++)
        if (cpus[c].proc == p) cpu = (int)c;
    struct proc *own = p->vm_owner ? p->vm_owner : l;
    /* Linux hides the address-space layout without ptrace read access. */
    int lay = procfs_may_read(p);
    uint32_t a_start = lay ? own->image_start : 0, a_end = lay ? own->image_end : 0;
    uint32_t a_brk = lay ? own->brk_base : 0, a_stack = lay ? (uint32_t)USER_STACK_TOP - 16 : 0;
    pb_printf(pb, "%d (%s) %c %d %d %d %u %d %u ",
              thread ? p->pid : l->pid, p->name, pp_state_char(p), ppid,
              l->pgrp, l->sid, pp_tty_nr(l), tpgid,
              p->pid == p->tgid ? 0x00400000U : 0x00400040U);   /* PF_RANDOMIZE-ish; PF_FORKNOEXEC */
    /* minflt cminflt majflt cmajflt utime stime cutime cstime */
    pb_printf(pb, "0 0 0 0 %u %u 0 0 ", ut, st);
    /* priority nice num_threads itrealvalue starttime */
    pb_printf(pb, "%d %d %d 0 %u ", 20 + p->nice, p->nice, pp_nthreads(p),
              l->start_tick);
    /* vsize rss rsslim startcode endcode startstack kstkesp kstkeip */
    pb_printf(pb, "%u %u 4294967295 %u %u %u 0 0 ",
              m.size * PAGE_SIZE, m.rss, a_start, a_end, a_stack);
    /* signal blocked sigignore sigcatch wchan nswap cnswap exit_signal
     * processor rt_priority policy delayacct guest_time cguest_time */
    pb_printf(pb, "%u %u %u %u %u 0 0 %d %d 0 0 0 0 0 ",
              pend >> 1, p->blocked_sigs >> 1, sigign, sigcgt,
              p->state == PROC_SLEEPING ? 1U : 0U,
              p->pid == p->tgid ? 17 : -1, cpu);
    /* start_data end_data start_brk arg_start arg_end env_start env_end
     * exit_code */
    pb_printf(pb, "%u %u %u 0 0 0 0 %d\n", a_start, a_end,
              a_brk, p->state == PROC_ZOMBIE ? p->exit_status : 0);
}

static void pp_build_status(pbuf_t *pb, struct proc *p, int thread) {
    struct proc *l = pp_leader(p);
    int ppid = l->parent ? l->parent->tgid : 0;
    pp_mem_t m;
    pp_mem(p, &m);
    uint32_t sigign = 0, sigcgt = 0;
    if (p->sighand)
        for (int s = 1; s < NSIGS; s++) {
            if (p->sighand->handlers[s] == SIG_IGN) sigign |= 1U << (s - 1);
            else if (p->sighand->handlers[s] != SIG_DFL) sigcgt |= 1U << (s - 1);
        }
    int fds = 0;
    if (p->ofile)
        for (int i = 0; i < MAX_FD; i++)
            if (p->ofile[i].type != FD_NONE) fds++;
    pb_printf(pb, "Name:\t%s\nUmask:\t%04o\nState:\t%s\nTgid:\t%d\nNgid:\t0\nPid:\t%d\n"
                  "PPid:\t%d\nTracerPid:\t0\n",
              p->name, p->umask, proc_state_long(p->state), p->tgid,
              thread ? p->pid : l->pid, ppid);
    pb_printf(pb, "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t%d\nGroups:\t",
              p->uid, p->euid, p->suid, p->euid, p->gid, p->egid, p->sgid, p->egid,
              fds <= 64 ? 64 : MAX_FD);
    for (uint32_t i = 0; i < p->ngroups && i < PROC_NGROUPS_MAX; i++)
        pb_printf(pb, "%u ", p->groups[i]);
    pb_printf(pb, "\nNStgid:\t%d\nNSpid:\t%d\nNSpgid:\t%d\nNSsid:\t%d\n",
              p->tgid, thread ? p->pid : l->pid, l->pgrp, l->sid);
    if (p->pgdir_phys) {
        uint32_t kb = PAGE_SIZE / 1024;
        pb_printf(pb, "VmPeak:\t%8u kB\nVmSize:\t%8u kB\nVmLck:\t%8u kB\nVmPin:\t%8u kB\n"
                      "VmHWM:\t%8u kB\nVmRSS:\t%8u kB\nRssAnon:\t%8u kB\n"
                      "RssFile:\t%8u kB\nRssShmem:\t%8u kB\nVmData:\t%8u kB\n"
                      "VmStk:\t%8u kB\nVmExe:\t%8u kB\nVmLib:\t%8u kB\n"
                      "VmPTE:\t%8u kB\nVmSwap:\t%8u kB\n",
                  m.size * kb, m.size * kb, 0U, 0U, m.rss * kb, m.rss * kb,
                  (m.rss - m.shared) * kb, 0U, m.shared * kb, m.data * kb,
                  m.stack * kb, m.text * kb, 0U, 4U, 0U);
    }
    uint32_t pend = p->pending_sigs;
    uint32_t shpend = p->sigshared ? p->sigshared->pending : 0;
    pb_printf(pb, "Threads:\t%d\nSigQ:\t0/1024\nSigPnd:\t%08x%08x\nShdPnd:\t%08x%08x\n"
                  "SigBlk:\t%08x%08x\nSigIgn:\t%08x%08x\nSigCgt:\t%08x%08x\n",
              pp_nthreads(p), 0U, pend >> 1, 0U, shpend >> 1, 0U, p->blocked_sigs >> 1,
              0U, sigign, 0U, sigcgt);
    pb_printf(pb, "CapInh:\t0000000000000000\nCapPrm:\t%s\nCapEff:\t%s\n"
                  "CapBnd:\t000001ffffffffff\nCapAmb:\t0000000000000000\n"
                  "NoNewPrivs:\t0\nSeccomp:\t0\nSpeculation_Store_Bypass:\tvulnerable\n"
                  "Cpus_allowed:\tff\nCpus_allowed_list:\t0-%u\n"
                  "voluntary_ctxt_switches:\t%u\nnonvoluntary_ctxt_switches:\t0\n",
              p->euid == 0 ? "000001ffffffffff" : "0000000000000000",
              p->euid == 0 ? "000001ffffffffff" : "0000000000000000",
              smp_cpu_count() ? smp_cpu_count() - 1 : 0, p->sched_count);
}

static void pp_build_statm(pbuf_t *pb, struct proc *p) {
    pp_mem_t m;
    pp_mem(p, &m);
    pb_printf(pb, "%u %u %u %u 0 %u 0\n", m.size, m.rss, m.shared, m.text,
              m.data + m.stack);
}

/* One Linux-format maps line: "start-end perms offset dev inode  path".
 * prot bits: 1=R 2=W 4=X (as mmap PROT_*), 8 = MAP_SHARED. */
static void pp_maps_line(pbuf_t *pb, uint32_t start, uint32_t end, uint32_t prot,
                         const char *path) {
    pb_printf(pb, "%08x-%08x %c%c%c%c 00000000 00:00 0 ", start, end,
              (prot & 1) ? 'r' : '-', (prot & 2) ? 'w' : '-',
              (prot & 4) ? 'x' : '-', (prot & 8) ? 's' : 'p');
    if (path && path[0]) pb_printf(pb, "                   %s", path);
    pb_printf(pb, "\n");
}

static void pp_build_maps(pbuf_t *pb, struct proc *p) {
    if (!p->pgdir_phys) return;
    struct proc *own = p->vm_owner ? p->vm_owner : pp_leader(p);
    /* Executable image (r-x), heap (rw-), every mmap()ed region in address
     * order from the VMA registry (the backing file's name, 's' for
     * MAP_SHARED), and the main-thread stack - the line glibc and
     * SpiderMonkey read for its bounds: the REAL eagerly mapped range. */
    if (own->image_end > own->image_start)
        pp_maps_line(pb, own->image_start, own->image_end, 1 | 4, own->exe);
    if (own->heap_end > own->brk_base)
        pp_maps_line(pb, own->brk_base, (own->heap_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1),
                     1 | 2, "[heap]");
    uint32_t vs, ve, vp;
    int vsh;
    const char *vname;
    for (int i = 0; proc_vma_iter_ex(own, i, &vs, &ve, &vp, &vsh, &vname) == 0; i++) {
        if (pb->pos > pb->cap - 160) break;      /* leave room for [stack] */
        pp_maps_line(pb, vs, ve, vp | (vsh ? 8 : 0), vname);
    }
    pp_maps_line(pb, (uint32_t)USER_STACK_BASE, (uint32_t)USER_STACK_TOP, 1 | 2, "[stack]");
}

static void pp_build_limits(pbuf_t *pb) {
    static const struct { const char *name; const char *soft; const char *hard; const char *unit; } L[] = {
        { "Max cpu time",          "unlimited", "unlimited", "seconds" },
        { "Max file size",         "unlimited", "unlimited", "bytes" },
        { "Max data size",         "unlimited", "unlimited", "bytes" },
        { "Max stack size",        "8388608",   "unlimited", "bytes" },
        { "Max core file size",    "0",         "unlimited", "bytes" },
        { "Max resident set",      "unlimited", "unlimited", "bytes" },
        { "Max processes",         "unlimited", "unlimited", "processes" },
        { "Max open files",        "512",       "512",       "files" },
        { "Max locked memory",     "unlimited", "unlimited", "bytes" },
        { "Max address space",     "unlimited", "unlimited", "bytes" },
        { "Max file locks",        "unlimited", "unlimited", "locks" },
        { "Max pending signals",   "unlimited", "unlimited", "signals" },
        { "Max msgqueue size",     "819200",    "819200",    "bytes" },
        { "Max nice priority",     "0",         "0",         "" },
        { "Max realtime priority", "0",         "0",         "" },
        { "Max realtime timeout",  "unlimited", "unlimited", "us" },
    };
    pb_printf(pb, "%-25s %-20s %-20s %-10s\n", "Limit", "Soft Limit", "Hard Limit", "Units");
    for (uint32_t i = 0; i < sizeof(L) / sizeof(L[0]); i++)
        pb_printf(pb, "%-25s %-20s %-20s %-10s\n", L[i].name, L[i].soft, L[i].hard, L[i].unit);
}

static void pp_build_fdinfo(pbuf_t *pb, struct proc *p, int fd) {
    if (!p->ofile || fd < 0 || fd >= MAX_FD) return;
    proc_file_t *f = &p->ofile[fd];
    if (f->type == FD_NONE) return;
    uint32_t flags = (uint32_t)f->flags | (f->cloexec ? 02000000U : 0);
    pb_printf(pb, "pos:\t%u\nflags:\t0%o\nmnt_id:\t%u\nino:\t%u\n",
              f->type == FD_FILE ? f->offset : 0U, flags,
              f->mnt ? 20U + f->mnt_seq : 1U,
              f->type == FD_FILE && f->node ? f->node->inode : 0U);
    if (f->type == FD_FILE && f->node)
        inotify_show_fdinfo(f->node, pb->b, &pb->pos, pb->cap);
}

/* readlink targets of exe/cwd/root, as paths in the caller's view. */
static void pp_build_link(pbuf_t *pb, struct proc *p, int kind) {
    struct proc *own = p->vm_owner ? p->vm_owner : pp_leader(p);
    if (kind == PK_EXE) {
        if (!p->pgdir_phys) return;               /* kernel thread: ENOENT */
        pb_printf(pb, "%s", own->exe[0] ? own->exe : own->name);
    } else if (kind == PK_CWD) {
        pb_printf(pb, "%s", p->cwd[0] ? p->cwd : "/");
    } else {
        pb_printf(pb, "/");
    }
}

static uint32_t pp_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    pnode_t *pn = (pnode_t *)n;
    struct proc *t = pp_task(pn->pid);
    if (!t) return (uint32_t)-3;                       /* -ESRCH */
    struct proc *p = pn->thread ? t : pp_leader(t);
    int priv = pn->kind == PK_ENVIRON || pn->kind == PK_AUXV || pn->kind == PK_MAPS ||
               pn->kind == PK_IO || pn->kind == PK_EXE || pn->kind == PK_CWD ||
               pn->kind == PK_ROOT || pn->kind == PK_FDINFO || pn->kind == PK_FD;
    if (priv && !procfs_may_read(p)) return (uint32_t)-13;  /* -EACCES */

    uint32_t cap = pn->kind == PK_MAPS ? 32768 :
                   pn->kind == PK_CMDLINE || pn->kind == PK_ENVIRON ? 4096 + sizeof(p->environ) :
                   pn->kind == PK_MOUNTINFO || pn->kind == PK_MOUNTS ? 8192 : 4096;
    pbuf_t pb = { (char *)kmalloc(cap), 0, cap };
    if (!pb.b) return (uint32_t)-12;
    switch (pn->kind) {
    case PK_STAT:    pp_build_stat(&pb, p, pn->thread); break;
    case PK_STATUS:  pp_build_status(&pb, p, pn->thread); break;
    case PK_STATM:   pp_build_statm(&pb, p); break;
    case PK_CMDLINE: pb_bytes(&pb, p->cmdline, p->cmdline_len); break;
    case PK_ENVIRON: pb_bytes(&pb, p->environ, p->environ_len); break;
    case PK_AUXV:    pb_bytes(&pb, p->auxv_data, p->auxv_bytes); break;
    case PK_COMM:    pb_printf(&pb, "%s\n", p->name); break;
    case PK_MAPS:    pp_build_maps(&pb, p); break;
    case PK_LIMITS:  pp_build_limits(&pb); break;
    case PK_IO: {
        uint64_t rc = 0, wc = 0;
        uint32_t sr = 0, sw = 0;
        for (int i = 0; i < ptable_hwm && i < MAX_PROCS; i++) {
            struct proc *q = &ptable[i];
            if (q->state == PROC_UNUSED) continue;
            if (pn->thread ? q != p : q->tgid != p->tgid) continue;
            rc += q->io_rchar; wc += q->io_wchar;
            sr += q->io_syscr; sw += q->io_syscw;
        }
        pb_printf(&pb, "rchar: %llu\nwchar: %llu\nsyscr: %u\nsyscw: %u\n"
                       "read_bytes: 0\nwrite_bytes: 0\ncancelled_write_bytes: 0\n",
                  rc, wc, sr, sw);
        break;
    }
    case PK_MOUNTINFO:
        pb.pos = vfs_mountinfo_format(pb.b, pb.cap);
        break;
    case PK_MOUNTS:
        pb.pos = vfs_mounts_format(pb.b, pb.cap);
        break;
    case PK_CGROUP:    pb_printf(&pb, "0::/\n"); break;
    case PK_OOM_SCORE: pb_printf(&pb, "0\n"); break;
    case PK_OOM_ADJ:   pb_printf(&pb, "0\n"); break;
    case PK_WCHAN:     pb_printf(&pb, "%s", p->state == PROC_SLEEPING ? "do_wait" : "0"); break;
    case PK_LOGINUID:  pb_printf(&pb, "%u", p->uid); break;
    case PK_SCHED:
        pb_printf(&pb, "%s (%d, #threads: %d)\n-------------------------------------------------------------------\n"
                       "se.sum_exec_runtime                          :%llu\nnr_switches                                  :%u\n",
                  p->name, p->pid, pp_nthreads(p), p->run_ns_total / 1000000ULL, p->sched_count);
        break;
    case PK_EXE: case PK_CWD: case PK_ROOT:
        pp_build_link(&pb, p, pn->kind);
        break;
    case PK_FDINFO:  pp_build_fdinfo(&pb, p, pn->arg); break;
    default: break;
    }
    uint32_t r = procfs_copy_blob((const uint8_t *)pb.b, pb.pos, off, len, buf);
    kfree(pb.b);
    return r;
}

/* comm: a process may rename its own threads (Linux: same thread group);
 * oom_score_adj, loginuid and sched take any write and keep nothing. */
static uint32_t pp_write(vfs_node_t *n, uint32_t off, uint32_t len, const uint8_t *buf) {
    pnode_t *pn = (pnode_t *)n;
    (void)off;
    struct proc *t = pp_task(pn->pid);
    if (!t) return (uint32_t)-3;
    struct proc *p = pn->thread ? t : pp_leader(t);
    if (pn->kind != PK_COMM) return len;
    if (!current_proc || current_proc->tgid != p->tgid) return (uint32_t)-22;
    char name[16];
    uint32_t k = len < 15 ? len : 15;
    memcpy(name, buf, k);
    name[k] = '\0';
    for (uint32_t i = 0; i < k; i++)
        if (name[i] == '\n') { name[i] = '\0'; break; }
    memcpy(p->name, name, 16);
    return len;
}

/* /proc/self → "<tgid>", /proc/thread-self → "<tgid>/task/<tid>". */
static vfs_node_t proc_selflink_node;
static vfs_node_t proc_threadself_node;

static uint32_t procfs_selflink_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                     uint8_t *buf) {
    char t[48];
    if (!current_proc) return 0;
    int l = n == &proc_threadself_node
          ? snprintf(t, sizeof(t), "%d/task/%d", current_proc->tgid, current_proc->pid)
          : snprintf(t, sizeof(t), "%d", current_proc->tgid);
    return procfs_copy_blob((const uint8_t *)t, (uint32_t)l, off, len, buf);
}
