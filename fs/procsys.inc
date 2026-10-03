/* ── System-wide /proc files: stat, uptime, loadavg, meminfo, vmstat ─────────
 * Included by fs/procfs.c.  The CPU figures come from the scheduler's
 * dispatch timestamps (cpus[].busy_ns / idle_ns), split into user and system
 * time by the timer-tick samples, in USER_HZ (100) clock ticks. */

extern uint32_t sched_ctxt;
extern uint32_t sched_loadavg[3];

static uint32_t procfs_build_text(vfs_node_t *n, void (*build)(pbuf_t *),
                                  uint32_t cap, uint32_t off, uint32_t len,
                                  uint8_t *buf) {
    (void)n;
    pbuf_t pb = { (char *)kmalloc(cap), 0, cap };
    if (!pb.b) return (uint32_t)-12;
    build(&pb);
    uint32_t r = procfs_copy_blob((const uint8_t *)pb.b, pb.pos, off, len, buf);
    kfree(pb.b);
    return r;
}

/* One CPU's user, system and idle time in ticks. */
static void cpu_ticks(uint32_t c, uint64_t *user, uint64_t *sys, uint64_t *idle) {
    struct cpu *cp = &cpus[c];
    uint64_t busy = cp->busy_ns / 10000000ULL;
    uint32_t tu = cp->tick_user, ts = cp->tick_sys;
    uint64_t s = (tu + ts) ? busy * ts / (tu + ts) : 0;
    *user = busy - s;
    *sys = s;
    *idle = cp->idle_ns / 10000000ULL;
}

static void build_stat(pbuf_t *pb) {
    uint32_t ncpu = smp_cpu_count();
    if (!ncpu) ncpu = 1;
    uint64_t tu = 0, ts = 0, ti = 0;
    for (uint32_t c = 0; c < ncpu && c < MAX_CPUS; c++) {
        uint64_t u, s, i;
        cpu_ticks(c, &u, &s, &i);
        tu += u; ts += s; ti += i;
    }
    pb_printf(pb, "cpu  %llu 0 %llu %llu 0 0 0 0 0 0\n", tu, ts, ti);
    for (uint32_t c = 0; c < ncpu && c < MAX_CPUS; c++) {
        uint64_t u, s, i;
        cpu_ticks(c, &u, &s, &i);
        pb_printf(pb, "cpu%u %llu 0 %llu %llu 0 0 0 0 0 0\n", c, u, s, i);
    }
    uint32_t running = 0, blocked = 0;
    for (int i = 0; i < ptable_hwm && i < MAX_PROCS; i++) {
        if (ptable[i].state == PROC_RUNNABLE || ptable[i].state == PROC_RUNNING) running++;
    }
    pb_printf(pb, "intr %u\nctxt %u\nbtime %u\nprocesses %u\nprocs_running %u\n"
                  "procs_blocked %u\nsoftirq 0 0 0 0 0 0 0 0 0 0 0\n",
              pit_ticks(), sched_ctxt, rtc_boot_epoch(), proc_forks_total,
              running ? running : 1, blocked);
}

static uint32_t procfs_stat_sys_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    return procfs_build_text(n, build_stat, 2048, off, len, buf);
}

/* "<uptime> <idle>": idle is summed over all CPUs, as Linux reports it. */
static void build_uptime(pbuf_t *pb) {
    uint32_t t = pit_ticks();
    uint64_t idle = 0;
    uint32_t ncpu = smp_cpu_count();
    if (!ncpu) ncpu = 1;
    for (uint32_t c = 0; c < ncpu && c < MAX_CPUS; c++)
        idle += cpus[c].idle_ns / 10000000ULL;
    pb_printf(pb, "%u.%02u %llu.%02u\n", t / 100, t % 100, idle / 100,
              (uint32_t)(idle % 100));
}

static uint32_t procfs_uptime_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    return procfs_build_text(n, build_uptime, 128, off, len, buf);
}

/* "1.00 0.50 0.25 running/total last_pid" */
static void build_loadavg(pbuf_t *pb) {
    uint32_t running = 0, total = 0;
    int last = 0;
    for (int i = 0; i < ptable_hwm && i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_UNUSED) continue;
        total++;
        if (p->state == PROC_RUNNABLE || p->state == PROC_RUNNING) running++;
        if (p->pid > last) last = p->pid;
    }
    for (int k = 0; k < 3; k++) {
        uint32_t v = sched_loadavg[k] + (1U << 11) / 200;      /* round to 0.01 */
        pb_printf(pb, "%u.%02u ", v >> 11, ((v & ((1U << 11) - 1)) * 100) >> 11);
    }
    pb_printf(pb, "%u/%u %d\n", running ? running : 1, total, last);
}

static uint32_t procfs_loadavg_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    return procfs_build_text(n, build_loadavg, 128, off, len, buf);
}

static void build_meminfo(pbuf_t *pb) {
    uint32_t total = pmm_ram_frames() * 4;
    uint32_t free = pmm_free_frames() * 4;
    uint32_t shm = sysvipc_shm_pages() * 4;
    uint32_t used = total > free ? total - free : 0;
    static const char *const zero[] = {
        "Buffers", "SwapCached", "Unevictable", "Mlocked", "SwapTotal", "SwapFree",
        "Zswap", "Zswapped", "Dirty", "Writeback", "Mapped", "KReclaimable",
        "SReclaimable", "NFS_Unstable", "Bounce", "WritebackTmp", "AnonHugePages",
        "ShmemHugePages", "ShmemPmdMapped", "HugePages_Total", "HugePages_Free",
        "HugePages_Rsvd", "HugePages_Surp",
    };
    pb_printf(pb, "MemTotal:       %8u kB\nMemFree:        %8u kB\nMemAvailable:   %8u kB\n",
              total, free, free);
    pb_printf(pb, "Cached:         %8u kB\nActive:         %8u kB\nInactive:       %8u kB\n",
              shm, used, 0U);
    pb_printf(pb, "Active(anon):   %8u kB\nInactive(anon): %8u kB\nActive(file):   %8u kB\n"
                  "Inactive(file): %8u kB\n", used, 0U, 0U, 0U);
    pb_printf(pb, "HighTotal:      %8u kB\nHighFree:       %8u kB\nLowTotal:       %8u kB\n"
                  "LowFree:        %8u kB\n",
              pmm_high_frames() * 4, pmm_high_free_frames() * 4,
              total - pmm_high_frames() * 4, free - pmm_high_free_frames() * 4);
    pb_printf(pb, "AnonPages:      %8u kB\nShmem:          %8u kB\nSlab:           %8u kB\n"
                  "SUnreclaim:     %8u kB\nKernelStack:    %8u kB\nPageTables:     %8u kB\n"
                  "CommitLimit:    %8u kB\nCommitted_AS:   %8u kB\nVmallocTotal:   %8u kB\n"
                  "VmallocUsed:    %8u kB\nVmallocChunk:   %8u kB\nHugepagesize:   %8u kB\n",
              used, shm, 0U, 0U, 0U, 0U, total, used, 0U, 0U, 0U, 2048U);
    for (uint32_t i = 0; i < sizeof(zero) / sizeof(zero[0]); i++) {
        char key[32];
        snprintf(key, sizeof(key), "%s:", zero[i]);
        if (!strncmp(zero[i], "HugePages_", 10))
            pb_printf(pb, "%-16s%8u\n", key, 0U);
        else
            pb_printf(pb, "%-16s%8u kB\n", key, 0U);
    }
}

static uint32_t procfs_meminfo_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    return procfs_build_text(n, build_meminfo, 4096, off, len, buf);
}

/* /proc/vmstat: the counters vmstat(8) and free read; no paging to disk. */
static void build_vmstat(pbuf_t *pb) {
    pb_printf(pb, "nr_free_pages %u\nnr_inactive_anon 0\nnr_active_anon %u\n"
                  "nr_inactive_file 0\nnr_active_file 0\nnr_unevictable 0\n"
                  "nr_mlock 0\nnr_anon_pages %u\nnr_mapped 0\nnr_file_pages 0\n"
                  "nr_dirty 0\nnr_writeback 0\nnr_shmem %u\nnr_page_table_pages 0\n"
                  "nr_kernel_stack 0\npgpgin 0\npgpgout 0\npswpin 0\npswpout 0\n"
                  "pgfault 0\npgmajfault 0\nctxt %u\n",
              pmm_free_frames(), pmm_ram_frames() - pmm_free_frames(),
              pmm_ram_frames() - pmm_free_frames(), sysvipc_shm_pages(), sched_ctxt);
}

static uint32_t procfs_vmstat_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    return procfs_build_text(n, build_vmstat, 2048, off, len, buf);
}

/* /proc/sysvipc/{shm,sem,msg}: the tables ipcs reads. */
static uint32_t procfs_sysvipc_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    pbuf_t pb = { (char *)kmalloc(16384), 0, 16384 };
    if (!pb.b) return (uint32_t)-12;
    pb.pos = sysvipc_proc_format(n->name, pb.b, pb.cap);
    uint32_t r = procfs_copy_blob((const uint8_t *)pb.b, pb.pos, off, len, buf);
    kfree(pb.b);
    return r;
}

/* ── /proc/sys: a small table of read-only text files (and a few that take
 * writes and ignore them). ── */
typedef struct {
    const char *dir;          /* "vm", "kernel" */
    const char *name;
    int         which;        /* sysctl id below */
    uint16_t    mode;
} sysctl_ent_t;

enum { SC_OVERCOMMIT, SC_SHMMAX, SC_SHMALL, SC_SHMMNI, SC_SEM, SC_MSGMAX, SC_MSGMNB,
       SC_MSGMNI, SC_PIDMAX, SC_OSTYPE, SC_OSRELEASE, SC_HOSTNAME, SC_NGROUPS,
       SC_MAX_USER_WATCHES, SC_MAX_USER_INSTANCES, SC_MAX_QUEUED_EVENTS,
       SC_FILE_MAX, SC_THREADS_MAX };

static const sysctl_ent_t sysctl_ents[] = {
    { "vm",     "overcommit_memory",    SC_OVERCOMMIT, 0644 },
    { "kernel", "shmmax",               SC_SHMMAX,     0444 },
    { "kernel", "shmall",               SC_SHMALL,     0444 },
    { "kernel", "shmmni",               SC_SHMMNI,     0444 },
    { "kernel", "sem",                  SC_SEM,        0444 },
    { "kernel", "msgmax",               SC_MSGMAX,     0444 },
    { "kernel", "msgmnb",               SC_MSGMNB,     0444 },
    { "kernel", "msgmni",               SC_MSGMNI,     0444 },
    { "kernel", "pid_max",              SC_PIDMAX,     0444 },
    { "kernel", "threads-max",          SC_THREADS_MAX, 0444 },
    { "kernel", "ostype",               SC_OSTYPE,     0444 },
    { "kernel", "osrelease",            SC_OSRELEASE,  0444 },
    { "kernel", "hostname",             SC_HOSTNAME,   0444 },
    { "kernel", "ngroups_max",          SC_NGROUPS,    0444 },
    { "fs",     "file-max",             SC_FILE_MAX,   0444 },
    { "fs/inotify", "max_user_watches",   SC_MAX_USER_WATCHES,   0444 },
    { "fs/inotify", "max_user_instances", SC_MAX_USER_INSTANCES, 0444 },
    { "fs/inotify", "max_queued_events",  SC_MAX_QUEUED_EVENTS,  0444 },
};
#define N_SYSCTL (sizeof(sysctl_ents) / sizeof(sysctl_ents[0]))
static const char *const sysctl_dirs[] = { "vm", "kernel", "fs", "fs/inotify" };
#define N_SYSCTL_DIRS 4
static vfs_node_t sysctl_nodes[N_SYSCTL];
static vfs_node_t sysctl_dir_nodes[N_SYSCTL_DIRS];

static uint32_t sysctl_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    const sysctl_ent_t *e = &sysctl_ents[(int)(uintptr_t)n->private];
    char t[96];
    int l = 0;
    switch (e->which) {
    case SC_OVERCOMMIT: l = snprintf(t, sizeof(t), "0\n"); break;
    case SC_SHMMAX:  l = snprintf(t, sizeof(t), "%u\n", SYSV_SHMMAX); break;
    case SC_SHMALL:  l = snprintf(t, sizeof(t), "%u\n", SYSV_SHMALL); break;
    case SC_SHMMNI:  l = snprintf(t, sizeof(t), "%u\n", SYSV_SHMMNI); break;
    case SC_SEM:     l = snprintf(t, sizeof(t), "%u\t%u\t%u\t%u\n", SYSV_SEMMSL,
                                  SYSV_SEMMNS, SYSV_SEMOPM, SYSV_SEMMNI); break;
    case SC_MSGMAX:  l = snprintf(t, sizeof(t), "%u\n", SYSV_MSGMAX); break;
    case SC_MSGMNB:  l = snprintf(t, sizeof(t), "%u\n", SYSV_MSGMNB); break;
    case SC_MSGMNI:  l = snprintf(t, sizeof(t), "%u\n", SYSV_MSGMNI); break;
    case SC_PIDMAX:  l = snprintf(t, sizeof(t), "4194304\n"); break;
    case SC_THREADS_MAX: l = snprintf(t, sizeof(t), "%u\n", (uint32_t)MAX_PROCS); break;
    case SC_OSTYPE:  l = snprintf(t, sizeof(t), "Linux\n"); break;
    case SC_OSRELEASE: l = snprintf(t, sizeof(t), "5.15.0-maeros\n"); break;
    case SC_HOSTNAME: l = snprintf(t, sizeof(t), "maeros\n"); break;
    case SC_NGROUPS: l = snprintf(t, sizeof(t), "%u\n", (uint32_t)PROC_NGROUPS_MAX); break;
    case SC_FILE_MAX: l = snprintf(t, sizeof(t), "65536\n"); break;
    case SC_MAX_USER_WATCHES: l = snprintf(t, sizeof(t), "%u\n", INOTIFY_MAX_WATCHES); break;
    case SC_MAX_USER_INSTANCES: l = snprintf(t, sizeof(t), "%u\n", INOTIFY_MAX_INSTANCES); break;
    case SC_MAX_QUEUED_EVENTS: l = snprintf(t, sizeof(t), "%u\n", INOTIFY_MAX_QUEUED); break;
    }
    return procfs_copy_blob((const uint8_t *)t, (uint32_t)l, off, len, buf);
}

static uint32_t sysctl_write(vfs_node_t *n, uint32_t off, uint32_t len, const uint8_t *buf) {
    (void)n; (void)off; (void)buf;
    return len;   /* overcommit_memory: accepted and ignored */
}

static int sysctl_dir_readdir(vfs_node_t *node, uint32_t idx, vfs_dirent_t *out) {
    const char *dir = (const char *)node->private;    /* "" for /proc/sys */
    uint32_t dlen = (uint32_t)strlen(dir), k = 0;
    /* Subdirectories directly below this one. */
    for (uint32_t i = 0; i < N_SYSCTL_DIRS; i++) {
        const char *d = sysctl_dirs[i];
        const char *rest;
        if (dlen) {
            if (strncmp(d, dir, dlen) || d[dlen] != '/') continue;
            rest = d + dlen + 1;
        } else {
            rest = d;
        }
        if (strchr(rest, '/')) continue;
        if (k++ != idx) continue;
        out->ino = 0x30000000U + i;
        out->type = VFS_FLAG_DIR;
        strncpy(out->name, rest, 255);
        out->name[255] = '\0';
        return 0;
    }
    for (uint32_t i = 0; dlen && i < N_SYSCTL; i++) {
        if (strcmp(sysctl_ents[i].dir, dir)) continue;
        if (k++ != idx) continue;
        out->ino = 0x30001000U + i;
        out->type = VFS_FLAG_FILE;
        strncpy(out->name, sysctl_ents[i].name, 255);
        out->name[255] = '\0';
        return 0;
    }
    return -1;
}

static vfs_node_t *sysctl_dir_finddir(vfs_node_t *node, const char *name) {
    const char *dir = (const char *)node->private;
    uint32_t dlen = (uint32_t)strlen(dir);
    for (uint32_t i = 0; i < N_SYSCTL_DIRS; i++) {
        const char *d = sysctl_dirs[i];
        const char *rest;
        if (dlen) {
            if (strncmp(d, dir, dlen) || d[dlen] != '/') continue;
            rest = d + dlen + 1;
        } else {
            rest = d;
        }
        if (!strcmp(rest, name)) return &sysctl_dir_nodes[i];
    }
    for (uint32_t i = 0; dlen && i < N_SYSCTL; i++)
        if (!strcmp(sysctl_ents[i].dir, dir) && !strcmp(sysctl_ents[i].name, name))
            return &sysctl_nodes[i];
    return NULL;
}

static vfs_node_t proc_sys_node;

static void sysctl_init(void) {
    memset(&proc_sys_node, 0, sizeof(proc_sys_node));
    strncpy(proc_sys_node.name, "sys", 255);
    proc_sys_node.flags = VFS_FLAG_DIR;
    proc_sys_node.inode = 79;
    proc_sys_node.mask = 0555;
    proc_sys_node.readdir_fn = sysctl_dir_readdir;
    proc_sys_node.finddir_fn = sysctl_dir_finddir;
    proc_sys_node.private = (void *)"";
    for (uint32_t i = 0; i < N_SYSCTL_DIRS; i++) {
        vfs_node_t *d = &sysctl_dir_nodes[i];
        memset(d, 0, sizeof(*d));
        const char *slash = strchr(sysctl_dirs[i], '/');
        strncpy(d->name, slash ? slash + 1 : sysctl_dirs[i], 255);
        d->flags = VFS_FLAG_DIR;
        d->inode = 0x30000000U + i;
        d->mask = 0555;
        d->readdir_fn = sysctl_dir_readdir;
        d->finddir_fn = sysctl_dir_finddir;
        d->private = (void *)sysctl_dirs[i];
    }
    for (uint32_t i = 0; i < N_SYSCTL; i++) {
        vfs_node_t *f = &sysctl_nodes[i];
        memset(f, 0, sizeof(*f));
        strncpy(f->name, sysctl_ents[i].name, 255);
        f->flags = VFS_FLAG_FILE;
        f->inode = 0x30001000U + i;
        f->mask = sysctl_ents[i].mode;
        f->read_fn = sysctl_read;
        if (sysctl_ents[i].mode & 0200) f->write_fn = sysctl_write;
        f->private = (void *)(uintptr_t)i;
    }
}

/* /proc/sysvipc */
static vfs_node_t proc_sysvipc_node;
static vfs_node_t proc_sysvipc_files[3];
static const char *const sysvipc_names[3] = { "shm", "sem", "msg" };

static int sysvipc_dir_readdir(vfs_node_t *node, uint32_t idx, vfs_dirent_t *out) {
    (void)node;
    if (idx >= 3) return -1;
    out->ino = 0x30002000U + idx;
    out->type = VFS_FLAG_FILE;
    strncpy(out->name, sysvipc_names[idx], 255);
    out->name[255] = '\0';
    return 0;
}

static vfs_node_t *sysvipc_dir_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    for (int i = 0; i < 3; i++)
        if (!strcmp(name, sysvipc_names[i])) return &proc_sysvipc_files[i];
    return NULL;
}

static void sysvipc_dir_init(void) {
    memset(&proc_sysvipc_node, 0, sizeof(proc_sysvipc_node));
    strncpy(proc_sysvipc_node.name, "sysvipc", 255);
    proc_sysvipc_node.flags = VFS_FLAG_DIR;
    proc_sysvipc_node.inode = 0x30002010U;
    proc_sysvipc_node.mask = 0555;
    proc_sysvipc_node.readdir_fn = sysvipc_dir_readdir;
    proc_sysvipc_node.finddir_fn = sysvipc_dir_finddir;
    for (int i = 0; i < 3; i++) {
        vfs_node_t *f = &proc_sysvipc_files[i];
        memset(f, 0, sizeof(*f));
        strncpy(f->name, sysvipc_names[i], 255);
        f->flags = VFS_FLAG_FILE;
        f->inode = 0x30002000U + (uint32_t)i;
        f->mask = 0444;
        f->read_fn = procfs_sysvipc_read;
    }
}
