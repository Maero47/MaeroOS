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
 * process.  Once its process is gone a node is freed when it is no longer
 * open and has been dead for a few seconds (a lookup in progress on another
 * CPU still holding it has long finished by then).
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
    int            refs;       /* open references (retain/close) */
    uint32_t       dead_tick;  /* PIT tick its process was first seen gone */
    struct pnode  *next;
} pnode_t;

static pnode_t *pnodes;

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

/* ── node cache ── */

static void pnode_retain(vfs_node_t *n) { ((pnode_t *)n)->refs++; }
static void pnode_close(vfs_node_t *n) {
    pnode_t *pn = (pnode_t *)n;
    if (pn->refs > 0) pn->refs--;
}

/* Free nodes whose process has been gone a while and that nobody holds. */
static void pnode_sweep(void) {
    uint32_t now = pit_ticks();
    pnode_t **pp = &pnodes;
    while (*pp) {
        pnode_t *pn = *pp;
        if (pp_task(pn->pid)) {
            pn->dead_tick = 0;
            pp = &pn->next;
            continue;
        }
        if (!pn->dead_tick) pn->dead_tick = now ? now : 1;
        if (pn->refs <= 0 && now - pn->dead_tick > 300) {
            *pp = pn->next;
            kfree(pn);
            continue;
        }
        pp = &pn->next;
    }
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
    if (now - sweep_tick > 100) { sweep_tick = now; pnode_sweep(); }

    struct proc *t = pp_task(pid);
    if (!t) return NULL;
    pnode_t *pn;
    for (pn = pnodes; pn; pn = pn->next)
        if (pn->pid == pid && pn->kind == kind && pn->arg == arg &&
            pn->thread == thread)
            break;
    if (!pn) {
        pn = (pnode_t *)kmalloc(sizeof(*pn));
        if (!pn) return NULL;
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
        pn->next = pnodes;
        pnodes = pn;
    }
    /* Ownership follows the task's credentials as they are now. */
    uint32_t uid, gid;
    pp_owner(thread ? t : pp_leader(t), &uid, &gid);
    pn->v.uid = uid;
    pn->v.gid = gid;
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
              m.size * PAGE_SIZE, m.rss, own->image_start,
              own->image_end, (uint32_t)USER_STACK_TOP - 16);
    /* signal blocked sigignore sigcatch wchan nswap cnswap exit_signal
     * processor rt_priority policy delayacct guest_time cguest_time */
    pb_printf(pb, "%u %u %u %u %u 0 0 %d %d 0 0 0 0 0 ",
              pend >> 1, p->blocked_sigs >> 1, sigign, sigcgt,
              p->state == PROC_SLEEPING ? 1U : 0U,
              p->pid == p->tgid ? 17 : -1, cpu);
    /* start_data end_data start_brk arg_start arg_end env_start env_end
     * exit_code */
    pb_printf(pb, "%u %u %u 0 0 0 0 %d\n", own->image_start, own->image_end,
              own->brk_base, p->state == PROC_ZOMBIE ? p->exit_status : 0);
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
