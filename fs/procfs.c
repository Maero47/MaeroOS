#include "procfs.h"
#include "vfs.h"
#include "../proc/process.h"
#include "../drivers/pci.h"
#include "../drivers/rtl8139.h"
#include "../net/net.h"
#include "../net/lwip_glue.h"
#include "../net/firewall.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../kernel/klog.h"
#include "../arch/i686/cpu/pit.h"
#include <kernel/config.h>
#include <stdint.h>
#include <stddef.h>

/* ── String-building helpers ──────────────────────────────────────────────── */

/* Append a NUL-terminated string into buf[*pos..cap-2]; returns 1 on success */
static int pappend(char *buf, uint32_t *pos, uint32_t cap, const char *str) {
    if (cap == 0 || *pos >= cap - 1) return 0;
    uint32_t len = (uint32_t)__builtin_strlen(str);
    if (*pos + len >= cap) len = cap - 1 - *pos;
    if (len == 0) return 0;
    __builtin_memcpy(buf + *pos, str, len);
    *pos += len;
    return 1;
}

/* Append a decimal integer (signed) */
static void pappend_int(char *buf, uint32_t *pos, uint32_t cap, int val) {
    char tmp[20];
    int neg = (val < 0);
    uint32_t uval = neg ? (uint32_t)(-(val + 1)) + 1u : (uint32_t)val;
    int i = 0;
    if (uval == 0) {
        tmp[i++] = '0';
    } else {
        while (uval) {
            tmp[i++] = '0' + (int)(uval % 10u);
            uval /= 10u;
        }
    }
    if (neg) tmp[i++] = '-';
    /* reverse */
    for (int j = 0; j < i / 2; j++) {
        char t = tmp[j]; tmp[j] = tmp[i - 1 - j]; tmp[i - 1 - j] = t;
    }
    tmp[i] = '\0';
    pappend(buf, pos, cap, tmp);
}

static void pappend_hex(char *buf, uint32_t *pos, uint32_t cap,
                        uint32_t val, int width) {
    static const char hex[] = "0123456789abcdef";
    for (int i = width - 1; i >= 0; i--) {
        char c[2];
        c[0] = hex[(val >> (i * 4)) & 0xFU];
        c[1] = '\0';
        pappend(buf, pos, cap, c);
    }
}

static const char *proc_state_short(proc_state_t state) {
    switch (state) {
    case PROC_UNUSED:   return "X";
    case PROC_EMBRYO:   return "I";
    case PROC_RUNNABLE: return "R";
    case PROC_RUNNING:  return "R";
    case PROC_SLEEPING: return "S";
    case PROC_STOPPED:  return "T";
    case PROC_ZOMBIE:   return "Z";
    default:            return "?";
    }
}

static const char *proc_state_long(proc_state_t state) {
    switch (state) {
    case PROC_EMBRYO:   return "I (idle)";
    case PROC_RUNNABLE: return "R (runnable)";
    case PROC_RUNNING:  return "R (running)";
    case PROC_SLEEPING: return "S (sleeping)";
    case PROC_STOPPED:  return "T (stopped)";
    case PROC_ZOMBIE:   return "Z (zombie)";
    default:            return "X (dead)";
    }
}

static void pappend_tty(char *buf, uint32_t *pos, uint32_t cap,
                        struct proc *p) {
    if (!p || !p->ctty) {
        pappend(buf, pos, cap, "-");
        return;
    }
    if (p->ctty->name[0] >= '0' && p->ctty->name[0] <= '9')
        pappend(buf, pos, cap, "pts/");
    pappend(buf, pos, cap, p->ctty->name);
}

static struct proc *procfs_find_pid(int pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (ptable[i].state != PROC_UNUSED && ptable[i].pid == pid)
            return &ptable[i];
    }
    return NULL;
}

static int procfs_parse_pid(const char *name) {
    int pid = 0;
    if (!name || *name < '0' || *name > '9') return -1;
    while (*name) {
        if (*name < '0' || *name > '9') return -1;
        pid = pid * 10 + (*name - '0');
        name++;
    }
    return pid;
}

static uint32_t procfs_build_status(struct proc *p, uint32_t off,
                                    uint32_t len, uint8_t *buf) {
    char content[512];
    uint32_t pos = 0;

    const char *pname = p ? p->name : "unknown";
    int pid  = p ? p->pid : 0;
    int ppid = (p && p->parent) ? p->parent->pid : 0;
    int pgrp = p ? p->pgrp : 0;
    int sid  = p ? p->sid : 0;

    pappend(content, &pos, sizeof(content), "Name:\t");
    pappend(content, &pos, sizeof(content), pname);
    pappend(content, &pos, sizeof(content), "\nPid:\t");
    pappend_int(content, &pos, sizeof(content), pid);
    pappend(content, &pos, sizeof(content), "\nPPid:\t");
    pappend_int(content, &pos, sizeof(content), ppid);
    pappend(content, &pos, sizeof(content), "\nState:\t");
    pappend(content, &pos, sizeof(content),
            p ? proc_state_long(p->state) : "X (dead)");
    pappend(content, &pos, sizeof(content), "\nPgid:\t");
    pappend_int(content, &pos, sizeof(content), pgrp);
    pappend(content, &pos, sizeof(content), "\nSid:\t");
    pappend_int(content, &pos, sizeof(content), sid);
    pappend(content, &pos, sizeof(content), "\nTty:\t");
    pappend_tty(content, &pos, sizeof(content), p);
    pappend(content, &pos, sizeof(content), "\nVmSize:\t4096 kB\n");
    content[pos] = '\0';

    if (off >= pos) return 0;
    uint32_t avail = pos - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

static uint32_t procfs_build_stat(struct proc *p, uint32_t off,
                                  uint32_t len, uint8_t *buf) {
    char content[256];
    uint32_t pos = 0;

    int pid  = p ? p->pid : 0;
    int ppid = (p && p->parent) ? p->parent->pid : 0;
    int pgrp = p ? p->pgrp : 0;
    int sid  = p ? p->sid : 0;
    const char *pname = p ? p->name : "unknown";

    pappend_int(content, &pos, sizeof(content), pid);
    pappend(content, &pos, sizeof(content), " (");
    pappend(content, &pos, sizeof(content), pname);
    pappend(content, &pos, sizeof(content), ") ");
    pappend(content, &pos, sizeof(content), p ? proc_state_short(p->state) : "X");
    pappend(content, &pos, sizeof(content), " ");
    pappend_int(content, &pos, sizeof(content), ppid);
    pappend(content, &pos, sizeof(content), " ");
    pappend_int(content, &pos, sizeof(content), pgrp);
    pappend(content, &pos, sizeof(content), " ");
    pappend_int(content, &pos, sizeof(content), sid);
    /* Real-ish values: thread count and virtual size. */
    int nthreads = 0;
    uint32_t vsize = 0, rss = 0;
    if (p) {
        for (int i = 0; i < MAX_PROCS; i++)
            if (ptable[i].state != PROC_UNUSED && ptable[i].tgid == p->tgid)
                nthreads++;
        if (nthreads < 1) nthreads = 1;
        uint32_t img  = (p->image_end > p->image_start) ? p->image_end - p->image_start : 0;
        uint32_t heap = (p->heap_end > p->brk_base) ? p->heap_end - p->brk_base : 0;
        uint32_t stk  = (uint32_t)USER_STACK_PAGES * PAGE_SIZE;
        vsize = img + heap + stk;
        rss   = (img + stk) / PAGE_SIZE;   /* resident pages (coarse) */
    }
    /* tty_nr, tpgid, flags, minflt, cminflt, majflt, cmajflt */
    pappend(content, &pos, sizeof(content),
            " 0 0 0 0 0 0 0");
    /* utime, stime, cutime, cstime, priority, nice */
    pappend(content, &pos, sizeof(content), " 0 0 0 0 20 0 ");
    pappend_int(content, &pos, sizeof(content), nthreads);   /* num_threads */
    /* itrealvalue, starttime */
    pappend(content, &pos, sizeof(content), " 0 0 ");
    pappend_int(content, &pos, sizeof(content), (int)vsize);  /* vsize (bytes) */
    pappend(content, &pos, sizeof(content), " ");
    pappend_int(content, &pos, sizeof(content), (int)rss);    /* rss (pages) */
    pappend(content, &pos, sizeof(content), "\n");
    content[pos] = '\0';

    if (off >= pos) return 0;
    uint32_t avail = pos - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

/* ── /proc/version ────────────────────────────────────────────────────────── */

static uint32_t procfs_version_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                     uint8_t *buf) {
    (void)n;
    static const char content[] =
        "Linux version 5.15.0-maeros (gcc) #1 SMP MaeroOS\n";
    uint32_t total = (uint32_t)(sizeof(content) - 1);
    if (off >= total) return 0;
    uint32_t avail = total - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

/* ── /proc/kmsg ───────────────────────────────────────────────────────────── */

static uint32_t procfs_kmsg_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                 uint8_t *buf) {
    (void)n;
    /* Snapshot the kernel ring buffer once per read pass (at off==0), then
     * serve the linear copy by offset — same pattern as /proc/pci. */
    static char content[65536];
    static uint32_t content_len = 0;

    if (off == 0)
        content_len = klog_snapshot(content, sizeof(content));

    if (off >= content_len) return 0;
    uint32_t avail = content_len - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

/* ── /proc/self/exe ───────────────────────────────────────────────────────── */

static uint32_t procfs_exe_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                 uint8_t *buf) {
    (void)n;
    char content[260];
    uint32_t pos = 0;
    /* No trailing newline: as a symlink target this must be the exact path. */
    if (current_proc && current_proc->exe[0])
        pappend(content, &pos, sizeof(content), current_proc->exe);
    else if (current_proc)
        pappend(content, &pos, sizeof(content), current_proc->name);
    content[pos] = '\0';

    if (off >= pos) return 0;
    uint32_t avail = pos - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

/* ── /proc/self/maps ──────────────────────────────────────────────────────── */

/* Emit one Linux-format maps line: "start-end perms offset dev inode  path\n".
 * prot bits: 1=R 2=W 4=X (matches mmap PROT_*); always private ('p'). */
static void maps_line(char *b, uint32_t *pos, uint32_t cap,
                      uint32_t start, uint32_t end, uint32_t prot,
                      const char *path) {
    char perms[5];
    perms[0] = (prot & 1) ? 'r' : '-';
    perms[1] = (prot & 2) ? 'w' : '-';
    perms[2] = (prot & 4) ? 'x' : '-';
    perms[3] = (prot & 8) ? 's' : 'p';   /* bit 3: MAP_SHARED mapping */
    perms[4] = '\0';
    pappend_hex(b, pos, cap, start, 8);
    pappend(b, pos, cap, "-");
    pappend_hex(b, pos, cap, end, 8);
    pappend(b, pos, cap, " ");
    pappend(b, pos, cap, perms);
    pappend(b, pos, cap, " 00000000 00:00 0 ");
    if (path && path[0]) pappend(b, pos, cap, path);
    pappend(b, pos, cap, "\n");
}

static uint32_t procfs_maps_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                  uint8_t *buf) {
    (void)n;
    static char content[32768];   /* the VMA registry lists every mapping now */
    static uint32_t content_len = 0;

    if (off == 0) {
        uint32_t pos = 0;
        struct proc *p = current_proc;
        if (p) {
            /* Executable image (r-x). */
            if (p->image_end > p->image_start)
                maps_line(content, &pos, sizeof(content),
                          p->image_start, p->image_end, 1 | 4, p->exe);
            /* Heap (rw-), grows up from brk_base to heap_end. */
            if (p->heap_end > p->brk_base)
                maps_line(content, &pos, sizeof(content),
                          p->brk_base, p->heap_end, 1 | 2, "[heap]");
            /* Every mmap()ed region, in address order, from the VMA registry
             * (anonymous, file-backed, shared); the backing file's name for
             * file mappings, 's' for MAP_SHARED. */
            uint32_t vs, ve, vp; int vsh; const char *vname;
            for (int i = 0; proc_vma_iter_ex(p, i, &vs, &ve, &vp, &vsh, &vname) == 0; i++) {
                if (pos > sizeof(content) - 128) break;  /* leave room for [stack] */
                maps_line(content, &pos, sizeof(content), vs, ve,
                          vp | (vsh ? 8 : 0), vname);
            }
        }
        /* The main-thread stack — the line glibc/SpiderMonkey read for stack
         * bounds.  Report the REAL eagerly-mapped range, not a fake 8 KiB. */
        maps_line(content, &pos, sizeof(content),
                  (uint32_t)USER_STACK_BASE, (uint32_t)USER_STACK_TOP,
                  1 | 2, "[stack]");
        content[pos < sizeof(content) ? pos : sizeof(content) - 1] = '\0';
        content_len = pos;
    }

    if (off >= content_len) return 0;
    uint32_t avail = content_len - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

/* ── /proc/self/status ────────────────────────────────────────────────────── */

static uint32_t procfs_status_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                    uint8_t *buf) {
    (void)n;
    return procfs_build_status(current_proc, off, len, buf);
}

/* ── /proc/self/stat ──────────────────────────────────────────────────────── */

static uint32_t procfs_stat_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                  uint8_t *buf) {
    (void)n;
    return procfs_build_stat(current_proc, off, len, buf);
}

/* Copy a byte range out of a captured buffer (cmdline/environ/auxv). */
static uint32_t procfs_copy_blob(const uint8_t *blob, uint32_t blob_len,
                                 uint32_t off, uint32_t len, uint8_t *buf) {
    if (off >= blob_len) return 0;
    uint32_t avail = blob_len - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, blob + off, avail);
    return avail;
}

/* ── /proc/self/cmdline ───────────────────────────────────────────────────── */
static uint32_t procfs_cmdline_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                    uint8_t *buf) {
    (void)n;
    if (!current_proc) return 0;
    return procfs_copy_blob((const uint8_t *)current_proc->cmdline,
                            current_proc->cmdline_len, off, len, buf);
}

/* ── /proc/self/environ ───────────────────────────────────────────────────── */
static uint32_t procfs_environ_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                    uint8_t *buf) {
    (void)n;
    if (!current_proc) return 0;
    return procfs_copy_blob((const uint8_t *)current_proc->environ,
                            current_proc->environ_len, off, len, buf);
}

/* ── /proc/self/auxv (binary type/value pairs) ───────────────────────────── */
static uint32_t procfs_auxv_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                 uint8_t *buf) {
    (void)n;
    if (!current_proc) return 0;
    return procfs_copy_blob(current_proc->auxv_data,
                            current_proc->auxv_bytes, off, len, buf);
}

/* ── /proc/self/statm (size resident shared text lib data dt, in pages) ───── */
static uint32_t procfs_statm_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                  uint8_t *buf) {
    (void)n;
    static char content[128];
    static uint32_t content_len = 0;
    if (off == 0) {
        uint32_t pos = 0;
        struct proc *p = current_proc;
        uint32_t img = (p && p->image_end > p->image_start)
                     ? (p->image_end - p->image_start) / PAGE_SIZE : 1;
        uint32_t heap = (p && p->heap_end > p->brk_base)
                      ? (p->heap_end - p->brk_base) / PAGE_SIZE : 0;
        uint32_t stack = (USER_STACK_PAGES);
        uint32_t total = img + heap + stack;
        pappend_int(content, &pos, sizeof(content), (int)total);   /* size */
        pappend(content, &pos, sizeof(content), " ");
        pappend_int(content, &pos, sizeof(content), (int)total);   /* resident */
        pappend(content, &pos, sizeof(content), " 0 ");            /* shared */
        pappend_int(content, &pos, sizeof(content), (int)img);     /* text */
        pappend(content, &pos, sizeof(content), " 0 ");            /* lib */
        pappend_int(content, &pos, sizeof(content), (int)(heap + stack)); /* data */
        pappend(content, &pos, sizeof(content), " 0\n");           /* dt */
        content_len = pos;
    }
    return procfs_copy_blob((const uint8_t *)content, content_len, off, len, buf);
}

/* ── /proc/self/fd directory ─────────────────────────────────────────────── */

/*
 * readdir: iterate over open file descriptors of current_proc.
 * idx maps to the Nth open fd slot (FD_NONE slots are skipped).
 */
static int procfs_fd_readdir(vfs_node_t *node, uint32_t idx, vfs_dirent_t *out) {
    (void)node;
    if (!current_proc) return -1;

    uint32_t found = 0;
    for (int i = 0; i < MAX_FD; i++) {
        if (current_proc->ofile[i].type == FD_NONE) continue;
        if (found == idx) {
            out->ino  = (uint32_t)i + 100u;
            out->type = VFS_FLAG_FILE;
            /* convert fd number to decimal string */
            char tmp[12];
            uint32_t pos = 0;
            pappend_int(tmp, &pos, sizeof(tmp), i);
            tmp[pos] = '\0';
            strncpy(out->name, tmp, 255);
            out->name[255] = '\0';
            return 0;
        }
        found++;
    }
    return -1;
}

/*
 * finddir: look up a numeric fd name (e.g. "0", "1") in the fd directory.
 * Returns a pointer to a per-fd static node array entry if the fd is open.
 */

/* Static nodes for individual fd entries — one per possible fd */
static vfs_node_t fd_file_nodes[MAX_FD];

static vfs_node_t *procfs_fd_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    if (!current_proc) return NULL;

    /* Parse name as a decimal integer */
    int fd = 0;
    const char *p = name;
    if (*p == '\0') return NULL;
    while (*p >= '0' && *p <= '9') {
        fd = fd * 10 + (*p - '0');
        p++;
    }
    if (*p != '\0') return NULL;  /* non-numeric suffix */
    if (fd < 0 || fd >= MAX_FD)   return NULL;
    if (current_proc->ofile[fd].type == FD_NONE) return NULL;

    /* For a regular file (incl. memfd/tmpfs), return the REAL underlying node so
     * that opening /proc/self/fd/N yields the SAME file object.  Firefox's
     * shared-memory Freeze() reopens /proc/self/fd/N read-only (DupReadOnly) and
     * maps it MAP_SHARED — it MUST share frames with the original fd, or the data
     * written via the first mapping is invisible (SharedStringMap magic mismatch
     * → MOZ_CRASH).  A synthetic empty node breaks that sharing. */
    if (current_proc->ofile[fd].type == FD_FILE && current_proc->ofile[fd].node)
        return current_proc->ofile[fd].node;

    /* Non-file fds (pipes/sockets): a placeholder node (can't be reopened). */
    vfs_node_t *fn = &fd_file_nodes[fd];
    memset(fn, 0, sizeof(vfs_node_t));
    strncpy(fn->name, name, 255);
    fn->name[255] = '\0';
    fn->flags  = VFS_FLAG_FILE;
    fn->inode  = (uint32_t)fd + 100u;
    fn->size   = 0;
    return fn;
}

/* Static node for /proc/self/fd directory */
static vfs_node_t proc_self_fd_node;

/* ── /proc/self directory ─────────────────────────────────────────────────── */

/*
 * Static file nodes for entries inside /proc/self.
 * These are re-used across calls; read_fn always consults current_proc at
 * call time so they are always fresh.
 */
static vfs_node_t proc_self_exe_node;
static vfs_node_t proc_self_maps_node;
static vfs_node_t proc_self_status_node;
static vfs_node_t proc_self_stat_node;
static vfs_node_t proc_self_cmdline_node;
static vfs_node_t proc_self_environ_node;
static vfs_node_t proc_self_auxv_node;
static vfs_node_t proc_self_statm_node;

/* readdir for /proc/self: enumerate the fixed set of entries */
static int procfs_self_readdir(vfs_node_t *node, uint32_t idx,
                                vfs_dirent_t *out) {
    (void)node;
    static const struct { const char *name; uint32_t type; uint32_t ino; } entries[] = {
        { "exe",     VFS_FLAG_FILE, 10 },
        { "maps",    VFS_FLAG_FILE, 11 },
        { "status",  VFS_FLAG_FILE, 12 },
        { "stat",    VFS_FLAG_FILE, 13 },
        { "fd",      VFS_FLAG_DIR,  14 },
        { "cmdline", VFS_FLAG_FILE, 15 },
        { "environ", VFS_FLAG_FILE, 16 },
        { "auxv",    VFS_FLAG_FILE, 17 },
        { "statm",   VFS_FLAG_FILE, 18 },
    };
    static const uint32_t nentries =
        sizeof(entries) / sizeof(entries[0]);

    if (idx >= nentries) return -1;
    out->ino  = entries[idx].ino;
    out->type = (uint8_t)entries[idx].type;
    strncpy(out->name, entries[idx].name, 255);
    out->name[255] = '\0';
    return 0;
}

/* finddir for /proc/self: map name → static vfs_node_t */
static vfs_node_t *procfs_self_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    if (strcmp(name, "exe")    == 0) return &proc_self_exe_node;
    if (strcmp(name, "maps")   == 0) return &proc_self_maps_node;
    if (strcmp(name, "status") == 0) return &proc_self_status_node;
    if (strcmp(name, "stat")   == 0) return &proc_self_stat_node;
    if (strcmp(name, "fd")     == 0) return &proc_self_fd_node;
    if (strcmp(name, "cmdline")== 0) return &proc_self_cmdline_node;
    if (strcmp(name, "environ")== 0) return &proc_self_environ_node;
    if (strcmp(name, "auxv")   == 0) return &proc_self_auxv_node;
    if (strcmp(name, "statm")  == 0) return &proc_self_statm_node;
    return NULL;
}

/* Static node for /proc/self directory */
static vfs_node_t proc_self_node;

/* ── /proc/<pid> directories ─────────────────────────────────────────────── */

static vfs_node_t proc_pid_dir_nodes[MAX_PROCS];
static vfs_node_t proc_pid_status_nodes[MAX_PROCS];
static vfs_node_t proc_pid_stat_nodes[MAX_PROCS];

static int procfs_slot_for_node(vfs_node_t *node) {
    if (!node || !node->private) return -1;
    struct proc *p = (struct proc *)node->private;
    if (p->state == PROC_UNUSED) return -1;
    return (int)(p - ptable);
}

static uint32_t procfs_pid_status_read(vfs_node_t *n, uint32_t off,
                                       uint32_t len, uint8_t *buf) {
    int slot = procfs_slot_for_node(n);
    if (slot < 0) return 0;
    return procfs_build_status(&ptable[slot], off, len, buf);
}

static uint32_t procfs_pid_stat_read(vfs_node_t *n, uint32_t off,
                                     uint32_t len, uint8_t *buf) {
    int slot = procfs_slot_for_node(n);
    if (slot < 0) return 0;
    return procfs_build_stat(&ptable[slot], off, len, buf);
}

static int procfs_pid_readdir(vfs_node_t *node, uint32_t idx,
                              vfs_dirent_t *out) {
    (void)node;
    static const struct { const char *name; uint32_t type; uint32_t ino; } entries[] = {
        { "status", VFS_FLAG_FILE, 1 },
        { "stat",   VFS_FLAG_FILE, 2 },
    };
    static const uint32_t nentries = sizeof(entries) / sizeof(entries[0]);
    if (idx >= nentries) return -1;

    out->ino  = entries[idx].ino;
    out->type = (uint8_t)entries[idx].type;
    strncpy(out->name, entries[idx].name, 255);
    out->name[255] = '\0';
    return 0;
}

static vfs_node_t *procfs_pid_finddir(vfs_node_t *node, const char *name) {
    int slot = procfs_slot_for_node(node);
    if (slot < 0) return NULL;
    struct proc *p = &ptable[slot];

    if (strcmp(name, "status") == 0) {
        vfs_node_t *n = &proc_pid_status_nodes[slot];
        memset(n, 0, sizeof(*n));
        strncpy(n->name, "status", 255);
        n->flags = VFS_FLAG_FILE;
        n->inode = (uint32_t)p->pid * 100u + 1u;
        n->read_fn = procfs_pid_status_read;
        n->private = p;
        return n;
    }
    if (strcmp(name, "stat") == 0) {
        vfs_node_t *n = &proc_pid_stat_nodes[slot];
        memset(n, 0, sizeof(*n));
        strncpy(n->name, "stat", 255);
        n->flags = VFS_FLAG_FILE;
        n->inode = (uint32_t)p->pid * 100u + 2u;
        n->read_fn = procfs_pid_stat_read;
        n->private = p;
        return n;
    }
    return NULL;
}

static vfs_node_t *procfs_pid_node(struct proc *p) {
    if (!p || p->state == PROC_UNUSED) return NULL;
    int slot = (int)(p - ptable);
    if (slot < 0 || slot >= MAX_PROCS) return NULL;

    vfs_node_t *n = &proc_pid_dir_nodes[slot];
    memset(n, 0, sizeof(*n));
    uint32_t pos = 0;
    pappend_int(n->name, &pos, sizeof(n->name), p->pid);
    n->name[pos] = '\0';
    n->flags = VFS_FLAG_DIR;
    n->inode = (uint32_t)p->pid * 100u;
    n->readdir_fn = procfs_pid_readdir;
    n->finddir_fn = procfs_pid_finddir;
    n->private = p;
    return n;
}

/* ── /proc root directory ─────────────────────────────────────────────────── */

/* Static node for /proc/version */
static vfs_node_t proc_version_node;

/* Static node for the /proc root directory */
static vfs_node_t proc_meminfo_node;
static vfs_node_t proc_uptime_node;
static vfs_node_t proc_cpuinfo_node;
static vfs_node_t proc_pci_node;
static vfs_node_t proc_netif_node;
static vfs_node_t proc_firewall_node;
static vfs_node_t proc_processes_node;
static vfs_node_t proc_kmsg_node;
static vfs_node_t proc_root_node;

/* ── /proc/meminfo ────────────────────────────────────────────────────────── */

static uint32_t procfs_meminfo_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                     uint8_t *buf) {
    (void)n;
    char content[256];
    uint32_t pos = 0;
    uint32_t total_kb = pmm_total_frames() * 4;
    uint32_t free_kb  = pmm_free_frames()  * 4;

    pappend(content, &pos, sizeof(content), "MemTotal:     ");
    pappend_int(content, &pos, sizeof(content), total_kb);
    pappend(content, &pos, sizeof(content), " kB\n");
    pappend(content, &pos, sizeof(content), "MemFree:      ");
    pappend_int(content, &pos, sizeof(content), free_kb);
    pappend(content, &pos, sizeof(content), " kB\n");
    content[pos] = '\0';

    if (off >= pos) return 0;
    uint32_t avail = pos - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

/* ── /proc/uptime ─────────────────────────────────────────────────────────── */

static uint32_t procfs_uptime_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                    uint8_t *buf) {
    (void)n;
    char content[64];
    uint32_t pos = 0;
    uint32_t t   = pit_ticks();          /* 100 Hz since boot */
    uint32_t secs = t / 100;
    uint32_t cs   = t % 100;             /* centiseconds */

    /* "<secs>.<cs> <idle>.<cs>\n" — second field is idle time (reuse secs). */
    pappend_int(content, &pos, sizeof(content), (int)secs);
    pappend(content, &pos, sizeof(content), ".");
    if (cs < 10) pappend(content, &pos, sizeof(content), "0");
    pappend_int(content, &pos, sizeof(content), (int)cs);
    pappend(content, &pos, sizeof(content), " ");
    pappend_int(content, &pos, sizeof(content), (int)secs);
    pappend(content, &pos, sizeof(content), ".");
    if (cs < 10) pappend(content, &pos, sizeof(content), "0");
    pappend_int(content, &pos, sizeof(content), (int)cs);
    pappend(content, &pos, sizeof(content), "\n");
    content[pos] = '\0';

    if (off >= pos) return 0;
    uint32_t avail = pos - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

/* ── /proc/cpuinfo ────────────────────────────────────────────────────────── */

static uint32_t procfs_cpuinfo_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                     uint8_t *buf) {
    (void)n;
    static const char content[] =
        "processor\t: 0\n"
        "vendor_id\t: MaeroOS\n"
        "model name\t: i686\n"
        "cpu MHz\t\t: 1000\n"
        "bogomips\t: 2000.00\n";
    uint32_t total = (uint32_t)(sizeof(content) - 1);
    if (off >= total) return 0;
    uint32_t avail = total - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

/* ── /proc/pci ────────────────────────────────────────────────────────────── */

static uint32_t procfs_pci_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                uint8_t *buf) {
    (void)n;
    static char content[4096];
    static uint32_t content_len = 0;

    if (off == 0) {
        uint32_t pos = 0;
        int count = pci_device_count();
        for (int i = 0; i < count; i++) {
            const pci_device_t *d = pci_get_device(i);
            if (!d) continue;

            pappend_hex(content, &pos, sizeof(content), d->bus, 2);
            pappend(content, &pos, sizeof(content), ":");
            pappend_hex(content, &pos, sizeof(content), d->slot, 2);
            pappend(content, &pos, sizeof(content), ".");
            pappend_hex(content, &pos, sizeof(content), d->func, 1);
            pappend(content, &pos, sizeof(content), " ");
            pappend_hex(content, &pos, sizeof(content), d->vendor_id, 4);
            pappend(content, &pos, sizeof(content), ":");
            pappend_hex(content, &pos, sizeof(content), d->device_id, 4);
            pappend(content, &pos, sizeof(content), " class=");
            pappend_hex(content, &pos, sizeof(content), d->class_code, 2);
            pappend(content, &pos, sizeof(content), ":");
            pappend_hex(content, &pos, sizeof(content), d->subclass, 2);
            pappend(content, &pos, sizeof(content), " prog=");
            pappend_hex(content, &pos, sizeof(content), d->prog_if, 2);
            pappend(content, &pos, sizeof(content), " irq=");
            pappend_int(content, &pos, sizeof(content), d->irq_line);
            pappend(content, &pos, sizeof(content), " bar0=");
            pappend_hex(content, &pos, sizeof(content), d->bar[0], 8);
            pappend(content, &pos, sizeof(content), "\n");
        }
        if (count == 0)
            pappend(content, &pos, sizeof(content), "no pci devices\n");
        content[pos] = '\0';
        content_len = pos;
    }

    if (off >= content_len) return 0;
    uint32_t avail = content_len - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

/* ── /proc/netif ──────────────────────────────────────────────────────────── */

static uint32_t procfs_netif_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                  uint8_t *buf) {
    (void)n;
    char content[512];
    uint32_t pos = 0;
    const rtl8139_info_t *rtl = rtl8139_get_info();
    net_poll_all();

    if (rtl && rtl->present) {
        netif_t *iface = net_find_interface("eth0");
        pappend(content, &pos, sizeof(content), "eth0: rtl8139 up ");
        pappend(content, &pos, sizeof(content), "io=0x");
        pappend_hex(content, &pos, sizeof(content), rtl->io_base, 4);
        pappend(content, &pos, sizeof(content), " irq=");
        pappend_int(content, &pos, sizeof(content), rtl->irq);
        pappend(content, &pos, sizeof(content), " mac=");
        for (int i = 0; i < 6; i++) {
            if (i) pappend(content, &pos, sizeof(content), ":");
            pappend_hex(content, &pos, sizeof(content), rtl->mac[i], 2);
        }
        pappend(content, &pos, sizeof(content), " rx=0x");
        pappend_hex(content, &pos, sizeof(content), rtl->rx_config, 8);
        pappend(content, &pos, sizeof(content), " tx=0x");
        pappend_hex(content, &pos, sizeof(content), rtl->tx_config, 8);
        if (iface) {
            char ipbuf[128];
            pappend(content, &pos, sizeof(content), " txpkts=");
            pappend_int(content, &pos, sizeof(content), iface->tx_packets);
            pappend(content, &pos, sizeof(content), " rxpkts=");
            pappend_int(content, &pos, sizeof(content), iface->rx_packets);
            pappend(content, &pos, sizeof(content), " drop=");
            pappend_int(content, &pos, sizeof(content), iface->rx_dropped);
            if (net_lwip_ipv4(ipbuf, sizeof(ipbuf)) > 0)
                pappend(content, &pos, sizeof(content), ipbuf);
        }
        pappend(content, &pos, sizeof(content), "\n");
    } else {
        pappend(content, &pos, sizeof(content), "no network interfaces\n");
    }
    content[pos] = '\0';

    if (off >= pos) return 0;
    uint32_t avail = pos - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

static uint32_t procfs_netif_write(vfs_node_t *n, uint32_t off, uint32_t len,
                                   const uint8_t *buf) {
    (void)n; (void)off; (void)buf;
    netif_t *iface = net_find_interface("eth0");
    if (!iface)
        return 0;

    uint8_t frame[64];
    memset(frame, 0, sizeof(frame));
    for (int i = 0; i < 6; i++)
        frame[i] = 0xff;
    memcpy(frame + 6, iface->mac, 6);
    frame[12] = 0x88;
    frame[13] = 0xb5;
    memcpy(frame + 14, "MaeroOS netprobe", 16);

    if (net_send(iface, frame, sizeof(frame)) < 0)
        return 0;
    return len;
}

/* ── /proc/firewall ───────────────────────────────────────────────────────── */

static uint32_t procfs_firewall_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                     uint8_t *buf) {
    (void)n;
    static char content[2048];
    uint32_t pos = firewall_dump(content, sizeof(content) - 1);
    content[pos] = '\0';
    if (off >= pos) return 0;
    uint32_t avail = pos - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

/* Each write is one or more newline-separated control lines (fwctl). */
static uint32_t procfs_firewall_write(vfs_node_t *n, uint32_t off, uint32_t len,
                                      const uint8_t *buf) {
    (void)n; (void)off;
    char line[256];
    uint32_t i = 0, l = 0;
    while (i < len) {
        char c = (char)buf[i++];
        if (c == '\n' || l >= sizeof(line) - 1) {
            line[l] = '\0';
            if (l) firewall_ctl_line(line);
            l = 0;
            continue;
        }
        line[l++] = c;
    }
    if (l) { line[l] = '\0'; firewall_ctl_line(line); }
    return len;
}

/* ── /proc/processes ─────────────────────────────────────────────────────── */

static uint32_t procfs_processes_read(vfs_node_t *n, uint32_t off,
                                      uint32_t len, uint8_t *buf) {
    (void)n;
    static char content[4096];
    static uint32_t content_len = 0;

    if (off == 0) {
        uint32_t pos = 0;
        pappend(content, &pos, sizeof(content),
                "PID PPID PGRP SID STATE TTY TIME NAME\n");
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            if (p->state == PROC_UNUSED) continue;
            int ppid = p->parent ? p->parent->pid : 0;

            pappend_int(content, &pos, sizeof(content), p->pid);
            pappend(content, &pos, sizeof(content), " ");
            pappend_int(content, &pos, sizeof(content), ppid);
            pappend(content, &pos, sizeof(content), " ");
            pappend_int(content, &pos, sizeof(content), p->pgrp);
            pappend(content, &pos, sizeof(content), " ");
            pappend_int(content, &pos, sizeof(content), p->sid);
            pappend(content, &pos, sizeof(content), " ");
            pappend(content, &pos, sizeof(content), proc_state_short(p->state));
            pappend(content, &pos, sizeof(content), " ");
            pappend_tty(content, &pos, sizeof(content), p);
            pappend(content, &pos, sizeof(content), " ");
            /* CPU time consumed, in 1/100ths of a second (PIT ticks) */
            pappend_int(content, &pos, sizeof(content),
                        (int)(p->utime_ticks / 100));
            pappend(content, &pos, sizeof(content), ".");
            pappend_int(content, &pos, sizeof(content),
                        (int)(p->utime_ticks % 100));
            pappend(content, &pos, sizeof(content), " ");
            pappend(content, &pos, sizeof(content), p->name);
            pappend(content, &pos, sizeof(content), "\n");
        }
        content[pos] = '\0';
        content_len = pos;
    }

    if (off >= content_len) return 0;
    uint32_t avail = content_len - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

/* readdir for /proc: enumerate fixed pseudo-files */
/* ── /proc/sys/vm/overcommit_memory ──────────────────────────────────────────
 * glibc's malloc and some allocators read this; "0" = heuristic overcommit
 * (the default, allocator-friendly).  We accept writes and ignore them. */
static vfs_node_t proc_sys_node;       /* /proc/sys */
static vfs_node_t proc_sys_vm_node;    /* /proc/sys/vm */
static vfs_node_t proc_overcommit_node;/* /proc/sys/vm/overcommit_memory */

static uint32_t procfs_overcommit_read(vfs_node_t *n, uint32_t off,
                                       uint32_t len, uint8_t *buf) {
    (void)n;
    static const char s[] = "0\n";
    return procfs_copy_blob((const uint8_t *)s, 2, off, len, buf);
}
static uint32_t procfs_overcommit_write(vfs_node_t *n, uint32_t off,
                                        uint32_t len, const uint8_t *buf) {
    (void)n; (void)off; (void)buf;
    return len;   /* accept + ignore */
}

static int procfs_sys_vm_readdir(vfs_node_t *node, uint32_t idx, vfs_dirent_t *out) {
    (void)node;
    if (idx != 0) return -1;
    out->ino = 81; out->type = VFS_FLAG_FILE;
    strncpy(out->name, "overcommit_memory", 255); out->name[255] = '\0';
    return 0;
}
static vfs_node_t *procfs_sys_vm_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    if (strcmp(name, "overcommit_memory") == 0) return &proc_overcommit_node;
    return NULL;
}
static int procfs_sys_readdir(vfs_node_t *node, uint32_t idx, vfs_dirent_t *out) {
    (void)node;
    if (idx != 0) return -1;
    out->ino = 80; out->type = VFS_FLAG_DIR;
    strncpy(out->name, "vm", 255); out->name[255] = '\0';
    return 0;
}
static vfs_node_t *procfs_sys_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    if (strcmp(name, "vm") == 0) return &proc_sys_vm_node;
    return NULL;
}

static int procfs_root_readdir(vfs_node_t *node, uint32_t idx,
                                vfs_dirent_t *out) {
    (void)node;
    static const struct { const char *name; uint32_t type; uint32_t ino; } entries[] = {
        { "version", VFS_FLAG_FILE, 1 },
        { "self",    VFS_FLAG_DIR,  2 },
        { "sys",     VFS_FLAG_DIR,  79 },
        { "meminfo", VFS_FLAG_FILE, 3 },
        { "cpuinfo", VFS_FLAG_FILE, 4 },
        { "pci",     VFS_FLAG_FILE, 5 },
        { "netif",   VFS_FLAG_FILE, 6 },
        { "processes", VFS_FLAG_FILE, 7 },
        { "kmsg",    VFS_FLAG_FILE, 8 },
        { "firewall", VFS_FLAG_FILE, 9 },
        { "uptime",  VFS_FLAG_FILE, 10 },
    };
    static const uint32_t nentries =
        sizeof(entries) / sizeof(entries[0]);

    if (idx < nentries) {
        out->ino  = entries[idx].ino;
        out->type = (uint8_t)entries[idx].type;
        strncpy(out->name, entries[idx].name, 255);
        out->name[255] = '\0';
        return 0;
    }

    uint32_t want = idx - nentries;
    uint32_t found = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_UNUSED) continue;
        if (found++ != want) continue;

        uint32_t pos = 0;
        out->ino = (uint32_t)p->pid * 100u;
        out->type = VFS_FLAG_DIR;
        pappend_int(out->name, &pos, sizeof(out->name), p->pid);
        out->name[pos] = '\0';
        return 0;
    }
    return -1;
}

/* finddir for /proc: map name → vfs_node_t */
static vfs_node_t *procfs_root_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    if (strcmp(name, "version") == 0) return &proc_version_node;
    if (strcmp(name, "self")    == 0) return &proc_self_node;
    if (strcmp(name, "sys")     == 0) return &proc_sys_node;
    if (strcmp(name, "meminfo") == 0) return &proc_meminfo_node;
    if (strcmp(name, "cpuinfo") == 0) return &proc_cpuinfo_node;
    if (strcmp(name, "uptime")  == 0) return &proc_uptime_node;
    if (strcmp(name, "pci")     == 0) return &proc_pci_node;
    if (strcmp(name, "netif")   == 0) return &proc_netif_node;
    if (strcmp(name, "firewall") == 0) return &proc_firewall_node;
    if (strcmp(name, "processes") == 0) return &proc_processes_node;
    if (strcmp(name, "kmsg")    == 0) return &proc_kmsg_node;

    int pid = procfs_parse_pid(name);
    if (pid > 0) {
        struct proc *p = procfs_find_pid(pid);
        if (p) return procfs_pid_node(p);
    }
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

vfs_node_t *procfs_mount(void) {
    /* /proc/version */
    memset(&proc_version_node, 0, sizeof(proc_version_node));
    strncpy(proc_version_node.name, "version", 255);
    proc_version_node.flags   = VFS_FLAG_FILE;
    proc_version_node.inode   = 1;
    proc_version_node.read_fn = procfs_version_read;

    /* /proc/self/exe — a SYMLINK to the running binary.  glibc/Firefox locate
     * their install directory via readlink("/proc/self/exe"); a plain file node
     * makes readlink() return -EINVAL and Firefox aborts with "Couldn't find
     * the application directory". */
    memset(&proc_self_exe_node, 0, sizeof(proc_self_exe_node));
    strncpy(proc_self_exe_node.name, "exe", 255);
    proc_self_exe_node.flags   = VFS_FLAG_SYMLINK;
    proc_self_exe_node.inode   = 10;
    proc_self_exe_node.read_fn = procfs_exe_read;

    /* /proc/self/maps */
    memset(&proc_self_maps_node, 0, sizeof(proc_self_maps_node));
    strncpy(proc_self_maps_node.name, "maps", 255);
    proc_self_maps_node.flags   = VFS_FLAG_FILE;
    proc_self_maps_node.inode   = 11;
    proc_self_maps_node.read_fn = procfs_maps_read;

    /* /proc/self/status */
    memset(&proc_self_status_node, 0, sizeof(proc_self_status_node));
    strncpy(proc_self_status_node.name, "status", 255);
    proc_self_status_node.flags   = VFS_FLAG_FILE;
    proc_self_status_node.inode   = 12;
    proc_self_status_node.read_fn = procfs_status_read;

    /* /proc/self/stat */
    memset(&proc_self_stat_node, 0, sizeof(proc_self_stat_node));
    strncpy(proc_self_stat_node.name, "stat", 255);
    proc_self_stat_node.flags   = VFS_FLAG_FILE;
    proc_self_stat_node.inode   = 13;
    proc_self_stat_node.read_fn = procfs_stat_read;

    /* /proc/self/fd directory */
    memset(&proc_self_fd_node, 0, sizeof(proc_self_fd_node));
    strncpy(proc_self_fd_node.name, "fd", 255);
    proc_self_fd_node.flags       = VFS_FLAG_DIR;
    proc_self_fd_node.inode       = 14;
    proc_self_fd_node.readdir_fn  = procfs_fd_readdir;
    proc_self_fd_node.finddir_fn  = procfs_fd_finddir;

    /* /proc/self/{cmdline,environ,auxv,statm} */
    memset(&proc_self_cmdline_node, 0, sizeof(proc_self_cmdline_node));
    strncpy(proc_self_cmdline_node.name, "cmdline", 255);
    proc_self_cmdline_node.flags   = VFS_FLAG_FILE;
    proc_self_cmdline_node.inode   = 15;
    proc_self_cmdline_node.read_fn = procfs_cmdline_read;

    memset(&proc_self_environ_node, 0, sizeof(proc_self_environ_node));
    strncpy(proc_self_environ_node.name, "environ", 255);
    proc_self_environ_node.flags   = VFS_FLAG_FILE;
    proc_self_environ_node.inode   = 16;
    proc_self_environ_node.read_fn = procfs_environ_read;

    memset(&proc_self_auxv_node, 0, sizeof(proc_self_auxv_node));
    strncpy(proc_self_auxv_node.name, "auxv", 255);
    proc_self_auxv_node.flags   = VFS_FLAG_FILE;
    proc_self_auxv_node.inode   = 17;
    proc_self_auxv_node.read_fn = procfs_auxv_read;

    memset(&proc_self_statm_node, 0, sizeof(proc_self_statm_node));
    strncpy(proc_self_statm_node.name, "statm", 255);
    proc_self_statm_node.flags   = VFS_FLAG_FILE;
    proc_self_statm_node.inode   = 18;
    proc_self_statm_node.read_fn = procfs_statm_read;

    /* /proc/self directory */
    memset(&proc_self_node, 0, sizeof(proc_self_node));
    strncpy(proc_self_node.name, "self", 255);
    proc_self_node.flags       = VFS_FLAG_DIR;
    proc_self_node.inode       = 2;
    proc_self_node.readdir_fn  = procfs_self_readdir;
    proc_self_node.finddir_fn  = procfs_self_finddir;

    /* /proc/meminfo */
    memset(&proc_meminfo_node, 0, sizeof(proc_meminfo_node));
    strncpy(proc_meminfo_node.name, "meminfo", 255);
    proc_meminfo_node.flags   = VFS_FLAG_FILE;
    proc_meminfo_node.inode   = 3;
    proc_meminfo_node.read_fn = procfs_meminfo_read;

    /* /proc/cpuinfo */
    memset(&proc_cpuinfo_node, 0, sizeof(proc_cpuinfo_node));
    strncpy(proc_cpuinfo_node.name, "cpuinfo", 255);
    proc_cpuinfo_node.flags   = VFS_FLAG_FILE;
    proc_cpuinfo_node.inode   = 4;
    proc_cpuinfo_node.read_fn = procfs_cpuinfo_read;

    /* /proc/uptime */
    memset(&proc_uptime_node, 0, sizeof(proc_uptime_node));
    strncpy(proc_uptime_node.name, "uptime", 255);
    proc_uptime_node.flags   = VFS_FLAG_FILE;
    proc_uptime_node.inode   = 10;
    proc_uptime_node.read_fn = procfs_uptime_read;

    /* /proc/pci */
    memset(&proc_pci_node, 0, sizeof(proc_pci_node));
    strncpy(proc_pci_node.name, "pci", 255);
    proc_pci_node.flags   = VFS_FLAG_FILE;
    proc_pci_node.inode   = 5;
    proc_pci_node.read_fn = procfs_pci_read;

    /* /proc/netif */
    memset(&proc_netif_node, 0, sizeof(proc_netif_node));
    strncpy(proc_netif_node.name, "netif", 255);
    proc_netif_node.flags   = VFS_FLAG_FILE;
    proc_netif_node.inode   = 6;
    proc_netif_node.read_fn = procfs_netif_read;
    proc_netif_node.write_fn = procfs_netif_write;

    /* /proc/firewall (root-only: 0600) */
    memset(&proc_firewall_node, 0, sizeof(proc_firewall_node));
    strncpy(proc_firewall_node.name, "firewall", 255);
    proc_firewall_node.flags    = VFS_FLAG_FILE;
    proc_firewall_node.inode    = 9;
    proc_firewall_node.mask     = 0600;
    proc_firewall_node.read_fn  = procfs_firewall_read;
    proc_firewall_node.write_fn = procfs_firewall_write;

    /* /proc/processes */
    memset(&proc_processes_node, 0, sizeof(proc_processes_node));
    strncpy(proc_processes_node.name, "processes", 255);
    proc_processes_node.flags   = VFS_FLAG_FILE;
    proc_processes_node.inode   = 7;
    proc_processes_node.read_fn = procfs_processes_read;

    /* /proc/kmsg */
    memset(&proc_kmsg_node, 0, sizeof(proc_kmsg_node));
    strncpy(proc_kmsg_node.name, "kmsg", 255);
    proc_kmsg_node.flags   = VFS_FLAG_FILE;
    proc_kmsg_node.inode   = 8;
    proc_kmsg_node.read_fn = procfs_kmsg_read;

    /* /proc root directory */
    memset(&proc_root_node, 0, sizeof(proc_root_node));
    strncpy(proc_root_node.name, "proc", 255);
    proc_root_node.flags       = VFS_FLAG_DIR;
    proc_root_node.inode       = 0;
    proc_root_node.readdir_fn  = procfs_root_readdir;
    proc_root_node.finddir_fn  = procfs_root_finddir;

    /* /proc/sys, /proc/sys/vm, /proc/sys/vm/overcommit_memory */
    memset(&proc_sys_node, 0, sizeof(proc_sys_node));
    strncpy(proc_sys_node.name, "sys", 255);
    proc_sys_node.flags      = VFS_FLAG_DIR;
    proc_sys_node.inode      = 79;
    proc_sys_node.readdir_fn = procfs_sys_readdir;
    proc_sys_node.finddir_fn = procfs_sys_finddir;

    memset(&proc_sys_vm_node, 0, sizeof(proc_sys_vm_node));
    strncpy(proc_sys_vm_node.name, "vm", 255);
    proc_sys_vm_node.flags      = VFS_FLAG_DIR;
    proc_sys_vm_node.inode      = 80;
    proc_sys_vm_node.readdir_fn = procfs_sys_vm_readdir;
    proc_sys_vm_node.finddir_fn = procfs_sys_vm_finddir;

    memset(&proc_overcommit_node, 0, sizeof(proc_overcommit_node));
    strncpy(proc_overcommit_node.name, "overcommit_memory", 255);
    proc_overcommit_node.flags    = VFS_FLAG_FILE;
    proc_overcommit_node.inode    = 81;
    proc_overcommit_node.read_fn  = procfs_overcommit_read;
    proc_overcommit_node.write_fn = procfs_overcommit_write;

    return &proc_root_node;
}
