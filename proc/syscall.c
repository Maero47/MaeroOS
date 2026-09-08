#include "syscall.h"
#include "scheduler.h"
#include "process.h"
#include "signal.h"
#include "pipe.h"
#include "usocket.h"
#include "shm.h"
#include "elf.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../arch/i686/mm/paging.h"
#include "../arch/i686/mm/tlb.h"
#include "../drivers/serial.h"
#include "../drivers/vga.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../kernel/random.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/cpu/tsc.h"
#include "../arch/i686/cpu/gdt.h"
#include "../arch/i686/cpu/fpu.h"
#include "../arch/i686/cpu/cpuid.h"
#include "../drivers/rtc.h"
#include "../arch/i686/include/io.h"
#include "../kernel/printk.h"
#include "../fs/vfs.h"
#include "../fs/devfs.h"
#include "../net/socket.h"
#include <registers.h>
#include <kernel/config.h>
#include <stdint.h>
#include <stddef.h>

/* ── Kernel-internal ABI structs (matching Linux i386 userspace layout) ──── */

/* Old 32-bit struct stat — sys_stat(106) / sys_fstat(108) */
struct kstat {
    uint16_t st_dev;
    uint16_t __pad1;
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint16_t st_rdev;
    uint16_t __pad2;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
} __attribute__((packed));

/* struct stat64 — sys_stat64(195) / sys_fstat64(197) / sys_lstat64(196) */
struct kstat64 {
    uint64_t st_dev;
    uint8_t  __pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t  __pad3[4];
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint64_t st_ino;
} __attribute__((packed));

/* struct utsname — sys_uname(122) */
struct kutsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

/* linux_dirent — sys_getdents(141) */
struct linux_dirent {
    uint32_t d_ino;
    uint32_t d_off;
    uint16_t d_reclen;
    char     d_name[1];   /* variable; d_type byte sits after the NUL */
} __attribute__((packed));

/* linux_dirent64 — sys_getdents64(220) */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1];   /* variable */
} __attribute__((packed));

/* struct timeval / timespec */
struct ktimeval  { int32_t tv_sec; int32_t tv_usec; };
struct ktimespec { int32_t tv_sec; int32_t tv_nsec; };

#define AT_FDCWD      (-100)
#define AT_REMOVEDIR  0x200

/* ── Helpers ──────────────────────────────────────────────────────────────── */

int vma_prot_lookup(uint32_t addr);   /* defined with the VMA registry below */
static int vma_range_free(uint32_t va, uint32_t length);

int access_ok(const void *ptr, size_t len) {
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t end = addr + len;

    if (len == 0)
        return addr <= 0xC0000000U;
    if (addr >= 0xC0000000U || end > 0xC0000000U || end < addr)
        return 0;

    uintptr_t page = addr & ~(uintptr_t)(PAGE_SIZE - 1);
    uintptr_t last = (end - 1) & ~(uintptr_t)(PAGE_SIZE - 1);

    for (;;) {
        uint32_t *pde = paging_get_pde((uint32_t)page);
        uint32_t pte = (*pde & PAGE_PRESENT) ? *paging_get_pte((uint32_t)page) : 0;
        if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER)) {
            /* Not populated: fine if a VMA with access covers it — the copy
             * demand-faults it in (Linux access_ok only checks the range; the
             * fault path does the rest).  A PROT_NONE page or PROT_NONE VMA is
             * not accessible. */
            if (pte & PAGE_PROTNONE) return 0;
            if (vma_prot_lookup((uint32_t)page) <= 0) return 0;
        }
        if (page == last)
            break;
        page += PAGE_SIZE;
    }

    return 1;
}

/*
 * SMP-safe user copies.  access_ok() validates the range, but under -smp 2 the
 * mapping can change between the check and the copy (a sibling CPU tearing the
 * address space down when the process is killed mid-syscall, or a COW double-
 * break) → the copy faults in kernel mode.  To recover instead of panicking we
 * use a Linux-style exception table: the `rep movsb` is registered in __ex_table
 * with a fixup label; if it faults, the page-fault handler redirects EIP to the
 * fixup, which sets the return to -EFAULT.  See fixup_exception() in paging.c.
 */
int copy_from_user(void *dst, const void *src, size_t len) {
    if (!access_ok(src, len))
        return -14;
    int ret = 0;
    __asm__ volatile(
        "   cld\n"
        "1: rep movsb\n"
        "   jmp 2f\n"
        "3: movl $-14, %[ret]\n"        /* fixup: -EFAULT */
        "2:\n"
        ".pushsection __ex_table,\"a\"\n"
        ".balign 4\n"
        ".long 1b\n"                     /* faulting insn  */
        ".long 3b\n"                     /* fixup target   */
        ".popsection\n"
        : [ret] "+r"(ret), "+S"(src), "+D"(dst), "+c"(len)
        : : "memory", "cc");
    return ret;
}

int copy_to_user(void *dst, const void *src, size_t len) {
    if (!access_ok(dst, len))
        return -14;
    int ret = 0;
    __asm__ volatile(
        "   cld\n"
        "1: rep movsb\n"
        "   jmp 2f\n"
        "3: movl $-14, %[ret]\n"        /* fixup: -EFAULT */
        "2:\n"
        ".pushsection __ex_table,\"a\"\n"
        ".balign 4\n"
        ".long 1b\n"
        ".long 3b\n"
        ".popsection\n"
        : [ret] "+r"(ret), "+S"(src), "+D"(dst), "+c"(len)
        : : "memory", "cc");
    return ret;
}

/*
 * Deliver a fatal-by-default signal to the current process for a ring-3 CPU
 * exception (GP fault, invalid opcode, …) instead of panicking the kernel —
 * Linux behaviour.  Returns 1 if handled (a user process exists), 0 to let the
 * arch code panic (no current process / kernel-mode fault).  Mirrors the user
 * SIGSEGV path in page_fault_handler: a same-EIP fault loop forces terminate.
 */
int user_fault_signal(registers_t *regs, int sig) {
    if (!current_proc) return 0;
    printk("[SIG] pid=%d fatal exception int=%d eip=%08x cs=%04x -> signal %d\n",
           current_proc->pid, (int)regs->int_no, (unsigned)regs->eip,
           (unsigned)(regs->cs & 0xFFFF), sig);
    if (regs->eip == current_proc->last_fault_eip) {
        if (++current_proc->fault_repeat >= 3)
            proc_group_exit(sig);            /* does not return */
    } else {
        current_proc->last_fault_eip = regs->eip;
        current_proc->fault_repeat   = 0;
    }
    signal_send(current_proc, sig);
    signal_deliver_pending(regs);            /* → handler, or proc_exit if SIG_DFL */
    return 1;
}

/*
 * Return last '/' position in path, or -1 if none after the initial '/'.
 * Used by sys_open to resolve the parent directory for O_CREAT.
 */
static int path_split(const char *path, char *dir_out, char *base_out) {
    int len = 0;
    while (path[len]) len++;

    int slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { slash = i; break; }
    }
    if (slash < 0) return -1;   /* no slash */

    /* dir part */
    if (slash == 0) {
        dir_out[0] = '/';
        dir_out[1] = '\0';
    } else {
        __builtin_memcpy(dir_out, path, (size_t)slash);
        dir_out[slash] = '\0';
    }

    /* base part */
    __builtin_memcpy(base_out, path + slash + 1, (size_t)(len - slash - 1));
    base_out[len - slash - 1] = '\0';
    return 0;
}

/*
 * vfs_open_at — like vfs_open() but handles relative paths by prepending cwd.
 * Requires current_proc to be set.
 */
static vfs_node_t *vfs_open_at(const char *path) {
    if (!path) return NULL;
    if (path[0] == '/') return vfs_open(path);
    if (!current_proc) return NULL;
    /* Relative: prepend cwd */
    char abspath[512];
    uint32_t cwdlen = (uint32_t)__builtin_strlen(current_proc->cwd);
    __builtin_memcpy(abspath, current_proc->cwd, cwdlen);
    if (cwdlen > 1) abspath[cwdlen++] = '/';
    uint32_t pathlen = (uint32_t)__builtin_strlen(path);
    if (cwdlen + pathlen >= 512) return NULL;
    __builtin_memcpy(abspath + cwdlen, path, pathlen + 1);
    return vfs_open(abspath);
}

static int path_is_root(const char *path) {
    return path && path[0] == '/' && path[1] == '\0';
}

static vfs_node_t *vfs_open_parent_at(const char *full_path,
                                      const char *dir_path) {
    if (full_path && full_path[0] == '/' && path_is_root(dir_path) &&
        vfs_path_uses_root_overlay(full_path)) {
        vfs_node_t *overlay = vfs_get_root_overlay();
        if (overlay) return overlay;
    }
    return vfs_open_at(dir_path);
}

static int canonicalize_path_at_cwd(const char *path, char *out,
                                    uint32_t out_size) {
    if (!path || !out || out_size < 2 || !current_proc) return -22;

    char combined[512];
    uint32_t len = 0;
    if (path[0] == '/') {
        combined[len++] = '/';
    } else {
        uint32_t cwdlen = (uint32_t)__builtin_strlen(current_proc->cwd);
        if (cwdlen >= sizeof(combined)) return -36;
        __builtin_memcpy(combined, current_proc->cwd, cwdlen);
        len = cwdlen;
        if (len == 0 || combined[0] != '/') {
            combined[0] = '/';
            len = 1;
        }
        if (len > 1 && len < sizeof(combined) - 1)
            combined[len++] = '/';
    }

    uint32_t pathlen = (uint32_t)__builtin_strlen(path);
    if (len + pathlen >= sizeof(combined)) return -36;
    __builtin_memcpy(combined + len, path, pathlen + 1);

    const char *components[64];
    uint32_t comp_len[64];
    uint32_t depth = 0;
    const char *p = combined;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        uint32_t clen = 0;
        while (*p && *p != '/') { p++; clen++; }
        if (clen == 1 && start[0] == '.') {
            continue;
        }
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            if (depth > 0) depth--;
            continue;
        }
        if (depth >= (sizeof(components) / sizeof(components[0]))) return -36;
        components[depth] = start;
        comp_len[depth] = clen;
        depth++;
    }

    uint32_t pos = 0;
    out[pos++] = '/';
    for (uint32_t i = 0; i < depth; i++) {
        if (pos > 1) out[pos++] = '/';
        if (pos + comp_len[i] >= out_size) return -36;
        __builtin_memcpy(out + pos, components[i], comp_len[i]);
        pos += comp_len[i];
    }
    out[pos] = '\0';
    return 0;
}

static int canonicalize_path_at_base(const char *base, const char *path,
                                     char *out, uint32_t out_size) {
    if (!base || !path || !out || out_size < 2 || base[0] != '/')
        return -22;
    if (path[0] == '/')
        return canonicalize_path_at_cwd(path, out, out_size);

    char combined[512];
    uint32_t blen = (uint32_t)__builtin_strlen(base);
    uint32_t plen = (uint32_t)__builtin_strlen(path);
    if (blen + 1 + plen >= sizeof(combined)) return -36;
    __builtin_memcpy(combined, base, blen);
    uint32_t len = blen;
    if (len > 1) combined[len++] = '/';
    __builtin_memcpy(combined + len, path, plen + 1);

    const char *old_cwd = current_proc->cwd;
    (void)old_cwd;
    return canonicalize_path_at_cwd(combined, out, out_size);
}

static int resolve_path_at_fd(int dirfd, const char *path, char *out,
                              uint32_t out_size) {
    if (!path || !out) return -22;
    if (path[0] == '/' || dirfd == AT_FDCWD)
        return canonicalize_path_at_cwd(path, out, out_size);
    if (dirfd < 0 || dirfd >= MAX_FD) return -9;

    proc_file_t *f = &current_proc->ofile[dirfd];
    if (f->type != FD_FILE || !f->node) return -9;
    if (!(f->node->flags & VFS_FLAG_DIR)) return -20;
    if (f->path[0] != '/') return -9;
    return canonicalize_path_at_base(f->path, path, out, out_size);
}

/* Copy a NUL-terminated string from user space into kernel buffer kbuf[max]. */
static int copy_user_str(const char *ustr, char *kbuf, int max) {
    if (!access_ok(ustr, 1)) return -14;
    int i = 0;
    for (; i < max - 1; i++) {
        char c;
        if (copy_from_user(&c, ustr + i, 1) < 0) return -14;
        kbuf[i] = c;
        if (c == '\0') return i;
    }
    kbuf[i] = '\0';
    return -36; /* ENAMETOOLONG */
}

static uint32_t vnode_mode(vfs_node_t *n) {
    /* Filesystems store permissions in mask (often without the S_IFMT
     * type bits) — synthesize the type from the VFS node kind so the
     * userspace S_ISREG/S_ISDIR family actually works. */
    uint32_t perm = n->mask ? (n->mask & 07777U)
                            : (n->flags == VFS_FLAG_DIR ? 0755U : 0644U);
    uint32_t type = n->mask & 0170000U;

    if (!type) {
        switch (n->flags & 0x7U) {
        case VFS_FLAG_DIR:     type = 0040000U; break;
        case VFS_FLAG_CHARDEV: type = 0020000U; break;
        case VFS_FLAG_BLKDEV:  type = 0060000U; break;
        case VFS_FLAG_PIPE:
        case VFS_FLAG_FIFO:    type = 0010000U; break;
        case VFS_FLAG_SYMLINK: type = 0120000U; break;
        default:               type = 0100000U; break;
        }
    }
    return type | perm;
}

/* VFS node kind → Linux dirent d_type (DT_*) */
static uint8_t vfs_type_to_dt(uint8_t t) {
    switch (t & 0x7U) {
    case VFS_FLAG_DIR:     return 4;    /* DT_DIR  */
    case VFS_FLAG_CHARDEV: return 2;    /* DT_CHR  */
    case VFS_FLAG_BLKDEV:  return 6;    /* DT_BLK  */
    case VFS_FLAG_PIPE:
    case VFS_FLAG_FIFO:    return 1;    /* DT_FIFO */
    case VFS_FLAG_SYMLINK: return 10;   /* DT_LNK  */
    default:               return 8;    /* DT_REG  */
    }
}

static void fill_kstat(struct kstat *st, vfs_node_t *n) {
    __builtin_memset(st, 0, sizeof(*st));
    st->st_ino     = n->inode;
    st->st_mode    = (uint16_t)vnode_mode(n);
    st->st_nlink   = 1;
    st->st_size    = n->size;
    st->st_blksize = 4096;
    st->st_blocks  = (n->size + 511) / 512;
    st->st_atime   = n->atime;
    st->st_mtime   = n->mtime;
    st->st_ctime   = n->ctime;
}

static void fill_kstat64(struct kstat64 *st, vfs_node_t *n) {
    __builtin_memset(st, 0, sizeof(*st));
    st->st_ino     = n->inode;
    st->__st_ino   = n->inode;
    st->st_mode    = vnode_mode(n);
    st->st_nlink   = 1;
    st->st_size    = (int64_t)n->size;
    st->st_blksize = 4096;
    st->st_blocks  = (n->size + 511) / 512;
    st->st_atime   = n->atime;
    st->st_mtime   = n->mtime;
    st->st_ctime   = n->ctime;
}

static int sys_mkdir_kernel_path(const char *path);
static int sys_unlink_kernel_path(const char *path);
static void fx_lockdbg(const char *op, const char *path, int rc);
static void io_wait_sleep(uint32_t max_ticks);
static void epoll_retain(struct epoll *ep);
static void epoll_release(struct epoll *ep);
static void vfork_wake_parent(void);                  /* CLONE_VFORK unblock */
void vma_clear(struct proc *p);                       /* free a proc's VMAs */
void vma_clone(struct proc *parent, struct proc *child);

/* ── eventfd ──────────────────────────────────────────────────────────────
 * A Linux-compatible eventfd: a 64-bit counter shared by all dup'd fds.  write
 * adds an 8-byte value; read returns (and clears, or in SEMAPHORE mode
 * decrements by 1) the counter, blocking while it is 0.  Crucially, write/read
 * call io_wake() so a thread polling the eventfd in another thread's event loop
 * is roused immediately — this is exactly how Firefox/glib/libevent signal
 * cross-thread work, which previously returned ENOSYS and stalled. */
#define EFD_SEMAPHORE 1
struct eventfd_obj {
    uint64_t count;
    int      flags;       /* EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC */
    int      refcount;
};

static void eventfd_release(struct eventfd_obj *e) {
    if (!e) return;
    if (--e->refcount <= 0) kfree(e);
}

static int eventfd_read(struct eventfd_obj *e, char *buf, int len, int nonblock) {
    if (len < 8) return -22;                       /* -EINVAL */
    while (e->count == 0) {
        if (nonblock) return -11;                  /* -EAGAIN */
        if (signal_interrupt_pending(current_proc)) return -4;  /* -EINTR */
        sleep_on(e);
    }
    uint64_t out;
    if (e->flags & EFD_SEMAPHORE) { out = 1; e->count -= 1; }
    else                          { out = e->count; e->count = 0; }
    __builtin_memcpy(buf, &out, 8);
    wake_up(e);                                    /* wake blocked writers */
    io_wake();                                     /* wake pollers (space avail) */
    return 8;
}

static int eventfd_write(struct eventfd_obj *e, const char *buf, int len, int nonblock) {
    if (len < 8) return -22;                        /* -EINVAL */
    uint64_t add;
    __builtin_memcpy(&add, buf, 8);
    if (add == 0xFFFFFFFFFFFFFFFFULL) return -22;   /* -EINVAL: ~0 is reserved */
    /* Block while the add would push the counter past its max (0xFFFF…FFFE). */
    while (e->count + add < e->count ||
           e->count + add > 0xFFFFFFFFFFFFFFFEULL) {
        if (nonblock) return -11;                   /* -EAGAIN */
        if (signal_interrupt_pending(current_proc)) return -4;  /* -EINTR */
        sleep_on(e);
    }
    e->count += add;
    wake_up(e);                                     /* wake blocked readers */
    io_wake();                                      /* wake pollers (now readable) */
    return 8;
}

/* eventfd(initval) / eventfd2(initval, flags) → new fd. */
static int sys_eventfd(unsigned int initval, int flags) {
    struct eventfd_obj *e = (struct eventfd_obj *)kmalloc(sizeof(*e));
    if (!e) return -12;
    e->count    = initval;
    e->flags    = flags;
    e->refcount = 1;
    int fd = -1;
    for (int i = 0; i < MAX_FD; i++) {
        if (current_proc->ofile[i].type == FD_NONE) { fd = i; break; }
    }
    if (fd < 0) { kfree(e); return -24; }   /* -EMFILE */
    current_proc->ofile[fd].type    = FD_EVENTFD;
    current_proc->ofile[fd].efd     = e;
    current_proc->ofile[fd].flags   = (flags & 0x800) ? O_NONBLOCK : 0;  /* EFD_NONBLOCK */
    current_proc->ofile[fd].cloexec = (flags & 0x80000) ? 1 : 0;          /* EFD_CLOEXEC */
    return fd;
}

void fd_retain(proc_file_t *f) {
    if (f->type == FD_FILE)    vfs_retain(f->node);
    if (f->type == FD_PIPE_R)  f->pipe->nreaders++;
    if (f->type == FD_PIPE_W)  f->pipe->nwriters++;
    if (f->type == FD_SOCKET)  net_socket_retain(f->socket);
    if (f->type == FD_USOCKET) usocket_retain(f->usock);
    if (f->type == FD_EPOLL)   epoll_retain(f->epoll);
    if (f->type == FD_EVENTFD) f->efd->refcount++;
}

void fd_release(proc_file_t *f) {
    if (f->type == FD_FILE)    vfs_close(f->node);
    if (f->type == FD_PIPE_R)  pipe_close_read(f->pipe);
    if (f->type == FD_PIPE_W)  pipe_close_write(f->pipe);
    if (f->type == FD_SOCKET)  net_socket_release(f->socket);
    if (f->type == FD_USOCKET) usocket_release(f->usock);
    if (f->type == FD_EPOLL)   epoll_release(f->epoll);
    if (f->type == FD_EVENTFD) eventfd_release(f->efd);
    f->type    = FD_NONE;
    f->node    = NULL;
    f->pipe    = NULL;
    f->socket  = NULL;
    f->usock   = NULL;
    f->epoll   = NULL;
    f->efd     = NULL;
    f->offset  = 0;
    f->flags   = 0;
    f->cloexec = 0;
    f->path[0] = '\0';
}

/* ── Shared, reference-counted fd table ──────────────────────────────────────
 * Threads of a process share one fdtable (CLONE_FILES); fork gives the child a
 * private copy.  Without this, a thread saw only a stale COPY of the fd table
 * taken at clone time — so an fd opened by the main thread after a worker
 * started was invisible to that worker (the compositor's render-buffer memfd
 * mmap'd EBADF, blocking all painting). */
struct fdtable *fdtable_alloc(void) {
    struct fdtable *t = (struct fdtable *)kmalloc(sizeof(*t));
    if (!t) return NULL;
    __builtin_memset(t, 0, sizeof(*t));
    t->refcount = 1;
    return t;
}
void fdtable_attach(struct proc *p, struct fdtable *t) {
    p->fdt   = t;
    p->ofile = t ? t->f : (proc_file_t *)0;
}
void fdtable_put(struct proc *p) {
    struct fdtable *t = p->fdt;
    p->fdt = (struct fdtable *)0;
    p->ofile = (proc_file_t *)0;
    if (!t) return;
    if (--t->refcount > 0) return;       /* other threads still share it */
    for (int i = 0; i < MAX_FD; i++)
        if (t->f[i].type != FD_NONE) fd_release(&t->f[i]);
    kfree(t);
}

/*
 * Syscall dispatch — called from isr_handler when int_no == 128.
 *
 * Linux-compatible ABI (int 0x80):
 *   EAX = syscall number
 *   EBX = arg1,  ECX = arg2,  EDX = arg3
 *   Return value in EAX (negative = error).
 */

/* ── sys_exit(int status) — EAX=1 ─────────────────────────────────────── */
/* exit() ends the CALLING THREAD only (Linux do_exit; glibc's pthread_exit
 * path).  The status is wait-encoded here as (code & 0xff) << 8, the form
 * WIFEXITED/WEXITSTATUS expect; a death by signal stores the bare signal
 * number instead (kernel/exit.c: tsk->exit_code = code, do_group_exit(sig)). */
static void sys_exit(registers_t *regs) {
    printk("[SYSCALL] sys_exit(%d) from pid %d\n",
           (int)regs->ebx, current_proc ? current_proc->pid : -1);
    proc_exit(((int)regs->ebx & 0xff) << 8);   /* noreturn */
}

/* Linux clone(2) flag bits (uapi/linux/sched.h); used by do_fork and sys_clone. */
#define CLONE_VM             0x00000100
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_VFORK          0x00004000
#define CLONE_THREAD         0x00010000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_SETTLS         0x00080000
#define CLONE_CHILD_SETTID   0x01000000

/* Core fork.  child_stack==0 → child shares the parent's stack pointer (classic
 * fork).  child_stack!=0 → child runs on that user stack instead (clone without
 * CLONE_VM but WITH a stack — e.g. Google Breakpad's crash dumper, which clones
 * a frozen copy of the address space onto a fresh stack and runs an entry fn).
 * Honouring child_stack here is essential: otherwise the child runs the glibc
 * clone trampoline on the parent's stack, pops garbage as its entry fn, and
 * jumps into the weeds. */
static int do_fork(registers_t *regs, uint32_t child_stack, uint32_t clone_flags,
                   uint32_t uptid, uint32_t uctid) {
    (void)regs;
    struct proc *parent = current_proc;
    if (!parent) return -1;

    /* Allocate child process slot */
    struct proc *child = allocproc();
    if (!child) return -11;  /* -ENOMEM */

    /* Copy parent's trapframe; child returns 0 from fork */
    __builtin_memcpy(child->tf, parent->tf, sizeof(registers_t));
    child->tf->eax = 0;

    /* The child belongs to the forking PROCESS, not to the forking thread
     * (Linux copy_process: p->real_parent = current->group_leader), so any
     * thread of the parent may wait for it and its SIGCHLD goes to the process. */
    child->parent = proc_group_leader(parent);
    __builtin_memcpy(child->name, parent->name, sizeof(parent->name));

    /* Inherit a private COPY of the handler table (Linux copy_sighand without
     * CLONE_SIGHAND); clear pending signals in child, keep the blocked mask. */
    if (parent->sighand) {
        __builtin_memcpy(child->sighand->handlers, parent->sighand->handlers,
                         sizeof(child->sighand->handlers));
        __builtin_memcpy(child->sighand->flags, parent->sighand->flags,
                         sizeof(child->sighand->flags));
    }
    child->pending_sigs  = 0;
    child->blocked_sigs  = parent->blocked_sigs;
    child->sigframe_addr = 0;

    /* Inherit working directory, heap break, umask, mmap state, and pgrp.
     * heap_end/mmap_next live on the thread-group LEADER, so when a non-leader
     * worker thread forks, read them from the leader (else the child's brk/mmap
     * cursors are stale and new allocations collide with cloned mappings). */
    struct proc *fowner = parent;
    if (parent->tgid != parent->pid)
        for (int i = 0; i < MAX_PROCS; i++)
            if (ptable[i].state != PROC_UNUSED && ptable[i].pid == parent->tgid) {
                fowner = &ptable[i]; break;
            }
    __builtin_memcpy(child->cwd, parent->cwd, sizeof(parent->cwd));
    child->heap_end  = fowner->heap_end;
    child->umask     = parent->umask;
    child->uid = parent->uid; child->gid = parent->gid;
    child->euid = parent->euid; child->egid = parent->egid;
    child->mmap_next = fowner->mmap_next;
    child->pgrp      = parent->pgrp;
    child->sid       = parent->sid;
    /* Inherit the TLS base: the child continues executing the calling thread's
     * code, which uses %gs-relative TLS (thread pointer, stack guard).  Without
     * this the scheduler rebases the child's GDT entry 6 to 0 and the first
     * %gs access faults — fatal for any fork() in a threaded process (e.g.
     * Firefox forking a subprocess).  The clone() path already does this. */
    child->tls_base  = parent->tls_base;
    child->ctty      = parent->ctty;
    if (child->ctty)
        vfs_retain(child->ctty);

    /* CLONE_PARENT_SETTID / CLONE_CHILD_SETTID / CLONE_CHILD_CLEARTID apply to
     * fork-style clones too (kernel/fork.c copy_process: parent_tidptr is
     * written by the parent, set_child_tid by the child in schedule_tail,
     * clear_child_tid at exit).  glibc's fork passes &THREAD_SELF->tid as
     * ctid so the child's descriptor holds ITS pid, not the parent's. */
    if ((clone_flags & CLONE_PARENT_SETTID) && uptid &&
        access_ok((void *)(uintptr_t)uptid, 4))
        *(uint32_t *)(uintptr_t)uptid = (uint32_t)child->pid;
    child->set_child_tid   = (clone_flags & CLONE_CHILD_SETTID)   ? uctid : 0;
    child->clear_child_tid = (clone_flags & CLONE_CHILD_CLEARTID) ? uctid : 0;

    vma_clone(parent, child);     /* fork: child gets its own copy of the VMAs */

    /* Create child's page directory with kernel mappings */
    child->pgdir_phys = pgdir_create();
    if (!child->pgdir_phys) {
        kfree(child->kstack);
        child->state = PROC_UNUSED;
        return -11;
    }

    /*
     * Clone user address space with copy-on-write.
     *
     * parent_pd is the parent's page directory via the recursive mapping
     * (parent's CR3 is still loaded — we haven't switched away).
     * child_pd is accessed via TEMP_MAP_VIRT2.
     * Each parent page table is accessed via PAGE_TABLES_BASE + i*4096.
     * Each child page table is allocated and accessed via TEMP_MAP_VIRT.
     */
    uint32_t *parent_pd = (uint32_t *)PAGE_DIR_VIRT;
    uint32_t *child_pd  = (uint32_t *)paging_temp_map2(child->pgdir_phys);

    for (int pde_idx = 0; pde_idx < 768; pde_idx++) {
        if (!(parent_pd[pde_idx] & PAGE_PRESENT)) continue;

        /* Allocate a new page table for the child */
        uint32_t child_pt_phys = pmm_alloc_frame();
        if (!child_pt_phys) {
            paging_temp_unmap2();
            pgdir_free_user(child->pgdir_phys);
            kfree(child->kstack);
            child->state = PROC_UNUSED;
            return -11;
        }

        /* Access the child's PT via TEMP_MAP_VIRT, zero it, then populate */
        uint32_t *child_pt = (uint32_t *)paging_temp_map(child_pt_phys);
        for (int k = 0; k < 1024; k++)
            child_pt[k] = 0;

        /* Access parent's PT via the recursive mapping */
        uint32_t *parent_pt =
            (uint32_t *)(PAGE_TABLES_BASE + (uint32_t)pde_idx * PAGE_SIZE);

        for (int pte_idx = 0; pte_idx < 1024; pte_idx++) {
            uint32_t pte = parent_pt[pte_idx];
            /* PAGE_PROTNONE entries own a frame too (mprotect(PROT_NONE)). */
            if (!(pte & (PAGE_PRESENT | PAGE_PROTNONE))) {
                child_pt[pte_idx] = 0;
                continue;
            }

            uint32_t frame_phys = pte & ~0xFFFU;

            /* Every private page becomes COW, writable or not: a read-only
             * private page may be made writable later by mprotect(), and the
             * COW bit is what keeps that write from leaking into the sharer
             * (Linux copy_present_pte marks the whole private range). */
            if (!(pte & PAGE_SHARED)) {
                /* Make COW: strip write bit, add COW flag in both.  NB: do NOT
                 * per-page invlpg here — for a large address space (Firefox's
                 * ~150 MB) that is tens of thousands of invlpgs, making fork slow
                 * enough that the -smp2 launch-completion race is lost (the main
                 * thread reaches WaitForProcessHandle before the launcher forks).
                 * A SINGLE local CR3 reload after the loop flushes the whole local
                 * TLB in O(1); the final tlb_shootdown handles the other CPUs. */
                uint32_t cow_pte = (pte & ~(uint32_t)PAGE_WRITABLE) | PAGE_COW;
                parent_pt[pte_idx] = cow_pte;
                child_pt[pte_idx]  = cow_pte;
            } else {
                /* Read-only or shared-memory page: share directly.  Shared
                 * pages must stay writable in both — COW would silently
                 * un-share them. */
                child_pt[pte_idx] = pte;
            }
            pmm_frame_incref(frame_phys);
        }

        paging_temp_unmap();   /* release child_pt (TEMP_MAP_VIRT) */

        /* Install child's PT in child's PD (still mapped at TEMP_MAP_VIRT2) */
        child_pd[pde_idx] = child_pt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    paging_temp_unmap2();  /* release child_pd */

    /* Flush THIS CPU's TLB in one shot (reload CR3 with the parent's own pgdir,
     * still loaded) — drops every now-stale writable entry for the pages just
     * marked COW.  Replaces the per-page invlpg that used to run inside the loop
     * (O(1) vs O(mapped pages)); the parent's user pages aren't touched during
     * the kernel-mode fork walk, so deferring the flush to here is safe. */
    {
        uint32_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }

    /* SMP: fork just marked every writable parent page read-only (COW) in the
     * SHARED pgdir.  A sibling thread of the parent running on another CPU still
     * has those pages cached WRITABLE in its TLB — without a shootdown it would
     * keep writing through the stale entry, bypassing COW and corrupting both
     * the parent's and the child's copy.  Force the flush now.  (Firefox forks
     * subprocesses from its ~20-thread parent constantly, so this is the
     * dominant corruption source under -smp 2.) */
    tlb_shootdown();

    /* Copy open file descriptors; bump pipe refcounts */
    for (int i = 0; i < MAX_FD; i++) {
        child->ofile[i] = parent->ofile[i];
        fd_retain(&parent->ofile[i]);
    }

    /* Inherit shared-memory mapping records (PTEs already cloned above) */
    shm_proc_fork(parent, child);

    /* clone() with an explicit child stack: run the child there (see do_fork
     * banner).  Set BEFORE marking RUNNABLE — under SMP the other CPU may
     * dispatch the child the instant it becomes runnable. */
    if (child_stack)
        child->tf->useresp = child_stack;

    child->state = PROC_RUNNABLE;

    printk("[SYSCALL] %s: parent pid=%d child pid=%d%s\n",
           child_stack ? "clone(fork)" : "sys_fork",
           parent->pid, child->pid, child_stack ? " (child stack)" : "");
    return child->pid;  /* parent gets child's PID */
}

/* ── sys_fork() — EAX=2 ────────────────────────────────────────────────── */
static int sys_fork(registers_t *regs) {
    return do_fork(regs, 0, 0, 0, 0);
}

/* ── sys_read(int fd, void *buf, size_t count) — EAX=3 ───────────────────── */
static int sys_read(registers_t *regs) {
    int      fd  = (int)regs->ebx;
    char    *buf = (char *)(uintptr_t)regs->ecx;
    int      len = (int)(uint32_t)regs->edx;

    if (len < 0 || !access_ok(buf, (size_t)len))
        return -14;  /* -EFAULT */
    if (fd < 0 || fd >= MAX_FD)
        return -9;   /* -EBADF */

    proc_file_t *f = &current_proc->ofile[fd];

    /* Pipe read */
    if (f->type == FD_PIPE_R)
        return pipe_read(f->pipe, buf, len, (f->flags & 0x800) != 0);

    /* eventfd read (8-byte counter) */
    if (f->type == FD_EVENTFD)
        return eventfd_read(f->efd, buf, len, (f->flags & O_NONBLOCK) != 0);

    /* VFS file read */
    if (f->type == FD_FILE) {
        int n = (int)vfs_read(f->node, f->offset, (uint32_t)len, (uint8_t *)buf);
        f->offset += (uint32_t)n;
        return n;
    }

    /* Fallback: serial stdin for fd=0 when no fd entry */
    if (fd == 0 && f->type == FD_NONE) {
        int n = 0;
        while (n < len) {
            char c = serial_getc();
            if (c == '\r') c = '\n';
            if (c == 3) {               /* Ctrl-C → SIGINT */
                signal_send(current_proc, SIGINT);
                return -4;              /* -EINTR */
            }
            serial_putc(c);             /* echo */
            vga_putchar(c);
            if (c == '\b' || c == 127) {
                if (n > 0) { n--; }
                continue;
            }
            buf[n++] = c;
            if (c == '\n') break;
        }
        return n;
    }

    if (f->type == FD_SOCKET)
        return net_socket_recvfrom(f->socket, buf, (uint32_t)len, NULL);

    if (f->type == FD_USOCKET)
        return usocket_read(f->usock, buf, len, (f->flags & O_NONBLOCK) != 0);

    return -9;  /* -EBADF */
}

/* ── sys_write(int fd, const void *buf, size_t len) — EAX=4 ──────────── */
static int sys_write(registers_t *regs) {
    int         fd  = (int)regs->ebx;
    const char *buf = (const char *)(uintptr_t)regs->ecx;
    int         len = (int)(uint32_t)regs->edx;

    if (len < 0 || !access_ok(buf, (size_t)len)) {
        printk("[SYSCALL] sys_write: bad user ptr 0x%08x\n", (unsigned)regs->ecx);
        return -14;   /* -EFAULT */
    }
    if (fd < 0 || fd >= MAX_FD) return -9;

    proc_file_t *f = &current_proc->ofile[fd];

    /* Pipe write */
    if (f->type == FD_PIPE_W)
        return pipe_write(f->pipe, buf, len, (f->flags & 0x800) != 0);

    /* eventfd write (8-byte counter add) */
    if (f->type == FD_EVENTFD)
        return eventfd_write(f->efd, buf, len, (f->flags & O_NONBLOCK) != 0);

    /* VFS file write (includes /dev/tty) */
    if (f->type == FD_FILE && f->node) {
        if (f->flags == O_RDONLY) return -9;   /* opened read-only */
        if (!f->node->write_fn)   return -9;   /* node not writable */
        uint32_t written = vfs_write(f->node, f->offset,
                                     (uint32_t)len, (const uint8_t *)buf);
        f->offset += written;
        return (int)written;
    }

    /* Fallback: fd=1/2 with no entry → serial+VGA stdout */
    if (f->type == FD_NONE && (fd == 1 || fd == 2)) {
        for (int i = 0; i < len; i++) {
            serial_putc(buf[i]);
            vga_putchar(buf[i]);
        }
        return len;
    }

    if (f->type == FD_SOCKET)
        return net_socket_sendto(f->socket, buf, (uint32_t)len, NULL);

    if (f->type == FD_USOCKET)
        return usocket_write(f->usock, buf, len, (f->flags & O_NONBLOCK) != 0);

    return -9;  /* -EBADF */
}

/* ── sys_waitpid(pid_t pid, int *status, int options) — EAX=7 ─────────── */
#define WNOHANG    1
#define WUNTRACED  2

/* Linux kernel/exit.c do_wait()/wait_consider_task(): the wait set is the
 * children of the calling PROCESS (real_parent == our group leader), so any
 * thread of a multithreaded parent can reap a child forked by another thread.
 * Only thread-group LEADERS are wait targets: CLONE_THREAD siblings are
 * released as soon as they exit and never reported.  A leader that is a zombie
 * is reported only once its whole group is gone (delay_group_leader), so the
 * process is collected exactly once and with the group's exit status.  Waiting
 * is interruptible: a deliverable signal returns -EINTR (restarted under
 * SA_RESTART), which is what lets a SIGKILL end a parent parked here. */
static int sys_waitpid(registers_t *regs) {
    int     req_pid    = (int)regs->ebx;
    int    *status_ptr = (int *)(uintptr_t)regs->ecx;
    int     options    = (int)regs->edx;

    if (status_ptr && !access_ok(status_ptr, sizeof(int)))
        return -14;  /* -EFAULT */

    struct proc *me = proc_group_leader(current_proc);
    int my_tgid = current_proc->tgid;

    for (;;) {
        int found_child = 0;

        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            if (p->state == PROC_UNUSED) continue;
            if (!p->parent || p->parent->tgid != my_tgid) continue;
            if (p->pid != p->tgid) continue;               /* threads: never */
            if (req_pid > 0 && p->pid != req_pid) continue;
            if (req_pid == 0 && p->pgrp != current_proc->pgrp) continue;
            if (req_pid < -1 && p->pgrp != -req_pid) continue;
            found_child = 1;

            if (p->state == PROC_ZOMBIE) {
                if (!proc_group_empty(p)) continue;        /* siblings still exiting */
                int child_pid = p->pid;
                if (status_ptr) {
                    int status = p->exit_status;
                    int cr = copy_to_user(status_ptr, &status, sizeof(status));
                    if (cr < 0) return cr;
                }
                printk("[SYSCALL] sys_waitpid: collected child pid=%d status=0x%x\n",
                       child_pid, p->exit_status);
                proc_release(p);
                return child_pid;
            }

            /* WUNTRACED: also report stopped children */
            if ((options & WUNTRACED) && p->state == PROC_STOPPED) {
                if (status_ptr) {
                    int status = (SIGTSTP << 8) | 0x7f;
                    int cr = copy_to_user(status_ptr, &status, sizeof(status));
                    if (cr < 0) return cr;
                }
                return p->pid;
            }
        }

        if (!found_child)
            return -10;  /* -ECHILD */

        if (options & WNOHANG)
            return 0;

        if (signal_interrupt_pending(current_proc))
            return -4;   /* -EINTR (Linux -ERESTARTSYS) */

        /* No zombie yet — sleep until a child exits.  The channel is our group
         * leader: proc_exit wakes the parent PROCESS, whichever thread waits. */
        sleep_on(me);
    }
}

/* Auto-reap orphan zombies (parent == init/PID 1).  init reaps all orphans
 * anyway, but it can be blocked in a per-child waitpid() while a burst of
 * orphans (e.g. a watchdog SIGKILL of Firefox's ~20-process tree) becomes
 * zombies — they would pile up and exhaust the process table.  Called from
 * proc_exit on every exit so dead orphans are freed promptly.  Reaping a zombie
 * that isn't running is safe (free its kstack + pgdir, mark the slot UNUSED). */
void reap_orphan_zombies(void) {
    struct proc *init = (void *)0;
    for (int i = 0; i < MAX_PROCS; i++)
        if (ptable[i].pid == 1 && ptable[i].state != PROC_UNUSED) { init = &ptable[i]; break; }
    if (!init) return;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p == current_proc) continue;     /* never reap the caller mid-exit:
                                              * its own kstack is still in use */
        if (p->state != PROC_ZOMBIE || p->parent != init) continue;
        if (p->pid != p->tgid || !proc_group_empty(p)) continue;
        proc_release(p);
    }
}

static int sys_open_kernel_path(const char *path, int flags) {
    vfs_node_t *node = vfs_open_at(path);
    if (!node) {
        /* O_CREAT: create the file if missing */
        if (!(flags & O_CREAT))
            return -2;   /* -ENOENT */

        char dir_path[256], base[256];
        if (path_split(path, dir_path, base) < 0)
            return -2;

        vfs_node_t *dir = vfs_open_parent_at(path, dir_path);
        if (!dir || !dir->create_fn)
            return -2;

        /* Need write+search on the parent directory to create here. */
        if (vfs_access_check(dir, current_proc->euid, current_proc->egid,
                             VFS_WANT_W | VFS_WANT_X) < 0)
            return -13;

        if (dir->create_fn(dir, base, VFS_FLAG_FILE) < 0)
            return -12;   /* -ENOMEM */

        node = vfs_open_at(path);
        if (!node) return -2;
        /* Stamp the creator as owner with 0666 & ~umask. */
        vfs_setattr(node, (0666 & ~current_proc->umask) & 07777,
                    current_proc->uid, current_proc->gid);
    } else {
        /* Existing node: check requested access mode. */
        int rw = flags & 3;
        int want = (rw == O_WRONLY) ? VFS_WANT_W
                 : (rw == O_RDWR)   ? (VFS_WANT_R | VFS_WANT_W)
                 : VFS_WANT_R;
        if (flags & O_TRUNC) want |= VFS_WANT_W;
        if (vfs_access_check(node, current_proc->euid, current_proc->egid,
                             want) < 0)
            return -13;   /* -EACCES */
    }

    /* Named FIFO: open as a pipe endpoint */
    if (node->flags == VFS_FLAG_FIFO) {
        /* Create shared pipe_buf_t on first open; reuse on subsequent opens */
        if (!node->private) {
            pipe_buf_t *pb = pipe_alloc();
            if (!pb) return -12;
            pb->nreaders = 0;
            pb->nwriters = 0;
            node->private = pb;
        }
        pipe_buf_t *pb = (pipe_buf_t *)node->private;
        /* Determine read vs write end from flags */
        int rw = flags & 3;  /* O_RDONLY=0, O_WRONLY=1, O_RDWR=2 */
        for (int i = 0; i < MAX_FD; i++) {
            if (current_proc->ofile[i].type == FD_NONE) {
                if (rw == O_WRONLY) {
                    current_proc->ofile[i].type = FD_PIPE_W;
                    pb->nwriters++;
                } else {
                    current_proc->ofile[i].type = FD_PIPE_R;
                    pb->nreaders++;
                }
                current_proc->ofile[i].pipe    = pb;
                current_proc->ofile[i].node    = node;
                current_proc->ofile[i].flags   = flags;  /* incl. O_NONBLOCK */
                current_proc->ofile[i].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                __builtin_memcpy(current_proc->ofile[i].path, path,
                                 __builtin_strlen(path) + 1);
                return i;
            }
        }
        return -24;
    }

    /* O_TRUNC: discard existing content */
    if ((flags & O_TRUNC) && node->truncate_fn)
        node->truncate_fn(node, 0);

    /* Find a free file descriptor slot */
    for (int i = 0; i < MAX_FD; i++) {
        if (current_proc->ofile[i].type == FD_NONE) {
            current_proc->ofile[i].type    = FD_FILE;
            current_proc->ofile[i].node    = node;
            current_proc->ofile[i].offset  = (flags & O_APPEND) ? node->size : 0;
            current_proc->ofile[i].flags   = flags & 3;  /* O_RDONLY/O_WRONLY/O_RDWR */
            current_proc->ofile[i].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
            __builtin_memcpy(current_proc->ofile[i].path, path,
                             __builtin_strlen(path) + 1);
            return i;
        }
    }
    return -24;  /* -EMFILE: too many open files */
}

/* ── sys_open(const char *path, int flags, int mode) — EAX=5 ─────────────── */
/* DEBUG helper: does haystack contain needle? (for the iconopen trace) */
static int dbg_str_has(const char *s, const char *sub) {
    for (; *s; s++) {
        const char *a = s, *b = sub;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static int sys_open(registers_t *regs) {
    const char *upath = (const char *)(uintptr_t)regs->ebx;
    int         flags = (int)regs->ecx;

    if (!access_ok(upath, 1))
        return -14;  /* -EFAULT */

    char path[256];
    int r = copy_user_str(upath, path, 256);
    if (r < 0) return r;

    char resolved[256];
    r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    int rc = sys_open_kernel_path(resolved, flags);
    fx_lockdbg("open", path, rc);
    {   /* DEBUG: trace image/icon/theme opens to pin the GTK NULL-pixbuf source */
        static int ic = 0;
        if (ic < 80 && (dbg_str_has(path, ".png") || dbg_str_has(path, ".svg") ||
                        dbg_str_has(path, "icon") || dbg_str_has(path, "theme") ||
                        dbg_str_has(path, "hicolor") || dbg_str_has(path, "pixmap"))) {
            ic++;
            printk("[iconopen] %s -> %d\n", path, rc);
        }
    }
    return rc;
}

/* ── sys_close(int fd) — EAX=6 ───────────────────────────────────────────── */
static int sys_close(registers_t *regs) {
    int fd = (int)regs->ebx;

    if (fd < 0 || fd >= MAX_FD) return -9;

    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type == FD_NONE) return -9;  /* -EBADF */

    fd_release(f);
    return 0;
}

/* ── sys_getpid() — EAX=20 ───────────────────────────────────────────────── */
static int sys_getpid(registers_t *regs) {
    (void)regs;
    /* Linux ABI: getpid() is the thread-GROUP id (gettid() is per-thread). */
    return current_proc ? current_proc->tgid : 0;
}

/* ── sys_brk(void *addr) — EAX=45 ───────────────────────────────────────── */
static int sys_brk(registers_t *regs) {
    uint32_t new_brk = (uint32_t)regs->ebx;

    /* Query: return current break */
    if (new_brk == 0)
        return (int)current_proc->heap_end;

    /* Clamp: must be in user address space below the stack guard. */
    if (new_brk >= USER_STACK_BASE)
        return -12;  /* -ENOMEM */

    uint32_t old_brk = current_proc->heap_end;

    if (new_brk > old_brk) {
        /* Grow heap: map pages from old_brk up to new_brk */
        uint32_t va = old_brk & ~(PAGE_SIZE - 1);
        uint32_t end = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        /* Linux do_brk_flags: the break may not run into a mapping. */
        {
            uint32_t chk = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            if (end > chk && !vma_range_free(chk, end - chk)) return -12;
        }
        for (; va < end; va += PAGE_SIZE) {
            /* Skip pages already mapped (old_brk might not be page-aligned) */
            if (va < old_brk && (*paging_get_pde(va) & PAGE_PRESENT) &&
                (*paging_get_pte(va) & PAGE_PRESENT))
                continue;

            uint32_t phys = pmm_alloc_frame();
            if (!phys) return -12;  /* -ENOMEM */
            pmm_frame_incref(phys);
            paging_map(va, phys,
                       PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
            /* Zero the new page */
            __builtin_memset((void *)va, 0, PAGE_SIZE);
        }
    } else if (new_brk < old_brk) {
        /* Shrink heap: unmap and free pages from new_brk up to old_brk */
        uint32_t va  = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint32_t end = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (; va < end; va += PAGE_SIZE) {
            if (!(*paging_get_pde(va) & PAGE_PRESENT)) continue;
            uint32_t *pte = paging_get_pte(va);
            if (!(*pte & PAGE_PRESENT)) continue;
            uint32_t phys = *pte & ~0xFFFU;
            paging_unmap(va);
            pmm_frame_decref(phys);
        }
    }

    current_proc->heap_end = new_brk;
    return (int)new_brk;
}

/* ── sys_exec(const char *path, char **argv, char **envp) — EAX=11 ──────── */
/* ── execve() argument collection ─────────────────────────────────────────────
 * Linux copies argv/envp out of the OLD address space before the point of no
 * return (fs/exec.c copy_strings) and bounds them two ways: MAX_ARG_STRLEN
 * (32 pages) per string, and bprm_stack_limits() — a quarter of RLIMIT_STACK
 * — for the whole block including the pointer arrays.  Past either it returns
 * -E2BIG.  MaeroOS advertises an 8 MiB main-thread stack (paging.c grows it on
 * fault), so the same quarter rule gives a 2 MiB ARG_MAX.
 *
 * The strings land in one growable kernel buffer and the vector holds byte
 * OFFSETS into it, so growing the buffer never invalidates the vector.
 *
 * The ARG_MAX bound is charged INCREMENTALLY, before each chunk is reserved or
 * copied, exactly as Linux checks bprm_stack_limits() inside copy_strings().
 * Checking only after both vectors are copied is not a bound at all: 4096 argv
 * pointers aimed at one 128 KiB user string cost the attacker 128 KiB and would
 * drive this buffer to ~540 MiB, and kmalloc does not fail gracefully — a heap
 * that outgrows HEAP_MAX halts the machine (mm/heap.c heap_expand).  An
 * unprivileged execve must never be able to reach that. */
#define EXEC_MAX_ARG_STRLEN  (32U * PAGE_SIZE)          /* Linux MAX_ARG_STRLEN */
#define EXEC_STACK_LIMIT     (8U * 1024U * 1024U)       /* our RLIMIT_STACK */
#define EXEC_ARG_MAX         (EXEC_STACK_LIMIT / 4U)    /* Linux bprm_stack_limits */
#define EXEC_MAX_ARG_STRINGS 4096                       /* Linux MAX_ARG_STRINGS */

struct exec_strings {
    char     *buf;   uint32_t used, cap;    /* NUL-separated string bytes */
    uint32_t *off;   uint32_t n,    ncap;   /* offset of each string in buf */
    uint32_t *total;  /* running argv+envp charge, SHARED by the two vectors */
};

static void es_init(struct exec_strings *v, uint32_t *total) {
    v->buf = NULL; v->used = v->cap = 0;
    v->off = NULL; v->n = v->ncap = 0;
    v->total = total;
}

static void es_free(struct exec_strings *v) {
    uint32_t *total = v->total;
    if (v->buf) kfree(v->buf);
    if (v->off) kfree(v->off);
    es_init(v, total);
}

/* Charge `bytes` against the shared argv+envp budget before anything is
 * allocated for them.  -E2BIG once the block would exceed ARG_MAX. */
static int es_charge(struct exec_strings *v, uint32_t bytes) {
    if (bytes > EXEC_ARG_MAX - *v->total) return -7;    /* -E2BIG */
    *v->total += bytes;
    return 0;
}

/* Make room for `need` more string bytes.  Doubling, so a long argv is linear;
 * clamped to the budget so the buffer itself can never exceed ARG_MAX. */
static int es_reserve(struct exec_strings *v, uint32_t need) {
    if (v->used + need <= v->cap) return 0;
    uint32_t cap = v->cap ? v->cap : 512;
    while (cap < v->used + need) cap *= 2;
    if (cap > EXEC_ARG_MAX) cap = v->used + need;
    char *nb = (char *)kmalloc(cap);
    if (!nb) return -12;
    if (v->buf) { __builtin_memcpy(nb, v->buf, v->used); kfree(v->buf); }
    v->buf = nb; v->cap = cap;
    return 0;
}

/* Record `off` as the start of the next string, charging its slot in the
 * pointer array the block will need on the new stack. */
static int es_index(struct exec_strings *v, uint32_t off) {
    if (v->n >= EXEC_MAX_ARG_STRINGS) return -7;        /* -E2BIG */
    if (es_charge(v, 4) < 0) return -7;                 /* -E2BIG */
    if (v->n == v->ncap) {
        uint32_t ncap = v->ncap ? v->ncap * 2 : 16;
        uint32_t *no = (uint32_t *)kmalloc(ncap * sizeof(uint32_t));
        if (!no) return -12;
        if (v->off) {
            __builtin_memcpy(no, v->off, v->n * sizeof(uint32_t));
            kfree(v->off);
        }
        v->off = no; v->ncap = ncap;
    }
    v->off[v->n++] = off;
    return 0;
}

/* Append a kernel string (len excludes the NUL). */
static int es_push(struct exec_strings *v, const char *str, uint32_t len) {
    if (es_charge(v, len + 1) < 0) return -7;           /* -E2BIG */
    if (es_reserve(v, len + 1) < 0) return -12;
    uint32_t start = v->used;
    __builtin_memcpy(v->buf + start, str, len);
    v->buf[start + len] = '\0';
    v->used = start + len + 1;
    int rc = es_index(v, start);
    if (rc < 0) v->used = start;
    return rc;
}

/* Append a NUL-terminated USER string.  Copied in page-bounded chunks (a
 * string may run right up to the end of a mapped page but never past it), not
 * byte by byte — the strings Linux allows are up to 128 KiB. */
static int es_push_user(struct exec_strings *v, const char *up) {
    uint32_t start = v->used, got = 0;
    for (;;) {
        uint32_t chunk = PAGE_SIZE - (((uint32_t)(uintptr_t)up + got) & (PAGE_SIZE - 1));
        /* Charge FIRST: the budget has to stop us before the allocation, not
         * after the whole vector has been copied. */
        if (es_charge(v, chunk) < 0) { v->used = start; return -7; }    /* -E2BIG */
        v->used = start + got;
        if (es_reserve(v, chunk) < 0) { v->used = start; return -12; }
        if (copy_from_user(v->buf + start + got, up + got, chunk) < 0) {
            v->used = start;
            return -14;
        }
        for (uint32_t i = 0; i < chunk; i++)
            if (v->buf[start + got + i] == '\0') {
                *v->total -= chunk - (i + 1);   /* refund the unused tail */
                v->used = start + got + i + 1;
                int rc = es_index(v, start);
                if (rc < 0) v->used = start;
                return rc;
            }
        got += chunk;
        if (got > EXEC_MAX_ARG_STRLEN) { v->used = start; return -7; }  /* -E2BIG */
    }
}

/* Copy a whole NULL-terminated user vector (argv or envp). */
static int es_push_user_vec(struct exec_strings *v, char **uvec) {
    if (!uvec || !access_ok(uvec, sizeof(char *))) return 0;
    for (uint32_t i = 0; ; i++) {
        char *up = NULL;
        if (copy_from_user(&up, &uvec[i], sizeof(up)) < 0) return -14;
        if (!up) return 0;
        int rc = es_push_user(v, up);
        if (rc < 0) return rc;
    }
}

/* Linux de_thread() (fs/exec.c): a thread that execve()s first kills every
 * other thread of its group and waits for them to be gone; if the caller is
 * not the group leader it then takes over the leader's identity (Linux
 * exchange_tids() + release_task(leader)) so the new image runs single-
 * threaded under the PROCESS's pid.  Called only past the point of no return.
 * Returns with current_proc as the sole, leading thread of its group. */
static void de_thread(void) {
    struct proc *me   = current_proc;
    int          tgid = me->tgid;
    int          others = 0;

    /* zap_other_threads(): SIGKILL every sibling. */
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q == me || q->state == PROC_UNUSED || q->tgid != tgid) continue;
        others = 1;
        if (q->state != PROC_ZOMBIE) signal_send(q, SIGKILL);
    }
    if (!others) return;                       /* already the whole process */

    /* Wait for them to die.  A killed sibling becomes a zombie and the
     * scheduler releases it; the group LEADER's zombie stays for us to take
     * over below, so it does not count as alive here. */
    for (int guard = 0; guard < 200000; guard++) {
        int alive = 0;
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *q = &ptable[i];
            if (q == me || q->state == PROC_UNUSED || q->tgid != tgid) continue;
            if (q->state == PROC_ZOMBIE && q->pid == tgid) continue;   /* leader */
            alive = 1;
            break;
        }
        if (!alive) break;
        yield();
    }

    /* A sibling dying from our SIGKILL runs the fatal-signal path, which ends
     * the whole THREAD GROUP (proc_group_exit) — us included: it stamps a
     * "killed by signal 9" status on the leader and queues SIGKILL on every
     * other member.  Linux suppresses exactly that while an execve is taking
     * the group over (signal_struct.group_exec_task).  Every sibling is gone
     * by this point, so undo the collateral damage here: the process is not
     * dying, it is being taken over. */
    me->pending_sigs &= ~(1u << SIGKILL);
    me->group_exit    = 0;
    me->exit_status   = 0;

    if (me->pid == tgid) return;               /* the caller IS the leader */

    /* Take over the leader's pid: the process keeps the identity its parent
     * knows and waits for, and the old leader's slot is freed. */
    struct proc *leader = NULL;
    for (int i = 0; i < MAX_PROCS; i++)
        if (&ptable[i] != me && ptable[i].state != PROC_UNUSED &&
            ptable[i].pid == tgid) { leader = &ptable[i]; break; }
    if (leader) {
        me->parent     = leader->parent;
        me->pgrp       = leader->pgrp;
        me->sid        = leader->sid;
        leader->group_exit = 0;
        /* The process's children belong to the surviving thread now. */
        for (int i = 0; i < MAX_PROCS; i++)
            if (ptable[i].state != PROC_UNUSED && ptable[i].parent == leader)
                ptable[i].parent = me;
        if (leader->state == PROC_ZOMBIE)
            proc_release(leader);
        else
            leader->tgid = leader->pid;        /* refused to die: detach it */
    }
    me->pid = tgid;
}

static int sys_exec(registers_t *regs) {
    const char *upath = (const char *)(uintptr_t)regs->ebx;

    if (!access_ok(upath, 1))
        return -14;  /* -EFAULT */

    /* Copy path into kernel buffer before we swap page directories */
    char path[256];
    int r = copy_user_str(upath, path, 256);
    if (r < 0) return r;

    /* Copy argv + envp strings from user space into kernel buffers (before CR3 swap) */
/* Load bases for position-independent objects: PIEs go low, the dynamic
 * linker (ld-musl) goes at the bottom of the mmap region (mmap_next is then
 * bumped above it so anonymous maps never clobber the interpreter image). */
#define EXEC_PIE_BASE    0x10000000U
#define EXEC_INTERP_BASE 0x40000000U
    struct exec_strings av, ev;
    /* One budget for both vectors, pre-charged with the two NULL terminators
     * their pointer arrays need.  es_charge() spends it as the strings are
     * copied, so an oversized argv is refused before the memory is taken. */
    uint32_t arg_budget = 8;
    /* Function scope, not block scope: the shebang rebuild below hands its
     * vector to `av`, which keeps pointing at this counter afterwards. */
    uint32_t shebang_budget = 8;
    es_init(&av, &arg_budget);
    es_init(&ev, &arg_budget);
    r = es_push_user_vec(&av, (char **)(uintptr_t)regs->ecx);
    if (r == 0) r = es_push_user_vec(&ev, (char **)(uintptr_t)regs->edx);
    if (r < 0) { es_free(&av); es_free(&ev); return r; }
    int argc = (int)av.n;
    int envc = (int)ev.n;
#define EXEC_FAIL(err) do { es_free(&av); es_free(&ev); return (err); } while (0)
#define KARGV(i) (av.buf + av.off[i])
#define KENVP(i) (ev.buf + ev.off[i])

    vfs_node_t *node = vfs_open_at(path);
    if (!node) {
        printk("[execfail] '%s' pid=%d ENOENT (open failed)\n", path, current_proc->pid);
        EXEC_FAIL(-2);   /* -ENOENT */
    }

    /* Need execute permission on the binary. */
    if (vfs_access_check(node, current_proc->euid, current_proc->egid,
                         VFS_WANT_X) < 0) {
        printk("[execfail] '%s' pid=%d EACCES (uid=%d gid=%d mode=%o)\n",
               path, current_proc->pid, (int)current_proc->euid,
               (int)current_proc->egid, (unsigned)node->mask);
        EXEC_FAIL(-13);   /* -EACCES */
    }

    /* set-user-ID bit: run with the file owner's effective uid (e.g. doas,
     * passwd are owned by root mode 04755).  Real uid is unchanged. */
    uint32_t new_euid = current_proc->euid;
    uint32_t new_egid = current_proc->egid;
    if (node->mask & 04000) new_euid = node->uid;
    if (node->mask & 02000) new_egid = node->gid;

    /* Shebang (#!) interpreter detection */
    char shebang_line[512];
    uint32_t shebang_read = vfs_read(node, 0, sizeof(shebang_line) - 1, (uint8_t *)shebang_line);
    shebang_line[shebang_read] = '\0';
    if (shebang_read >= 2 && shebang_line[0] == '#' && shebang_line[1] == '!') {
        const char *lp = shebang_line + 2;
        /* skip leading spaces */
        while (*lp == ' ') lp++;
        /* extract interpreter path */
        char interp[256];
        int ii = 0;
        while (*lp && *lp != '\n' && *lp != ' ' && *lp != '\r' && ii < 255)
            interp[ii++] = *lp++;
        interp[ii] = '\0';
        /* extract optional interpreter argument */
        while (*lp == ' ') lp++;
        char interp_arg[256];
        int ai = 0;
        while (*lp && *lp != '\n' && *lp != '\r' && ai < 255)
            interp_arg[ai++] = *lp++;
        interp_arg[ai] = '\0';

        if (ii > 0) {
            /* Rebuild argv: [interp, interp_arg?, script_path, orig_argv[1..]] */
            struct exec_strings nv;
            es_init(&nv, &shebang_budget);
            int rc = es_push(&nv, interp, (uint32_t)ii);
            if (rc == 0 && ai > 0) rc = es_push(&nv, interp_arg, (uint32_t)ai);
            if (rc == 0) rc = es_push(&nv, path, (uint32_t)__builtin_strlen(path));
            for (int i = 1; rc == 0 && i < argc; i++)
                rc = es_push(&nv, KARGV(i), (uint32_t)__builtin_strlen(KARGV(i)));
            if (rc < 0) { es_free(&nv); EXEC_FAIL(rc); }
            es_free(&av);
            av   = nv;
            argc = (int)av.n;

            /* Re-resolve to interpreter */
            __builtin_memcpy(path, interp, (size_t)ii + 1);
            node = vfs_open_at(path);
            if (!node) EXEC_FAIL(-2);
        }
    }

    /* Create new address space */
    uint32_t new_pgdir = pgdir_create();
    if (!new_pgdir) EXEC_FAIL(-12);

    /*
     * Load the main object.  A static ET_EXEC ignores the bias (fixed VAs);
     * a PIE (ET_DYN) loads at EXEC_PIE_BASE.  If it names a PT_INTERP, we then
     * load that dynamic linker (ld-musl) at EXEC_INTERP_BASE and hand control
     * to *it* — it relocates the program and resolves symbols in user space.
     */
    elf_info_t einfo;
    if (elf_load_bias(node, new_pgdir, EXEC_PIE_BASE, &einfo) < 0) {
        pgdir_free_user(new_pgdir);
        EXEC_FAIL(-8);  /* -ENOEXEC */
    }
    uint32_t prog_entry = einfo.entry;   /* main program entry (AT_ENTRY) */
    uint32_t entry      = einfo.entry;   /* address we actually iret to */
    uint32_t heap_end   = einfo.heap_end;
    uint32_t interp_base = 0;            /* AT_BASE (0 = static, no ld.so) */
    uint32_t mmap_floor  = 0x40000000U;  /* where mmap() begins */

    if (einfo.has_interp) {
        /* Resolve the dynamic linker.  The ELF names "/lib/ld-musl-i386.so.1",
         * but the initrd is flat (no nested dirs) — fall back to the basename
         * at root and to /disk/lib so the same binary works from either FS. */
        vfs_node_t *lnode = vfs_open_at(einfo.interp);
        if (!lnode) {
            const char *bn = einfo.interp;
            for (const char *p = einfo.interp; *p; p++)
                if (*p == '/') bn = p + 1;
            char alt[96];
            int an = 0;
            alt[an++] = '/';
            for (int i = 0; bn[i] && an < 94; i++) alt[an++] = bn[i];
            alt[an] = '\0';
            lnode = vfs_open_at(alt);             /* "/ld-musl-i386.so.1" */
            if (!lnode) {
                char dpath[112];
                int dn = 0;
                const char *pre = "/disk/lib/";
                for (int i = 0; pre[i]; i++) dpath[dn++] = pre[i];
                for (int i = 0; bn[i] && dn < 110; i++) dpath[dn++] = bn[i];
                dpath[dn] = '\0';
                lnode = vfs_open_at(dpath);       /* "/disk/lib/ld-musl..." */
            }
        }
        if (!lnode) {
            printk("[ELF] interpreter '%s' not found\n", einfo.interp);
            pgdir_free_user(new_pgdir);
            EXEC_FAIL(-2);  /* -ENOENT */
        }
        elf_info_t linfo;
        if (elf_load_bias(lnode, new_pgdir, EXEC_INTERP_BASE, &linfo) < 0) {
            pgdir_free_user(new_pgdir);
            EXEC_FAIL(-8);
        }
        interp_base = linfo.load_bias;
        entry       = linfo.entry;          /* jump to the dynamic linker */
        /* Keep mmap() clear of the interpreter image. */
        if (linfo.heap_end > mmap_floor) mmap_floor = linfo.heap_end;
    }

    /*
     * Build the initial-stack image in a kernel buffer first, then copy it
     * into the new stack's frames.  Linux (fs/exec.c) puts the argv/envp
     * strings at the very top of the stack and the argc/argv[]/envp[]/auxv
     * frame just below them; the whole thing spans as many pages as it needs,
     * which is what lets a 64 KiB argument or a 6 KiB environment through.
     * Bytes below `str_off` are unused (the image is packed top-down).
     *
     * Layout (high address at top):
     *   [envp strings, NUL-terminated, packed from the top downward]
     *   [argv strings, NUL-terminated, packed below the envp strings]
     *   [AT_RANDOM 16 bytes, AT_EXECFN path]
     *   [argc, argv[0..argc-1], NULL, envp[0..envc-1], NULL, auxv, AT_NULL]
     *                                                           ← ESP here
     */
    uint32_t path_len = (uint32_t)__builtin_strlen(path);
    uint32_t img_cap  = av.used + ev.used + 16 + path_len + 1 +
                        ((uint32_t)argc + (uint32_t)envc + 4 + 2 * 20 + 2) * 4 +
                        64;                        /* alignment slack */
    img_cap = (img_cap + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
    /* The image must stay inside the 8 MiB stack the kernel is prepared to
     * grow; ARG_MAX already guarantees it, this is the belt-and-braces check. */
    if (img_cap > EXEC_STACK_LIMIT - PAGE_SIZE) {
        pgdir_free_user(new_pgdir);
        EXEC_FAIL(-7);                             /* -E2BIG */
    }
    uint8_t *kstack = (uint8_t *)kmalloc(img_cap);
    if (!kstack) {
        pgdir_free_user(new_pgdir);
        EXEC_FAIL(-12);
    }
    __builtin_memset(kstack, 0, img_cap);
    /* kstack[i] will live at this user address: */
    uint32_t ustack_base = USER_STACK_TOP - img_cap;

    /* User addresses of each packed string (the frame's pointer arrays). */
    uint32_t *uargv_ptrs = (uint32_t *)kmalloc(((uint32_t)argc + 1) * 4);
    uint32_t *uenvp_ptrs = (uint32_t *)kmalloc(((uint32_t)envc + 1) * 4);
    if (!uargv_ptrs || !uenvp_ptrs) {
        if (uargv_ptrs) kfree(uargv_ptrs);
        if (uenvp_ptrs) kfree(uenvp_ptrs);
        kfree(kstack);
        pgdir_free_user(new_pgdir);
        EXEC_FAIL(-12);
    }

    /* Pack strings from the top of the image downward: envp first, then argv */
    uint32_t str_off = img_cap;

    /* Pack envp strings */
    for (int i = envc - 1; i >= 0; i--) {
        uint32_t slen = (uint32_t)__builtin_strlen(KENVP(i)) + 1;  /* with NUL */
        str_off -= slen;
        __builtin_memcpy(kstack + str_off, KENVP(i), slen);
        uenvp_ptrs[i] = ustack_base + str_off;
    }

    /* Pack argv strings */
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t slen = (uint32_t)__builtin_strlen(KARGV(i)) + 1;  /* with NUL */
        str_off -= slen;
        __builtin_memcpy(kstack + str_off, KARGV(i), slen);
        uargv_ptrs[i] = ustack_base + str_off;
    }

    /*
     * Linux i386 process-entry stack (what musl/glibc _start expects):
     *
     *   esp → argc
     *         argv[0..argc-1], NULL          (pointer array INLINE)
     *         envp[0..envc-1], NULL
     *         auxv pairs (type,value)..., AT_NULL pair
     *         [AT_RANDOM 16 bytes + strings above]
     *
     * Our own crt0.asm reads the same layout (argv = esp+4).
     */

    /* 16 random bytes for AT_RANDOM (stack canaries).  ustack_base is page
     * aligned, so aligning the offset aligns the user address too. */
    str_off -= 16;
    str_off &= ~3U;
    random_get_bytes(kstack + str_off, 16);
    uint32_t at_random_uaddr = ustack_base + str_off;

    /* A copy of the executable path for AT_EXECFN (glibc __progname, dl checks). */
    uint32_t at_execfn_uaddr = 0;
    {
        str_off -= path_len + 1;
        str_off &= ~3U;
        __builtin_memcpy(kstack + str_off, path, (size_t)path_len + 1);
        at_execfn_uaddr = ustack_base + str_off;
    }

    /* auxv (built top-down so AT_NULL lands at the highest address) */
    uint32_t auxv[2 * 20];
    int auxn = 0;
#define AUX(t, v) do { auxv[auxn++] = (t); auxv[auxn++] = (v); } while (0)
    if (einfo.phdr_vaddr) {
        AUX(3, einfo.phdr_vaddr);            /* AT_PHDR  */
        AUX(4, einfo.phent);                 /* AT_PHENT */
        AUX(5, einfo.phnum);                 /* AT_PHNUM */
    }
    if (interp_base)
        AUX(7, interp_base);                 /* AT_BASE — dynamic linker base */
    AUX(6, PAGE_SIZE);                       /* AT_PAGESZ */
    AUX(9, prog_entry);                      /* AT_ENTRY — the program, not ld.so */
    AUX(11, current_proc->uid);              /* AT_UID  — real credentials, not */
    AUX(12, current_proc->euid);             /* AT_EUID   hardcoded 0; glibc's   */
    AUX(13, current_proc->gid);              /* AT_GID    loader inspects these   */
    AUX(14, current_proc->egid);             /* AT_EGID   (e.g. dynamic-linker).  */
    /* AT_SECURE: set only on a real privilege transition (setuid/setgid exec).
     * For a normal exec where ruid==euid it must be 0, else glibc enters secure
     * mode and ignores LD_LIBRARY_PATH — which breaks the desktop's GTK/X apps
     * (they run as the unprivileged session user and need LD_LIBRARY_PATH). */
    AUX(23, (current_proc->uid != current_proc->euid ||
             current_proc->gid != current_proc->egid) ? 1 : 0);
    AUX(17, 100);                            /* AT_CLKTCK */
    AUX(25, at_random_uaddr);                /* AT_RANDOM */
    AUX(16, cpuid_hwcap());                  /* AT_HWCAP — CPUID.1:EDX feature bits */
    AUX(26, 0);                              /* AT_HWCAP2 */
    AUX(31, at_execfn_uaddr);                /* AT_EXECFN — exec path string */
#undef AUX

    /* Capture the auxv (type,value pairs + an AT_NULL pair) for /proc/self/auxv.
     * auxv[] holds auxn u32 words; append two zero words for the terminator. */
    {
        uint32_t words = (uint32_t)auxn + 2;
        if (words * 4 > sizeof(current_proc->auxv_data))
            words = sizeof(current_proc->auxv_data) / 4;
        uint32_t *dst = (uint32_t *)current_proc->auxv_data;
        for (uint32_t i = 0; i < words; i++)
            dst[i] = (i < (uint32_t)auxn) ? auxv[i] : 0;
        current_proc->auxv_bytes = words * 4;
    }

    /* total frame: argc + argv[] + NULL + envp[] + NULL + auxv + AT_NULL */
    {
        uint32_t words = 1 + (uint32_t)argc + 1 + (uint32_t)envc + 1 +
                         (uint32_t)auxn + 2;
        str_off -= words * 4;
        str_off &= ~15U;                     /* 16-byte align the frame */
    }
    {
        uint32_t *fp = (uint32_t *)(kstack + str_off);
        int w = 0;
        fp[w++] = (uint32_t)argc;
        for (int i = 0; i < argc; i++) fp[w++] = uargv_ptrs[i];
        fp[w++] = 0;
        for (int i = 0; i < envc; i++) fp[w++] = uenvp_ptrs[i];
        fp[w++] = 0;
        for (int i = 0; i < auxn; i++) fp[w++] = auxv[i];
        fp[w++] = 0;                         /* AT_NULL */
        fp[w++] = 0;
    }

    uint32_t user_esp = ustack_base + str_off;

    /* Allocate and map the new user stack region, filling in the image as we
     * go.  It is at least USER_STACK_PAGES long, and longer when the initial
     * stack needs it; the fault handler grows it further (paging.c). */
    {
        uint32_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
        if (ustack_base < stack_base) stack_base = ustack_base;
        for (uint32_t va = stack_base; va < USER_STACK_TOP; va += PAGE_SIZE) {
            uint32_t stack_phys = pmm_alloc_frame();
            if (!stack_phys) {
                kfree(uargv_ptrs);
                kfree(uenvp_ptrs);
                kfree(kstack);
                pgdir_free_user(new_pgdir);
                EXEC_FAIL(-12);
            }
            pmm_frame_incref(stack_phys);
            preempt_disable();
            uint8_t *kp = (uint8_t *)paging_temp_map(stack_phys);
            __builtin_memset(kp, 0, PAGE_SIZE);
            if (va + PAGE_SIZE > ustack_base) {     /* part of the image lands here */
                uint32_t src = va - ustack_base;    /* va >= ustack_base always */
                __builtin_memcpy(kp, kstack + src, PAGE_SIZE);
            }
            paging_temp_unmap();
            preempt_enable();
            pgdir_map(new_pgdir, va, stack_phys,
                      PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        }
    }
    kfree(uargv_ptrs);
    kfree(uenvp_ptrs);
    kfree(kstack);

    /* Point of no return (Linux begin_new_exec): every other thread of this
     * process dies here and the caller becomes the group leader (audit C3). */
    de_thread();

    /* Close any FD_CLOEXEC file descriptors (before CR3 swap) */
    for (int i = 0; i < MAX_FD; i++) {
        proc_file_t *ef = &current_proc->ofile[i];
        if (ef->type == FD_NONE || !ef->cloexec) continue;
        fd_release(ef);
    }

    /* Update process state */
    uint32_t old_pgdir = current_proc->pgdir_phys;
    current_proc->pgdir_phys = new_pgdir;
    current_proc->heap_end   = heap_end;
    current_proc->mmap_next  = mmap_floor;
    /* Image span + heap base for a real /proc/self/maps. */
    current_proc->image_start = einfo.load_bias ? einfo.load_bias
                              : (einfo.phdr_vaddr ? (einfo.phdr_vaddr & ~0xFFFU)
                                                  : 0x08048000U);
    current_proc->image_end  = heap_end;
    current_proc->brk_base   = heap_end;
    vma_clear(current_proc);     /* drop the old address space's anon VMAs */
    current_proc->euid       = new_euid;   /* honour any set-uid/gid bit */
    current_proc->egid       = new_egid;
    current_proc->tgid       = current_proc->pid;  /* exec → new thread-group leader */
    current_proc->vm_owner   = NULL;               /* own address space from here */
    /* Linux begin_new_exec(): the new image inherits none of the old thread's
     * per-thread user-memory hooks — they point into an address space that no
     * longer exists, and honouring them at exit would scribble on the new one. */
    current_proc->clear_child_tid  = 0;
    current_proc->set_child_tid    = 0;
    current_proc->robust_list_head = 0;
    vfork_wake_parent();         /* CLONE_VFORK: we have our own pgdir now */

    /* Store executable path for /proc/self/exe */
    {
        int plen = 0;
        while (path[plen] && plen < 255) plen++;
        __builtin_memcpy(current_proc->exe, path, (size_t)(plen + 1));

        const char *base = path;
        for (int i = 0; i < plen; i++) {
            if (path[i] == '/' && path[i + 1])
                base = path + i + 1;
        }
        int nlen = 0;
        while (base[nlen] && nlen < 15) {
            current_proc->name[nlen] = base[nlen];
            nlen++;
        }
        current_proc->name[nlen] = '\0';
    }

    /* Capture argv/envp (NUL-separated, NUL-terminated) for /proc/self/cmdline
     * and /proc/self/environ.  KARGV/KENVP still point into the kernel-heap
     * collectors (page-table independent) here, before es_free(). */
    {
        uint32_t cl = 0;
        for (int i = 0; i < argc; i++) {
            const char *s = KARGV(i);
            while (*s && cl < sizeof(current_proc->cmdline) - 1)
                current_proc->cmdline[cl++] = *s++;
            if (cl < sizeof(current_proc->cmdline)) current_proc->cmdline[cl++] = '\0';
        }
        current_proc->cmdline_len = cl;

        uint32_t el = 0;
        for (int i = 0; i < envc; i++) {
            const char *s = KENVP(i);
            while (*s && el < sizeof(current_proc->environ) - 1)
                current_proc->environ[el++] = *s++;
            if (el < sizeof(current_proc->environ)) current_proc->environ[el++] = '\0';
        }
        current_proc->environ_len = el;
    }

    /* Reset caught signals to SIG_DFL (fs/exec.c flush_signal_handlers).  If
     * the table is still shared with other threads, unshare it first (Linux
     * unshare_sighand) so their dispositions are untouched. */
    if (current_proc->sighand && current_proc->sighand->refcount > 1) {
        struct sighand *fresh = sighand_alloc();
        if (fresh) {
            sighand_put(current_proc->sighand);
            current_proc->sighand = fresh;
        }
    }
    if (current_proc->sighand) {
        __builtin_memset(current_proc->sighand->handlers, 0,
                         sizeof(current_proc->sighand->handlers));
        __builtin_memset(current_proc->sighand->flags, 0,
                         sizeof(current_proc->sighand->flags));
    }
    current_proc->pending_sigs  = 0;
    current_proc->sigframe_addr = 0;

    /* Update trapframe to run new program */
    registers_t *tf = current_proc->tf;
    tf->eip     = entry;
    tf->useresp = user_esp;

    /* The new image starts with no shared-memory mappings and a fresh mmap
     * region (the old pgdir's frame references are released below). */
    shm_proc_cleanup(current_proc);
    current_proc->mmap_next = mmap_floor;
    fpu_state_init(fpu_area(current_proc));

    /* Switch to new page directory before freeing old one */
    __asm__ volatile("mov %0, %%cr3" :: "r"(new_pgdir) : "memory");

    /* Free old address space */
    if (!pgdir_release(old_pgdir))
        pgdir_free_user(old_pgdir);

    printk("[SYSCALL] exec '%s' pid=%d entry=0x%08x argc=%d\n",
           path, current_proc->pid, (unsigned)entry, argc);
    /* [argv] dump firefox's argv to confirm -profile <dir> actually reaches
     * firefox-bin (the "profile cannot be loaded" modal implies it doesn't). */
    if (path[0] && dbg_str_has(path, "firefox")) {
        for (int ai = 0; ai < argc && ai < 20; ai++)
            printk("[argv] pid=%d [%d]='%s'\n", current_proc->pid, ai, KARGV(ai));
        /* [fdtab] For a CONTENT process (-contentproc), dump the inherited fd
         * table right after exec: which fd numbers survived, and their types.
         * Chromium remaps the prefMap/jsInit shared-memory memfds (FD_FILE=1) to
         * specific numbers the child mmaps; if those numbers hold FD_EPOLL(6) or
         * FD_NONE(0) instead, the remapping/inheritance is broken → mmap EBADF. */
        int is_content = 0;
        for (int ai = 1; ai < argc && ai < 4; ai++)
            if (dbg_str_has(KARGV(ai), "contentproc")) is_content = 1;
        if (is_content) {
            for (int fd = 0; fd < MAX_FD; fd++) {
                int ty = (int)current_proc->ofile[fd].type;
                if (ty != FD_NONE)
                    printk("[fdtab] pid=%d fd=%d type=%d%s\n",
                           current_proc->pid, fd, ty,
                           ty == FD_FILE && current_proc->ofile[fd].node ?
                             (dbg_str_has(current_proc->ofile[fd].node->name, "memfd") ?
                                " MEMFD" : "") : "");
            }
        }
    }
    /* (watchpoint_arm exists for KVM/real-hw debugging of the GTK heap race;
     * QEMU TCG ignores guest DR registers so it's not armed here.) */
    es_free(&av);
    es_free(&ev);
#undef KARGV
#undef KENVP
#undef EXEC_FAIL
    return 0;  /* trapret irets to entry */
}

/* ── sys_kill(pid_t pid, int sig) — EAX=37 ──────────────────────────────── */
static int sys_kill(registers_t *regs) {
    int pid = (int)regs->ebx;
    int sig = (int)regs->ecx;

    if (sig < 0 || sig >= NSIGS) return -22;  /* -EINVAL */

    /* pid=0 -> current process group; pid<0 -> process group -pid.  One
     * signal per PROCESS (Linux __kill_pgrp_info -> group_send_sig_info for
     * each process in the group), delivered to a thread that does not block it. */
    if (pid == 0) {
        int pg = current_proc ? current_proc->pgrp : 0;
        if (sig) signal_send_pgrp(pg, sig);
        return 0;
    }
    if (pid < 0) {
        int sent = sig ? signal_send_pgrp(-pid, sig) : 1;
        return sent ? 0 : -3;
    }

    /* kill(pid): pid may name any thread of a process (Linux kill_pid_info
     * uses the thread group of the task with that pid).  The signal is
     * process-directed: complete_signal() picks one thread that does not block
     * it; a fatal default disposition then ends the whole group at delivery. */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (ptable[i].pid == pid && ptable[i].state != PROC_UNUSED) {
            if (sig == 0) return 0;               /* existence check */
            if (sig == SIGKILL) {
                /* Kill the thread group AND the entire process subtree.  The
                 * subtree part is not Linux behaviour (Linux kills only the
                 * group); it is kept deliberately so a watchdog killing a
                 * misbehaving multiprocess application does not leak its
                 * forked children into the 128-slot process table.  Marking is
                 * done before any victim runs (signal_send only sets a pending
                 * bit), so parent links are still intact for the walk. */
                int tg = ptable[i].tgid;
                for (int j = 0; j < MAX_PROCS; j++)
                    if (ptable[j].state != PROC_UNUSED && ptable[j].tgid == tg)
                        signal_send(&ptable[j], sig);
                int changed = 1;
                while (changed) {
                    changed = 0;
                    for (int j = 0; j < MAX_PROCS; j++) {
                        struct proc *p = &ptable[j];
                        if (p->state == PROC_UNUSED) continue;
                        if (p->pending_sigs & (1u << SIGKILL)) continue;  /* already */
                        if (p->parent && (p->parent->pending_sigs & (1u << SIGKILL))) {
                            signal_send(p, SIGKILL);
                            changed = 1;
                        }
                    }
                }
            } else {
                signal_send_group(&ptable[i], sig);
            }
            return 0;
        }
    }
    return -3;  /* -ESRCH: no such process */
}

/* ── sys_signal(int signum, sighandler_t handler) — EAX=48 ─────────────── */
static int sys_signal(registers_t *regs) {
    int          signum  = (int)regs->ebx;
    sighandler_t handler = (sighandler_t)(uintptr_t)regs->ecx;

    if (signum < 1 || signum >= NSIGS) return -22;  /* -EINVAL */
    if (signum == SIGKILL || signum == SIGSTOP) return -22;

    sighandler_t old = current_proc->sighand->handlers[signum];
    current_proc->sighand->handlers[signum] = handler;
    return (int)(uintptr_t)old;
}

/* ── sys_sigreturn() — EAX=119 ──────────────────────────────────────────── */
static void sys_sigreturn(registers_t *regs) {
    /* The per-frame trampoline passed the restore address in ecx and a type
     * marker in edx (nesting-safe; honours handler ucontext modifications). */
    if (sigreturn_restore(regs, regs->ecx, regs->edx) < 0)
        proc_group_exit(SIGSEGV);   /* spurious/invalid sigreturn */
    current_proc->sigframe_addr = 0;
}

/* ── sys_pipe(int fd[2]) — EAX=42 ───────────────────────────────────────── */
static int sys_pipe(registers_t *regs) {
    int *fds = (int *)(uintptr_t)regs->ebx;

    if (!access_ok(fds, 2 * sizeof(int)))
        return -14;  /* -EFAULT */

    pipe_buf_t *pb = pipe_alloc();
    if (!pb) return -12;  /* -ENOMEM */

    /* Find two free slots */
    int rfd = -1, wfd = -1;
    for (int i = 0; i < MAX_FD && (rfd < 0 || wfd < 0); i++) {
        if (current_proc->ofile[i].type == FD_NONE) {
            if (rfd < 0) rfd = i;
            else         wfd = i;
        }
    }
    if (rfd < 0 || wfd < 0) { kfree(pb); return -24; }  /* -EMFILE */

    current_proc->ofile[rfd].type  = FD_PIPE_R;
    current_proc->ofile[rfd].pipe  = pb;
    current_proc->ofile[rfd].flags = 0;
    current_proc->ofile[wfd].type  = FD_PIPE_W;
    current_proc->ofile[wfd].pipe  = pb;
    current_proc->ofile[wfd].flags = 0;

    int kfds[2] = { rfd, wfd };
    int cr = copy_to_user(fds, kfds, sizeof(kfds));
    if (cr < 0) {
        fd_release(&current_proc->ofile[rfd]);
        fd_release(&current_proc->ofile[wfd]);
        return cr;
    }
    return 0;
}

/* ── sys_dup2(int oldfd, int newfd) — EAX=63 ───────────────────────────── */
static int sys_dup2(registers_t *regs) {
    int oldfd = (int)regs->ebx;
    int newfd = (int)regs->ecx;

    if (oldfd < 0 || oldfd >= MAX_FD || newfd < 0 || newfd >= MAX_FD)
        return -9;   /* -EBADF */

    proc_file_t *src = &current_proc->ofile[oldfd];
    if (src->type == FD_NONE) return -9;

    if (oldfd == newfd) return newfd;

    /* Close newfd if currently open */
    proc_file_t *dst = &current_proc->ofile[newfd];
    if (dst->type != FD_NONE)
        fd_release(dst);

    /* [dup2fd] trace firefox dup2 of a MEMFD (Chromium fds_to_remap places the
     * prefs/prefMap/jsInit shared-memory memfds at specific target numbers for
     * the content child).  old→new + the memfd's size reveals which shared
     * memory (by size: prefs 20194 / prefMap 234963 / jsInit 240916) lands at
     * which fd — to explain why the child mmaps jsInit at fd 11 (empty). */
    if (src->type == FD_FILE && src->node &&
        dbg_str_has(src->node->name, "memfd") && current_proc &&
        current_proc->name[0]=='f' && current_proc->name[1]=='i' &&
        current_proc->name[4]=='f') {
        static int d2 = 0;
        if (d2 < 40) { d2++;
            printk("[dup2fd] pid=%d memfd %d->%d size=%u\n",
                   current_proc->pid, oldfd, newfd,
                   (unsigned)src->node->size); }
    }

    /* Copy the entry and bump refcount */
    *dst = *src;
    /* POSIX/Linux dup2(2): the DUPLICATE fd does NOT inherit the close-on-exec
     * flag — it is always CLEARED on newfd.  Our `*dst = *src` copies cloexec
     * from the source, which is WRONG: Chromium dup2's a MFD_CLOEXEC memfd onto
     * a target fd to hand it to the content child across exec (fds_to_remap),
     * expecting it to SURVIVE exec.  Copying cloexec=1 made execve close it →
     * the jsInit/prefMap shared-memory fd vanished → child mmap'd a stale/reused
     * fd → EBADF → ImageBridgeChild::InitSameProcess NULL-deref crash.  Clear it. */
    dst->cloexec = 0;
    fd_retain(dst);

    return newfd;
}

/* ── sys_unlink(path) — EAX=10 ───────────────────────────────────────────── */
static int sys_unlink(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0)
        return -14;

    char resolved[256];
    int r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return sys_unlink_kernel_path(resolved);
}

/* ── sys_chdir(path) — EAX=12 ────────────────────────────────────────────── */
static int sys_chdir(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0)
        return -14;
    vfs_node_t *n = vfs_open_at(path);
    if (!n) return -2;
    if (!(n->flags & VFS_FLAG_DIR)) return -20;  /* ENOTDIR */
    char canonical[256];
    int cr = canonicalize_path_at_cwd(path, canonical, sizeof(canonical));
    if (cr < 0) return cr;
    __builtin_memcpy(current_proc->cwd, canonical, sizeof(canonical));
    return 0;
}

/* ── sys_lseek(fd, offset, whence) — EAX=19 ─────────────────────────────── */
static int sys_lseek(registers_t *regs) {
    int fd     = (int)regs->ebx;
    int off    = (int)regs->ecx;   /* signed */
    int whence = (int)regs->edx;

    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type == FD_NONE) return -9;
    if (f->type == FD_PIPE_R || f->type == FD_PIPE_W) return -29;  /* ESPIPE */

    uint32_t new_off;
    uint32_t fsize = f->node ? f->node->size : 0;
    switch (whence) {
    case 0: new_off = (uint32_t)off; break;
    case 1: new_off = f->offset + (uint32_t)off; break;
    case 2: new_off = fsize + (uint32_t)off; break;
    default: return -22;
    }
    f->offset = new_off;
    return (int)new_off;
}

/* ── credential syscalls ─────────────────────────────────────────────────── */
static int sys_getuid(registers_t *regs)  { (void)regs; return (int)current_proc->uid; }
static int sys_getgid(registers_t *regs)  { (void)regs; return (int)current_proc->gid; }
static int sys_geteuid(registers_t *regs) { (void)regs; return (int)current_proc->euid; }
static int sys_getegid(registers_t *regs) { (void)regs; return (int)current_proc->egid; }

/* setuid(uid): root sets all three (real/eff/saved); a non-root process may
 * only set its (e)uid back to its real uid.  Returns 0 or -EPERM. */
static int sys_setuid(registers_t *regs) {
    uint32_t u = (uint32_t)regs->ebx;
    if (current_proc->euid == 0) {
        current_proc->uid = current_proc->euid = u;
        return 0;
    }
    if (u == current_proc->uid) { current_proc->euid = u; return 0; }
    return -1; /* -EPERM */
}
static int sys_setgid(registers_t *regs) {
    uint32_t g = (uint32_t)regs->ebx;
    if (current_proc->euid == 0) {
        current_proc->gid = current_proc->egid = g;
        return 0;
    }
    if (g == current_proc->gid) { current_proc->egid = g; return 0; }
    return -1; /* -EPERM */
}
static int sys_seteuid(registers_t *regs) {
    uint32_t u = (uint32_t)regs->ebx;
    if (current_proc->euid == 0 || u == current_proc->uid) {
        current_proc->euid = u;
        return 0;
    }
    return -1;
}

/* ── sys_mkdir(path, mode) — EAX=39 ─────────────────────────────────────── */
static int sys_mkdir(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0)
        return -14;

    char resolved[256];
    int r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return sys_mkdir_kernel_path(resolved);
}

/* ── sys_times — EAX=43 ──────────────────────────────────────────────────── */
static int sys_times(registers_t *regs) {
    /* struct tms { clock_t tms_utime, tms_stime, tms_cutime, tms_cstime }; */
    uint32_t *buf = (uint32_t *)(uintptr_t)regs->ebx;
    if (buf) {
        uint32_t ut = current_proc ? current_proc->utime_ticks : 0;
        uint32_t tms[4] = { ut, 0, 0, 0 };
        int cr = copy_to_user(buf, tms, sizeof(tms));
        if (cr < 0) return cr;
    }
    return (int)pit_ticks();
}

/* ── sys_ioctl(fd, request, arg) — EAX=54 ───────────────────────────────── */
static int sys_ioctl(registers_t *regs) {
    int fd  = (int)regs->ebx;
    int req = (int)regs->ecx;

    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type == FD_FILE && f->node && f->node->ioctl_fn) {
        void *arg = (void *)(uintptr_t)regs->edx;
        size_t arg_len = 1;
        if (req == FBIOGET_FSCREENINFO) arg_len = sizeof(fb_fix_screeninfo_t);
        if (req == FBIOGET_VSCREENINFO) arg_len = sizeof(fb_var_screeninfo_t);
        if ((uint32_t)req == 0x80045430U || (uint32_t)req == 0x40045431U)
            arg_len = sizeof(int);
        if (req == 0x5401 || req == 0x5402 || req == 0x5403 || req == 0x5404)
            arg_len = 36;
        if (req == 0x5413)
            arg_len = sizeof(uint16_t) * 4;
        if (req == 0x5414 || req == 0x5415 || req == 0x540E)
            arg_len = sizeof(int);
        if (arg && !access_ok(arg, arg_len)) return -14;
        return f->node->ioctl_fn(f->node, (uint32_t)req, arg);
    }

    /* TCGETS = 0x5401, TCSETS = 0x5402, TIOCGWINSZ = 0x5413 */
    if (req == 0x5413) {
        /* TIOCGWINSZ — return fake 80x25 terminal */
        uint16_t *ws = (uint16_t *)(uintptr_t)regs->edx;
        if (ws) {
            uint16_t kws[4] = { 25, 80, 0, 0 };
            int cr = copy_to_user(ws, kws, sizeof(kws));
            if (cr < 0) return cr;
        }
        return 0;
    }
    if (req == 0x5401) {
        /* TCGETS — return real termios from TTY state */
        void *t = (void *)(uintptr_t)regs->edx;
        if (t) {
            uint8_t kt[36];
            tty_get_termios(kt);
            int cr = copy_to_user(t, kt, sizeof(kt));
            if (cr < 0) return cr;
        }
        return 0;
    }
    if (req == 0x5402 || req == 0x5403 || req == 0x5404) {
        /* TCSETS/TCSETSW/TCSETSF — update real termios state */
        const void *t = (const void *)(uintptr_t)regs->edx;
        if (t) {
            uint8_t kt[36];
            int cr = copy_from_user(kt, t, sizeof(kt));
            if (cr < 0) return cr;
            tty_set_termios(kt);
        }
        return 0;
    }
    if (req == 0x5410) {  /* TIOCGSID: return the session ID (use pgrp of shell) */
        int *out = (int *)(uintptr_t)regs->edx;
        int sid = current_proc ? current_proc->pgrp : 1;
        if (out) {
            int cr = copy_to_user(out, &sid, sizeof(sid));
            if (cr < 0) return cr;
        }
        return sid;
    }
    if (req == 0x5414) {  /* TIOCGPGRP: get foreground pgrp */
        int *out = (int *)(uintptr_t)regs->edx;
        int fg = tty_fg_pgrp ? tty_fg_pgrp : (current_proc ? current_proc->pgrp : 1);
        if (out) {
            int cr = copy_to_user(out, &fg, sizeof(fg));
            if (cr < 0) return cr;
        }
        return fg;
    }
    if (req == 0x5415) {  /* TIOCSPGRP: set foreground pgrp */
        const int *inp = (const int *)(uintptr_t)regs->edx;
        if (inp) {
            int fg = 0;
            int cr = copy_from_user(&fg, inp, sizeof(fg));
            if (cr < 0) return cr;
            tty_fg_pgrp = fg;
        }
        return 0;
    }
    return -25;  /* ENOTTY */
}

/* ── sys_fcntl(fd, cmd, arg) — EAX=55 ───────────────────────────────────── */
static int sys_fcntl(registers_t *regs) {
    int fd  = (int)regs->ebx;
    int cmd = (int)regs->ecx;
    int arg = (int)regs->edx;

    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type == FD_NONE) return -9;

    switch (cmd) {
    case 0:   /* F_DUPFD — find a free fd >= arg, cloexec=0 */
    case 1030: /* F_DUPFD_CLOEXEC — same but set cloexec */
        if (arg < 0 || arg >= MAX_FD) return -22;
        for (int i = arg >= 0 ? arg : 0; i < MAX_FD; i++) {
            if (current_proc->ofile[i].type == FD_NONE) {
                current_proc->ofile[i] = *f;
                /* F_DUPFD clears cloexec; F_DUPFD_CLOEXEC sets it */
                current_proc->ofile[i].cloexec = (cmd == 1030) ? 1 : 0;
                fd_retain(&current_proc->ofile[i]);
                return i;
            }
        }
        return -24;  /* EMFILE */
    case 1:  /* F_GETFD */ return f->cloexec ? FD_CLOEXEC : 0;
    case 2:  /* F_SETFD */
        f->cloexec = (arg & FD_CLOEXEC) ? 1 : 0;
        return 0;
    case 3:  /* F_GETFL */
        if (f->type == FD_NONE) return -9;
        return f->flags;
    case 4:  /* F_SETFL */
        f->flags = (f->flags & 3) | (arg & (O_APPEND | 0x800));
        return 0;
    case 5:   /* F_GETLK   */
    case 12:  /* F_GETLK64 — advisory locks are not enforced.  CRITICAL: the caller
              * uses F_GETLK to probe whether locking works AND to learn if the
              * region is already locked; it reads back l_type.  We must report
              * the region as UNLOCKED by writing l_type = F_UNLCK (2), else the
              * caller (e.g. Firefox's nsProfileLock::LockWithFcntl) concludes the
              * profile is held by another process and fails with "Access was
              * denied".  struct flock/flock64 on i386: short l_type is field 0.
              * NB: glibc on 32-bit compiled with _FILE_OFFSET_BITS=64 (which
              * Firefox is) issues the *64* variants (12/13/14), not 5/6/7 — so
              * these MUST be handled or nsProfileLock fails and Firefox declares
              * its profile "missing or inaccessible" (a modal that hangs). */
        if (!arg || !access_ok((void *)(uintptr_t)arg, sizeof(short) * 2))
            return -14;
        *(short *)(uintptr_t)arg = 2;   /* F_UNLCK */
        return 0;
    case 6:   /* F_SETLK    */
    case 7:   /* F_SETLKW   */
    case 13:  /* F_SETLK64  */
    case 14:  /* F_SETLKW64 */
        if (!arg || !access_ok((void *)(uintptr_t)arg, sizeof(short) * 2))
            return -14;
        return 0;
    case 1033: /* F_ADD_SEALS — memfd sealing.  We don't enforce seals, but
                * accept them so Firefox's freezeable shared memory "freezes"
                * (an EINVAL here leaves the buffer in a broken, unfrozen state). */
        f->seals |= (uint32_t)arg;
        return 0;
    case 1034: /* F_GET_SEALS */
        return (int)f->seals;
    }
    return -22;  /* EINVAL */
}

/* ── Clocks ──────────────────────────────────────────────────────────────────
 * clock_gettime ids (uapi/linux/time.h).  MONOTONIC, MONOTONIC_RAW, BOOTTIME
 * and MONOTONIC_COARSE are uptime; REALTIME, REALTIME_COARSE and TAI are the
 * RTC epoch sampled at boot plus uptime (nothing steps the clock); the CPU-time
 * clocks are the scheduler's per-thread tick accounting.  The fine clocks are
 * TSC-interpolated between 100 Hz ticks (arch/i686/cpu/tsc.c), so their
 * resolution is 1 ns like Linux reports; the *_COARSE clocks are the tick. */
#define CLK_REALTIME_K          0
#define CLK_MONOTONIC_K         1
#define CLK_PROCESS_CPUTIME_K   2
#define CLK_THREAD_CPUTIME_K    3
#define CLK_MONOTONIC_RAW_K     4
#define CLK_REALTIME_COARSE_K   5
#define CLK_MONOTONIC_COARSE_K  6
#define CLK_BOOTTIME_K          7
#define CLK_REALTIME_ALARM_K    8
#define CLK_BOOTTIME_ALARM_K    9
#define CLK_TAI_K               11

/* CPU time of one thread or of a whole thread group, in ticks. */
static uint32_t cputime_ticks(int tgid, int tid) {
    uint32_t t = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_UNUSED) continue;
        if (tid ? (p->pid == tid) : (p->tgid == tgid)) t += p->utime_ticks;
    }
    return t;
}

/* Read clock `clk` into (sec, nsec).  Returns 0 or -EINVAL for unknown ids. */
static int kclock_get(int clk, int64_t *sec, uint32_t *nsec) {
    uint32_t ms, mns;
    switch (clk) {
    case CLK_MONOTONIC_K: case CLK_MONOTONIC_RAW_K: case CLK_BOOTTIME_K:
    case CLK_BOOTTIME_ALARM_K:
        clock_mono(&ms, &mns);
        *sec = ms; *nsec = mns;
        return 0;
    case CLK_MONOTONIC_COARSE_K: {
        uint32_t t = pit_ticks();
        *sec = t / TICK_HZ; *nsec = (t % TICK_HZ) * TICK_NS;
        return 0;
    }
    case CLK_REALTIME_K: case CLK_REALTIME_ALARM_K: case CLK_TAI_K:
        clock_mono(&ms, &mns);
        *sec = (int64_t)rtc_boot_epoch() + ms; *nsec = mns;
        return 0;
    case CLK_REALTIME_COARSE_K: {
        uint32_t t = pit_ticks();
        *sec = (int64_t)rtc_boot_epoch() + t / TICK_HZ; *nsec = (t % TICK_HZ) * TICK_NS;
        return 0;
    }
    case CLK_PROCESS_CPUTIME_K: case CLK_THREAD_CPUTIME_K: {
        uint32_t t = current_proc
                   ? cputime_ticks(current_proc->tgid,
                                   clk == CLK_THREAD_CPUTIME_K ? current_proc->pid : 0)
                   : 0;
        *sec = t / TICK_HZ; *nsec = (t % TICK_HZ) * TICK_NS;
        return 0;
    }
    default:
        break;
    }
    if (clk < 0) {
        /* Dynamic CPU clocks (clock_getcpuclockid / pthread_getcpuclockid):
         * id = ~(pid << 3) | type, bit 2 selects a thread (CPUCLOCK_PERTHREAD). */
        int pid  = ~(clk >> 3);
        int type = clk & 7;
        if ((type & 3) == 3) return -22;
        if (pid == 0 && current_proc) pid = (type & 4) ? current_proc->pid : current_proc->tgid;
        uint32_t t = (type & 4) ? cputime_ticks(0, pid) : cputime_ticks(pid, 0);
        int found = 0;
        for (int i = 0; i < MAX_PROCS && !found; i++)
            if (ptable[i].state != PROC_UNUSED &&
                ((type & 4) ? ptable[i].pid == pid : ptable[i].tgid == pid)) found = 1;
        if (!found) return -22;
        *sec = t / TICK_HZ; *nsec = (t % TICK_HZ) * TICK_NS;
        return 0;
    }
    return -22;
}

/* Resolution of clock `clk` in ns, or -EINVAL. */
static int kclock_res(int clk, uint32_t *nsec) {
    switch (clk) {
    case CLK_REALTIME_COARSE_K: case CLK_MONOTONIC_COARSE_K:
        *nsec = TICK_NS;                 /* the tick (Linux: jiffy) */
        return 0;
    case CLK_REALTIME_K: case CLK_MONOTONIC_K: case CLK_MONOTONIC_RAW_K:
    case CLK_BOOTTIME_K: case CLK_REALTIME_ALARM_K: case CLK_BOOTTIME_ALARM_K:
    case CLK_TAI_K: case CLK_PROCESS_CPUTIME_K: case CLK_THREAD_CPUTIME_K:
        *nsec = clock_tsc_calibrated() ? 1 : TICK_NS;
        return 0;
    default:
        if (clk < 0) { *nsec = 1; return 0; }
        return -22;
    }
}

/* Sleep until the MONOTONIC instant (dsec, dnsec) since boot.  Returns 0 once
 * the deadline has passed, or -EINTR when a signal that will be delivered is
 * pending, with the remaining time in (*rsec, *rnsec) (Linux hrtimer_nanosleep:
 * the remainder is what nanosleep(2) reports and what a restart would sleep).
 * The scheduler wakes us at the first tick at or after the deadline, so we can
 * never return early; spurious wakeups simply re-arm. */
static int ksleep_until_mono(uint32_t dsec, uint32_t dnsec, uint32_t *rsec, uint32_t *rnsec) {
    for (;;) {
        uint32_t s, ns;
        clock_mono(&s, &ns);
        if (s > dsec || (s == dsec && ns >= dnsec)) {
            if (rsec) { *rsec = 0; *rnsec = 0; }
            return 0;
        }
        if (signal_interrupt_pending(current_proc)) {
            if (rsec) {
                uint32_t rs = dsec - s, rn;
                if (dnsec >= ns) rn = dnsec - ns; else { rs--; rn = dnsec + 1000000000U - ns; }
                *rsec = rs; *rnsec = rn;
            }
            return -4;                       /* -EINTR */
        }
        uint32_t wt = clock_mono_to_tick(dsec, dnsec);
        if ((int32_t)(wt - pit_ticks()) <= 0) wt = pit_ticks() + 1;
        if (!wt) wt = 1;                     /* 0 means "no deadline" */
        current_proc->wake_tick = wt;
        sleep_on((void *)&ksleep_until_mono);
    }
}

/* ── Timeout deadlines (select / poll / epoll_wait / futex) ──────────────────
 * Linux never lets a timeout-based wait return EARLY: poll_select_set_timeout()
 * and schedule_hrtimeout_range() round the requested interval UP, so the
 * elapsed time the caller measures is always >= what it asked for.
 *
 * A tick counter alone cannot promise that.  pit_ticks() is sampled at an
 * arbitrary point INSIDE the current 10 ms tick, so "N ticks have gone by"
 * means anywhere from (N-1)*10 ms to N*10 ms of real time — a 100 ms select
 * armed for 10 ticks could return after 91 ms, which is what p08 caught.
 *
 * So a deadline is kept as an ABSOLUTE instant on the fine-grained monotonic
 * clock — the same clock clock_gettime() reports, so the caller's own
 * measurement agrees with ours — and the scheduler's wake tick is derived with
 * clock_mono_to_tick(), the first tick at or after that instant. */
struct kdeadline { uint32_t sec, nsec; };

/* Deadline `sec` seconds + `nsec` nanoseconds from now. */
static void deadline_set(struct kdeadline *d, uint32_t sec, uint32_t nsec) {
    uint32_t s, ns;
    clock_mono(&s, &ns);
    s  += sec + nsec / 1000000000U;
    ns += nsec % 1000000000U;
    if (ns >= 1000000000U) { ns -= 1000000000U; s++; }
    d->sec = s; d->nsec = ns;
}

/* Deadline `ms` milliseconds from now. */
static void deadline_set_ms(struct kdeadline *d, uint32_t ms) {
    deadline_set(d, ms / 1000U, (ms % 1000U) * 1000000U);
}

/* Non-zero once the monotonic clock has reached the deadline. */
static int deadline_expired(const struct kdeadline *d) {
    uint32_t s, ns;
    clock_mono(&s, &ns);
    return s > d->sec || (s == d->sec && ns >= d->nsec);
}

/* The tick this sleeper must be woken at so it cannot wake before `d`. */
static uint32_t deadline_wake_tick(const struct kdeadline *d) {
    uint32_t wt = clock_mono_to_tick(d->sec, d->nsec);
    uint32_t now = pit_ticks();
    if ((int32_t)(wt - now) <= 0) wt = now + 1;   /* never in the past */
    return wt ? wt : 1;                            /* 0 means "no deadline" */
}

/* Ticks to sleep before the next readiness re-check: at most `cap`, and never
 * past the deadline's wake tick. */
static uint32_t deadline_sleep_ticks(const struct kdeadline *d, uint32_t cap) {
    uint32_t left = deadline_wake_tick(d) - pit_ticks();
    if ((int32_t)left <= 0) return 1;
    return left < cap ? left : cap;
}

/* Validate a timespec (Linux timespec64_valid): nsec in [0, 1e9). */
static int ts_valid(int64_t sec, int64_t nsec) {
    return sec >= 0 && nsec >= 0 && nsec < 1000000000LL;
}

/* Turn a clock_nanosleep request into a MONOTONIC deadline.  Returns 0, or a
 * negative errno (unsupported clock, bad flags).  *expired is set when the
 * absolute deadline is already in the past. */
#define TIMER_ABSTIME_K 1
static int knanosleep_deadline(int clk, int flags, int64_t rsec, int64_t rnsec,
                               uint32_t *dsec, uint32_t *dnsec, int *expired) {
    if (flags & ~TIMER_ABSTIME_K) return -22;
    switch (clk) {
    case CLK_REALTIME_K: case CLK_MONOTONIC_K: case CLK_BOOTTIME_K:
    case CLK_PROCESS_CPUTIME_K:           /* accepted by Linux; we sleep on wall time */
        break;
    case CLK_REALTIME_COARSE_K: case CLK_MONOTONIC_COARSE_K: case CLK_MONOTONIC_RAW_K:
    case CLK_THREAD_CPUTIME_K:
        return -95;                       /* -EOPNOTSUPP (Linux: ENOTSUP) */
    default:
        return -22;
    }
    if (rnsec < 0 || rnsec >= 1000000000LL) return -22;
    uint32_t ms, mns;
    clock_mono(&ms, &mns);
    *expired = 0;
    if (flags & TIMER_ABSTIME_K) {
        int64_t asec = rsec;
        if (clk == CLK_REALTIME_K) asec -= (int64_t)rtc_boot_epoch();   /* wall → uptime */
        if (asec < 0 || (asec == 0 && rnsec == 0) || asec < (int64_t)ms ||
            (asec == (int64_t)ms && (uint32_t)rnsec <= mns)) { *expired = 1; return 0; }
        if (asec > (int64_t)ms + 20000000LL) asec = (int64_t)ms + 20000000LL;  /* ~231 days: keep tick math in range */
        *dsec = (uint32_t)asec; *dnsec = (uint32_t)rnsec;
        return 0;
    }
    if (rsec < 0) return -22;
    if (rsec == 0 && rnsec == 0) { *expired = 1; return 0; }
    if (rsec > 20000000LL) rsec = 20000000LL;
    uint32_t ds = ms + (uint32_t)rsec, dn = mns + (uint32_t)rnsec;
    if (dn >= 1000000000U) { dn -= 1000000000U; ds++; }
    *dsec = ds; *dnsec = dn;
    return 0;
}

/* ── sys_gettimeofday(timeval *tv, timezone *tz) — EAX=78 ───────────────── */
static int sys_gettimeofday(registers_t *regs) {
    struct ktimeval *tv = (struct ktimeval *)(uintptr_t)regs->ebx;
    if (tv) {
        struct ktimeval ktv;
        int64_t sec; uint32_t nsec;
        kclock_get(CLK_REALTIME_K, &sec, &nsec);
        ktv.tv_sec  = (int32_t)sec;
        ktv.tv_usec = (int32_t)(nsec / 1000U);
        int cr = copy_to_user(tv, &ktv, sizeof(ktv));
        if (cr < 0) return cr;
    }
    return 0;
}

/* ── sys_time(time_t *t) — EAX=13 ───────────────────────────────────────── */
static int sys_time(registers_t *regs) {
    int32_t *ut = (int32_t *)(uintptr_t)regs->ebx;
    int64_t sec; uint32_t nsec;
    kclock_get(CLK_REALTIME_K, &sec, &nsec);
    int32_t s32 = (int32_t)sec;
    if (ut) {
        int cr = copy_to_user(ut, &s32, sizeof(s32));
        if (cr < 0) return cr;
    }
    return s32;
}

/* ── sys_stat(path, stat*) — EAX=106 ────────────────────────────────────── */
static int sys_stat(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0)
        return -14;
    struct kstat *st = (struct kstat *)(uintptr_t)regs->ecx;
    if (!access_ok(st, sizeof(*st))) return -14;

    vfs_node_t *n = vfs_open_at(path);
    fx_lockdbg("stat", path, n ? 0 : -2);
    if (!n) return -2;
    struct kstat kst;
    fill_kstat(&kst, n);
    return copy_to_user(st, &kst, sizeof(kst));
}

/* ── sys_fstat(fd, stat*) — EAX=108 ─────────────────────────────────────── */
static int sys_fstat(registers_t *regs) {
    int fd = (int)regs->ebx;
    struct kstat *st = (struct kstat *)(uintptr_t)regs->ecx;
    if (!access_ok(st, sizeof(*st))) return -14;

    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];

    if (f->type == FD_NONE) {
        /* stdin/stdout/stderr: pretend it's a char device */
        if (fd <= 2) {
            struct kstat kst;
            __builtin_memset(&kst, 0, sizeof(kst));
            kst.st_mode = 0020666; /* S_IFCHR | rw-rw-rw- */
            return copy_to_user(st, &kst, sizeof(kst));
        }
        return -9;
    }
    if (f->type == FD_PIPE_R || f->type == FD_PIPE_W) {
        struct kstat kst;
        __builtin_memset(&kst, 0, sizeof(kst));
        kst.st_mode  = 0010666;  /* S_IFIFO */
        kst.st_blksize = 4096;
        return copy_to_user(st, &kst, sizeof(kst));
    }
    struct kstat kst;
    fill_kstat(&kst, f->node);
    return copy_to_user(st, &kst, sizeof(kst));
}

/* ── sys_uname(utsname*) — EAX=122 ──────────────────────────────────────── */
static int sys_uname(registers_t *regs) {
    struct kutsname *u = (struct kutsname *)(uintptr_t)regs->ebx;
    if (!access_ok(u, sizeof(*u))) return -14;

    static const char *sysname   = "Linux";
    static const char *nodename  = "maeros";
    static const char *release   = "5.15.0-maeros";
    static const char *version   = "#1 SMP MaeroOS";
    static const char *machine   = "i686";
    static const char *domain    = "";

    struct kutsname ku;
    __builtin_memset(&ku, 0, sizeof(ku));
    __builtin_memcpy(ku.sysname,    sysname,  __builtin_strlen(sysname)  + 1);
    __builtin_memcpy(ku.nodename,   nodename, __builtin_strlen(nodename) + 1);
    __builtin_memcpy(ku.release,    release,  __builtin_strlen(release)  + 1);
    __builtin_memcpy(ku.version,    version,  __builtin_strlen(version)  + 1);
    __builtin_memcpy(ku.machine,    machine,  __builtin_strlen(machine)  + 1);
    __builtin_memcpy(ku.domainname, domain,   1);
    return copy_to_user(u, &ku, sizeof(ku));
}

/* ── sys_nanosleep(timespec *req, timespec *rem) — EAX=162 ──────────────── */
static int sys_nanosleep(registers_t *regs) {
    struct ktimespec *req = (struct ktimespec *)(uintptr_t)regs->ebx;
    struct ktimespec *rem = (struct ktimespec *)(uintptr_t)regs->ecx;
    if (!req) return -14;
    struct ktimespec kreq;
    int cr = copy_from_user(&kreq, req, sizeof(kreq));
    if (cr < 0) return cr;
    if (!ts_valid(kreq.tv_sec, kreq.tv_nsec)) return -22;

    uint32_t ds, dn; int expired;
    int r = knanosleep_deadline(CLK_MONOTONIC_K, 0, kreq.tv_sec, kreq.tv_nsec, &ds, &dn, &expired);
    if (r < 0) return r;
    if (expired) return 0;
    uint32_t rs = 0, rn = 0;
    r = ksleep_until_mono(ds, dn, &rs, &rn);
    /* Linux writes the remainder only when interrupted (-EINTR). */
    if (r == -4 && rem) {
        struct ktimespec krem = { (int32_t)rs, (int32_t)rn };
        cr = copy_to_user(rem, &krem, sizeof(krem));
        if (cr < 0) return cr;
    }
    return r;
}

/* ── sys_getdents(fd, buf, count) — EAX=141 ─────────────────────────────── */
static int sys_getdents(registers_t *regs) {
    int       fd    = (int)regs->ebx;
    char     *buf   = (char *)(uintptr_t)regs->ecx;
    uint32_t  count = (uint32_t)regs->edx;

    if (!access_ok(buf, count)) return -14;
    if (fd < 0 || fd >= MAX_FD) return -9;

    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type != FD_FILE || !(f->node->flags & VFS_FLAG_DIR)) return -20;

    uint32_t written = 0;
    vfs_dirent_t de;

    for (;;) {
        if (vfs_readdir(f->node, f->offset, &de) < 0) break;
        f->offset++;

        uint32_t nlen    = __builtin_strlen(de.name);
        uint32_t reclen  = (9 + nlen + 1 + 1 + 3) & ~3U; /* 8B hdr + name + type + NUL */
        if (written + reclen > count) break;
        if (reclen > 512) return written ? (int)written : -22;

        uint8_t rec[512];
        __builtin_memset(rec, 0, reclen);
        struct linux_dirent *d = (struct linux_dirent *)rec;
        d->d_ino    = de.ino;
        d->d_off    = (uint32_t)(written + reclen);
        d->d_reclen = (uint16_t)reclen;
        __builtin_memcpy(d->d_name, de.name, nlen);
        /* d_type goes at d_name[reclen - 9] */
        rec[reclen - 1] = vfs_type_to_dt(de.type);
        rec[8 + nlen]   = '\0';
        int cr = copy_to_user(buf + written, rec, reclen);
        if (cr < 0) return cr;
        written += reclen;
    }
    return (int)written;
}

/* ── sys_getdents64(fd, buf, count) — EAX=220 ───────────────────────────── */
static int sys_getdents64(registers_t *regs) {
    int       fd    = (int)regs->ebx;
    char     *buf   = (char *)(uintptr_t)regs->ecx;
    uint32_t  count = (uint32_t)regs->edx;

    if (!access_ok(buf, count)) return -14;
    if (fd < 0 || fd >= MAX_FD) return -9;

    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type != FD_FILE || !(f->node->flags & VFS_FLAG_DIR)) return -20;

    uint32_t written = 0;
    vfs_dirent_t de;

    for (;;) {
        if (vfs_readdir(f->node, f->offset, &de) < 0) break;
        f->offset++;

        uint32_t nlen   = __builtin_strlen(de.name);
        uint32_t reclen = (19 + nlen + 1 + 7) & ~7U; /* 19B hdr + name + NUL */
        if (written + reclen > count) break;
        if (reclen > 512) return written ? (int)written : -22;

        uint8_t rec[512];
        __builtin_memset(rec, 0, reclen);
        struct linux_dirent64 *d = (struct linux_dirent64 *)rec;
        d->d_ino    = de.ino;
        d->d_off    = (int64_t)(written + reclen);
        d->d_reclen = (uint16_t)reclen;
        d->d_type   = vfs_type_to_dt(de.type);
        __builtin_memcpy(d->d_name, de.name, nlen);
        d->d_name[nlen] = '\0';
        int cr = copy_to_user(buf + written, rec, reclen);
        if (cr < 0) return cr;
        written += reclen;
    }
    {   /* DEBUG: count entries returned for icon dirs (does GTK see the icons?) */
        static int dc = 0;
        if (dc < 30 && dbg_str_has(f->path, "icons") && regs->edx >= 256) {
            dc++;
            int n = 0; vfs_dirent_t dd;
            for (int k = 0; vfs_readdir(f->node, k, &dd) >= 0 && k < 4096; k++) n++;
            printk("[gdents] %s: off=%u wrote=%u total_entries=%d\n",
                   f->path, (unsigned)f->offset, (unsigned)written, n);
        }
    }
    return (int)written;
}

/* ── sys_getcwd(buf, size) — EAX=183 ────────────────────────────────────── */
static int sys_getcwd(registers_t *regs) {
    char    *buf  = (char *)(uintptr_t)regs->ebx;
    uint32_t size = (uint32_t)regs->ecx;

    if (!access_ok(buf, size)) return -14;
    uint32_t clen = __builtin_strlen(current_proc->cwd) + 1;
    if (clen > size) return -34;  /* ERANGE */
    int cr = copy_to_user(buf, current_proc->cwd, clen);
    if (cr < 0) return cr;
    /* The Linux getcwd(2) syscall returns the LENGTH of the filled buffer
     * (including the NUL), not the buffer pointer.  glibc's wrapper checks
     * `retval >= 0`; returning the user buffer address (e.g. a 0xBFFFxxxx stack
     * pointer, high bit set) reads as NEGATIVE → glibc reports getcwd failure,
     * which breaks realpath()/nsLocalFile and made Firefox declare its profile
     * "missing or inaccessible". */
    return (int)clen;
}

/*
 * The mmap region allocator (mmap_next) belongs to the ADDRESS SPACE, not the
 * thread.  Threads of one process (CLONE_VM) share the page directory, so each
 * must allocate from the same cursor — otherwise two threads hand out the same
 * virtual address and corrupt each other's heap.  Route all access through the
 * thread-group leader (pid == tgid); fall back to self if the leader is gone.
 */
static struct proc *mmap_owner(void) {
    if (!current_proc) return NULL;
    struct proc *p = current_proc;
    /* A CLONE_VM child that is NOT a thread (vfork, posix_spawn, a Breakpad
     * dumper clone) shares its creator's address space (and thus the
     * demand-paged VMA list + mmap cursor) while being its own thread group —
     * route it to the owning group so faults on the shared address space
     * resolve, and so its stack mmaps don't get a private (empty) VMA list. */
    if (p->vm_owner) p = p->vm_owner;
    if (p->tgid == p->pid) return p;
    for (int i = 0; i < MAX_PROCS; i++)
        if (ptable[i].state != PROC_UNUSED &&
            ptable[i].pid == p->tgid)
            return &ptable[i];
    return p;
}

/* ── Demand-paged anonymous VMAs ─────────────────────────────────────────────
 * An anonymous mmap records a VMA and returns immediately; the pages are
 * allocated (zeroed) on first fault.  The VMA list belongs to the address space
 * (the tgid leader); all VMA ops run with interrupts off because threads share
 * the list and a fault can occur on any thread. */
/* ── Virtual memory areas ────────────────────────────────────────────────────
 * Every mmap()ed region of an address space is recorded in a VMA, kept sorted
 * by address and owned by the thread-group leader (Linux: mm_struct's VMA
 * tree).  The registry is authoritative for a mapping's existence and its
 * protection; the page tables only cache what has been populated.  It drives
 *   - the free-space search, so munmap()ed ranges are reused (mm/mmap.c,
 *     vm_unmapped_area) and a long-lived process can mmap/munmap forever;
 *   - protection of pages that fault in later, and the SIGSEGV decision for
 *     PROT_NONE ranges (mm/memory.c, access_error);
 *   - re-population after MADV_DONTNEED (a zero page, or the file contents
 *     again for a private file mapping);
 *   - /proc/self/maps.
 * Pages are populated eagerly at mmap time for small mappings and on first
 * touch for large ones; both paths go through vma_populate_page(). */

struct vma {
    uint32_t start;       /* page-aligned, inclusive */
    uint32_t end;         /* page-aligned, exclusive */
    uint32_t prot;        /* raw mmap PROT bits: 1=R 2=W 4=X; 0 = PROT_NONE */
    uint32_t flags;       /* VMA_F_* */
    vfs_node_t *file;     /* NULL = anonymous (zero-fill); else file-backed   */
    uint32_t file_off;    /* byte offset in `file` corresponding to `start`   */
    uint32_t file_size;   /* file size snapshot (bytes past it are BSS-zero)  */
    struct vma *next;     /* next by ascending start */
};

/* PTEs of this VMA carry PAGE_SHARED (MAP_SHARED file/anon, /dev/fb0): they
 * are shared with children instead of COW'd, and MADV_DONTNEED leaves them
 * alone because there is no per-VMA re-population path for shared frames. */
#define VMA_F_SHARED   0x1U

#define PROT_READ_K    0x1
#define PROT_WRITE_K   0x2
#define PROT_EXEC_K    0x4

#define MAP_SHARED_K            0x01
#define MAP_PRIVATE_K           0x02
#define MAP_FIXED_K             0x10
#define MAP_ANONYMOUS_K         0x20
#define MAP_FIXED_NOREPLACE_K   0x100000

/* Free-space search window.  The floor keeps mmap() above the ELF images and
 * the brk heap (the interpreter is loaded at 0x40000000 and is skipped by the
 * present-page scan); the top stays out of the main thread's 8 MiB stack
 * growth window (Linux: mmap_base sits below the stack plus stack_guard_gap;
 * audit M14). */
#define MMAP_FLOOR   0x40000000U
#define MMAP_TOP     ((uint32_t)USER_STACK_TOP - (8U << 20))

/* Small mappings are populated at mmap time; large ones fault in lazily (8 MiB
 * thread stacks and multi-hundred-MiB libraries mostly go untouched). */
#define VMA_DEMAND_MIN       (4U * 1024U * 1024U)
#define VMA_FILE_DEMAND_MIN  (1U * 1024U * 1024U)

static inline uint32_t vma_irq_save(void) {
    uint32_t f; __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory"); return f;
}
static inline void vma_irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* A PTE that owns a frame: present, or PROT_NONE'd (not present, PAGE_PROTNONE
 * marker, frame kept so the data survives an mprotect(PROT_NONE)/mprotect(RW)
 * round trip exactly like Linux's _PAGE_PROTNONE). */
static inline int pte_mapped(uint32_t pte) {
    return (pte & (PAGE_PRESENT | PAGE_PROTNONE)) != 0;
}

/* PTE flag bits for a freshly populated page of a mapping with `prot`. */
static uint32_t pte_flags_for(uint32_t prot, uint32_t shared) {
    uint32_t f = PAGE_USER | (shared ? PAGE_SHARED : 0);
    if (prot == 0) return f | PAGE_PROTNONE;          /* inaccessible, frame kept */
    f |= PAGE_PRESENT;
    if (prot & PROT_WRITE_K) f |= PAGE_WRITABLE;
    return f;
}

static struct vma *vma_alloc(uint32_t start, uint32_t end, uint32_t prot,
                             uint32_t flags, vfs_node_t *file, uint32_t file_off) {
    struct vma *v = (struct vma *)kmalloc(sizeof(struct vma));
    if (!v) return NULL;
    v->start = start; v->end = end; v->prot = prot; v->flags = flags;
    v->file = file; v->file_off = file ? file_off : 0;
    v->file_size = file ? file->size : 0;
    v->next = NULL;
    if (file) vfs_retain(file);          /* keep node alive past fd close */
    return v;
}

static void vma_free_one(struct vma *v) {
    if (v->file) vfs_close(v->file);     /* release node ref */
    kfree(v);
}

/* Two anonymous VMAs with identical attributes that touch can be one VMA
 * (Linux vma_merge).  Keeps the list short under allocators that map many
 * adjacent chunks. */
static int vma_can_merge(const struct vma *a, const struct vma *b) {
    return a->end == b->start && !a->file && !b->file &&
           a->prot == b->prot && a->flags == b->flags;
}

/* Insert v into the owner's sorted list, merging with its neighbours.  Returns
 * the node that now covers v's range (v itself, or the neighbour it was merged
 * into — v is freed in that case). */
static struct vma *vma_insert(struct proc *o, struct vma *v) {
    uint32_t irq = vma_irq_save();
    struct vma **pp = &o->vmas;
    struct vma *prev = NULL;
    while (*pp && (*pp)->start < v->start) { prev = *pp; pp = &(*pp)->next; }
    v->next = *pp;
    *pp = v;
    if (v->end > o->mmap_next) o->mmap_next = v->end;   /* high-water mark */
    /* merge with the successor */
    struct vma *n = v->next;
    if (n && vma_can_merge(v, n)) { v->end = n->end; v->next = n->next; kfree(n); }
    /* merge with the predecessor */
    if (prev && vma_can_merge(prev, v)) {
        prev->end = v->end; prev->next = v->next; kfree(v); v = prev;
    }
    vma_irq_restore(irq);
    return v;
}

/* Merge every pair of adjacent mergeable VMAs (after mprotect re-unified the
 * protection of neighbouring pieces, e.g. a JIT toggling W^X on one region). */
static void vma_merge_all(struct proc *o) {
    uint32_t irq = vma_irq_save();
    for (struct vma *v = o->vmas; v && v->next; ) {
        struct vma *n = v->next;
        if (vma_can_merge(v, n)) { v->end = n->end; v->next = n->next; kfree(n); }
        else v = n;
    }
    vma_irq_restore(irq);
}

/* Record a VMA for [start,end).  Returns the VMA covering it (possibly a
 * merged neighbour spanning more than [start,end)) or NULL. */
static struct vma *vma_add(uint32_t start, uint32_t end, uint32_t prot,
                           uint32_t flags, vfs_node_t *file, uint32_t file_off) {
    struct proc *o = mmap_owner();
    if (!o || end <= start) return NULL;
    struct vma *v = vma_alloc(start, end, prot, flags, file, file_off);
    if (!v) return NULL;
    return vma_insert(o, v);
}

/* Find the VMA containing `addr`, or NULL. */
static struct vma *vma_find(uint32_t addr) {
    struct proc *o = mmap_owner();
    if (!o) return NULL;
    for (struct vma *v = o->vmas; v && v->start <= addr; v = v->next)
        if (addr < v->end) return v;
    return NULL;
}

/* Protection of the VMA covering addr, or -1 if no VMA covers it.  Used by
 * the page-fault handler to refuse a COW break in a read-only mapping. */
int vma_prot_lookup(uint32_t addr) {
    struct vma *v = vma_find(addr & ~0xFFFU);
    return v ? (int)v->prot : -1;
}

/* Expose the VMA list to procfs (struct vma is private here). */
int proc_vma_iter(struct proc *p, int idx,
                  uint32_t *start, uint32_t *end, uint32_t *prot) {
    if (!p) return -1;
    int i = 0;
    for (struct vma *v = p->vmas; v; v = v->next, i++) {
        if (i == idx) {
            if (start) *start = v->start;
            if (end)   *end   = v->end;
            if (prot)  *prot  = v->prot;
            return 0;
        }
    }
    return -1;
}

/* Same, plus the sharing flag and the backing file's name for /proc/pid/maps. */
int proc_vma_iter_ex(struct proc *p, int idx, uint32_t *start, uint32_t *end,
                     uint32_t *prot, int *shared, const char **name) {
    if (!p) return -1;
    int i = 0;
    for (struct vma *v = p->vmas; v; v = v->next, i++) {
        if (i == idx) {
            if (start)  *start  = v->start;
            if (end)    *end    = v->end;
            if (prot)   *prot   = v->prot;
            if (shared) *shared = (v->flags & VMA_F_SHARED) ? 1 : 0;
            if (name)   *name   = v->file ? v->file->name : "";
            return 0;
        }
    }
    return -1;
}

/* Address of the first page in [start,end) whose PTE owns a frame, or 0. */
static uint32_t first_mapped_page(uint32_t start, uint32_t end) {
    for (uint32_t a = start; a < end; ) {
        if (!(*paging_get_pde(a) & PAGE_PRESENT)) {
            uint32_t n = (a & ~0x3FFFFFU) + 0x400000U;   /* next 4 MiB PDE */
            if (n <= a) break;                            /* wrapped */
            a = n;
            continue;
        }
        if (pte_mapped(*paging_get_pte(a))) return a ? a : 1;
        a += PAGE_SIZE;
    }
    return 0;
}

/* True if NOTHING occupies [va, va+length): no VMA overlaps and no page table
 * entry owns a frame (the latter also covers regions that are not VMAs: the
 * ELF image, the brk heap, SysV-style shm attachments, the stack). */
static int vma_range_free(uint32_t va, uint32_t length) {
    uint32_t end = va + length;
    if (end < va) return 0;
    struct proc *o = mmap_owner();
    if (o)
        for (struct vma *v = o->vmas; v && v->start < end; v = v->next)
            if (va < v->end) return 0;
    return first_mapped_page(va, end) == 0;
}

/* First-fit search for a free range of `length` bytes whose start is a
 * multiple of `align` inside [MMAP_FLOOR, MMAP_TOP).  Gaps between VMAs are
 * verified against the page tables so non-VMA occupants are skipped too.
 * Returns 0 when nothing fits (-ENOMEM).  Linux searches top-down from
 * mmap_base; bottom-up first-fit gives the same reuse guarantee and keeps
 * the layout this kernel's users already expect (libraries low, stacks high). */
static uint32_t vma_gap_find(uint32_t length, uint32_t align) {
    struct proc *o = mmap_owner();
    if (!o || !length) return 0;
    if (align < PAGE_SIZE) align = PAGE_SIZE;
    uint32_t cur = MMAP_FLOOR;
    for (int guard = 0; guard < 100000; guard++) {
        /* find the first gap at or after cur that fits */
        uint32_t cand = 0;
        uint32_t lo = cur;
        struct vma *v = o->vmas;
        for (;;) {
            uint32_t a = (lo + align - 1) & ~(align - 1);
            if (a < lo || a + length < a || a + length > MMAP_TOP) return 0;
            while (v && v->end <= lo) v = v->next;
            if (!v || a + length <= v->start) { cand = a; break; }
            lo = v->end;
        }
        /* the gap is VMA-free; make sure no stray page table entry lives there */
        uint32_t occ = first_mapped_page(cand, cand + length);
        if (!occ) return cand;
        cur = occ + PAGE_SIZE;
    }
    return 0;
}

/* Remove (and trim/split) any VMA coverage of [start,end) from the owner.
 * Order is preserved; a split keeps the right piece right after the left. */
static void vma_remove_range(uint32_t start, uint32_t end) {
    struct proc *o = mmap_owner();
    if (!o || end <= start) return;
    uint32_t irq = vma_irq_save();
    struct vma **pp = &o->vmas;
    while (*pp) {
        struct vma *v = *pp;
        if (v->start >= end) break;                         /* sorted: done */
        if (v->end <= start) { pp = &v->next; continue; }
        /* overlap */
        if (v->start >= start && v->end <= end) {           /* fully covered: drop */
            *pp = v->next;
            vma_irq_restore(irq);
            vma_free_one(v);
            irq = vma_irq_save();
            continue;
        }
        if (v->start < start && v->end > end) {             /* split into two */
            struct vma *right = vma_alloc(end, v->end, v->prot, v->flags, v->file,
                                          v->file ? v->file_off + (end - v->start) : 0);
            if (right) {
                right->file_size = v->file_size;
                right->next = v->next;
                v->next = right;
            }
            v->end = start;
            pp = &v->next;
            continue;
        }
        if (v->start < start) { v->end = start; }           /* trim right edge */
        else {                                              /* trim left edge */
            if (v->file) v->file_off += end - v->start;
            v->start = end;
        }
        pp = &v->next;
    }
    vma_irq_restore(irq);
}

/* Update prot of VMA coverage over [start,end), splitting as needed while
 * keeping the list sorted (Linux mprotect_fixup / split_vma). */
static void vma_protect_range(uint32_t start, uint32_t end, uint32_t prot) {
    struct proc *o = mmap_owner();
    if (!o || end <= start) return;
    uint32_t irq = vma_irq_save();
    for (struct vma *v = o->vmas; v && v->start < end; v = v->next) {
        if (v->end <= start || v->prot == prot) continue;
        uint32_t cs = v->start > start ? v->start : start;
        uint32_t ce = v->end   < end   ? v->end   : end;
        if (v->start < cs) {
            /* keep [v->start,cs) as v; carve [cs,v->end) as mid and continue
             * with mid so the tail split below applies to it */
            struct vma *mid = vma_alloc(cs, v->end, v->prot, v->flags, v->file,
                                        v->file ? v->file_off + (cs - v->start) : 0);
            if (!mid) { v->prot = prot; continue; }   /* fallback: prot whole VMA */
            mid->file_size = v->file_size;
            mid->next = v->next;
            v->next = mid;
            v->end = cs;
            v = mid;
        }
        /* v starts at cs; if it runs past ce split the tail off with the old prot */
        if (ce < v->end) {
            struct vma *right = vma_alloc(ce, v->end, v->prot, v->flags, v->file,
                                          v->file ? v->file_off + (ce - v->start) : 0);
            if (right) {
                right->file_size = v->file_size;
                right->next = v->next;
                v->next = right;
                v->end = ce;
            }
        }
        v->prot = prot;
    }
    vma_irq_restore(irq);
    vma_merge_all(o);
}

/* Free all VMAs of a process (exec / last-thread exit). */
void vma_clear(struct proc *p) {
    if (!p) return;
    uint32_t irq = vma_irq_save();
    struct vma *v = p->vmas;
    p->vmas = NULL;
    vma_irq_restore(irq);
    while (v) { struct vma *n = v->next; vma_free_one(v); v = n; }
}

/* Copy parent's VMA list to child (fork), preserving order. */
void vma_clone(struct proc *parent, struct proc *child) {
    child->vmas = NULL;
    if (!parent) return;
    /* VMAs live on the thread-group LEADER (mmap_owner), not on a non-leader
     * worker thread.  When a worker forks (e.g. Firefox's content-process
     * launch happens on the "IPC Launch" thread, not the main thread), copy the
     * LEADER's list — otherwise the child has no coverage and SIGSEGVs on the
     * first libxul / thread-stack page it touches. */
    struct proc *owner = parent;
    if (parent->tgid != parent->pid)
        for (int i = 0; i < MAX_PROCS; i++)
            if (ptable[i].state != PROC_UNUSED && ptable[i].pid == parent->tgid) {
                owner = &ptable[i]; break;
            }
    struct vma **tail = &child->vmas;
    for (struct vma *v = owner->vmas; v; v = v->next) {
        struct vma *nv = vma_alloc(v->start, v->end, v->prot, v->flags, v->file, v->file_off);
        if (!nv) return;
        nv->file_size = v->file_size;
        *tail = nv;
        tail = &nv->next;
    }
}

/* ── Shared file mappings (MAP_SHARED) ───────────────────────────────────────
 * Firefox's IPC (SharedStringMap = the shared preferences map, and base shared
 * memory) creates a memfd, maps it MAP_SHARED read-write, writes data, then maps
 * the SAME memfd MAP_SHARED again (read-only, and in content processes) and
 * reads it back.  This REQUIRES all mappings of a memfd to share the same
 * physical frames.  The old mmap copied file contents into private frames, so
 * the second mapping saw an empty copy → SharedStringMap dereferenced a NULL/
 * zero pointer and crashed (caught by Firefox, but it then quits with no GUI).
 *
 * We keep a registry keyed by the file's vfs_node: a lazily-grown array of one
 * physical frame per file page, shared (refcounted) by every MAP_SHARED mapper. */
#define SHMAP_MAX 256   /* headroom for simultaneously-live memfds (each content
                         * process maps several); dead ones are reclaimed on
                         * demand once no fd/mapping references them. */
static struct shmap_entry {
    vfs_node_t *node;      /* keyed by vfs node (memfd/tmpfs file) */
    uint32_t   *frames;    /* phys frame per page; 0 = not yet allocated */
    uint32_t    npages;    /* length of frames[] */
} shmaps[SHMAP_MAX];

/* Return the existing shmap entry for a node, or NULL (no allocation). */
static struct shmap_entry *shmap_lookup(vfs_node_t *node) {
    for (int i = 0; i < SHMAP_MAX; i++)
        if (shmaps[i].node == node) return &shmaps[i];
    return NULL;
}
/* Return the shared phys frame backing page `pg`, or 0 if not allocated. */
static uint32_t shmap_peek(struct shmap_entry *e, uint32_t pg) {
    if (!e || pg >= e->npages) return 0;
    return e->frames[pg];
}

/* Reclaim a shmap slot IFF no process still maps any of its frames.  Each frame
 * carries one "registry" ref (taken in shmap_frame); a frame whose refcount is
 * <=1 is mapped by nobody else, so the whole entry is dead once ALL its frames
 * are <=1.  This is how leaked memfd/tmpfs shared maps get freed: our memfd is a
 * named /tmp/.memfd-N tmpfs file that persists (no delete-on-close), so without
 * this the 128-slot table filled across watchdog restarts → shmap_get()==NULL →
 * mmap(MAP_SHARED) failed → Firefox MOZ_RELEASE_ASSERT(mMap.initialized()). */
/* True iff any process still holds an open fd to this node.  A memfd whose fd is
 * open must keep its data even while unmapped (POSIX memfd/shm persistence), so
 * such an entry is NOT dead and must never be reclaimed — freeing its frames
 * would silently destroy live shared data (scattered NULL-object crashes when a
 * later read/map sees zeroed/stale content). */
static int shmap_node_has_open_fd(vfs_node_t *node) {
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_UNUSED || !p->ofile) continue;
        for (int f = 0; f < MAX_FD; f++)
            if (p->ofile[f].type == FD_FILE && p->ofile[f].node == node)
                return 1;
    }
    return 0;
}

static int shmap_try_reclaim(struct shmap_entry *e) {
    if (shmap_node_has_open_fd(e->node)) return 0;   /* live fd → data must persist */
    for (uint32_t p = 0; p < e->npages; p++)
        if (e->frames && e->frames[p] && pmm_frame_refcount(e->frames[p]) > 1)
            return 0;   /* still mapped by a live process — keep it */
    for (uint32_t p = 0; p < e->npages; p++)
        if (e->frames && e->frames[p]) pmm_frame_decref(e->frames[p]);
    if (e->frames) kfree(e->frames);
    e->frames = NULL; e->npages = 0; e->node = NULL;
    return 1;
}

static struct shmap_entry *shmap_get(vfs_node_t *node) {
    struct shmap_entry *free_e = NULL;
    for (int i = 0; i < SHMAP_MAX; i++) {
        if (shmaps[i].node == node) return &shmaps[i];
        if (!shmaps[i].node && !free_e) free_e = &shmaps[i];
    }
    if (!free_e) {                       /* table full — reclaim dead entries */
        for (int i = 0; i < SHMAP_MAX; i++)
            if (shmaps[i].node && shmap_try_reclaim(&shmaps[i])) { free_e = &shmaps[i]; break; }
        if (!free_e) return NULL;
    }
    free_e->node   = node;
    free_e->frames = NULL;
    free_e->npages = 0;
    return free_e;
}

/* memfd nodes store their data IN this registry (see shmem_read below), so
 * there is no separate file body to seed a fresh frame from. */
static uint32_t shmem_read(vfs_node_t *, uint32_t, uint32_t, uint8_t *);
static int node_is_shmem(vfs_node_t *n) { return n && n->read_fn == shmem_read; }

/* Get (allocating + initialising from file content on first touch) the shared
 * physical frame backing page `pg` of the file. */
static uint32_t shmap_frame(struct shmap_entry *e, vfs_node_t *node, uint32_t pg) {
    if (pg >= e->npages) {
        /* Double, never grow by a constant: a 64 MiB memfd is 16384 pages and
         * a +16 step would recopy the table on every page (O(n^2)). */
        uint32_t newn = e->npages ? e->npages * 2 : 16;
        if (newn < pg + 16) newn = pg + 16;
        uint32_t *nf = (uint32_t *)kmalloc(newn * sizeof(uint32_t));
        if (!nf) return 0;
        __builtin_memset(nf, 0, newn * sizeof(uint32_t));
        if (e->frames) {
            __builtin_memcpy(nf, e->frames, e->npages * sizeof(uint32_t));
            kfree(e->frames);
        }
        e->frames = nf;
        e->npages = newn;
    }
    if (e->frames[pg]) return e->frames[pg];
    uint32_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    pmm_frame_incref(phys);                  /* registry holds one ref */
    uint8_t *kp = (uint8_t *)paging_temp_map(phys);
    __builtin_memset(kp, 0, PAGE_SIZE);
    if (node->read_fn && !node_is_shmem(node))  /* preserve any pre-written content */
        vfs_read(node, pg * PAGE_SIZE, PAGE_SIZE, kp);
    paging_temp_unmap();
    e->frames[pg] = phys;
    return phys;
}

/* ── memfd (shmem) file backing ──────────────────────────────────────────────
 * Linux: a memfd is a shmem inode whose page-cache pages ARE the pages a
 * MAP_SHARED mapping maps (mm/memfd.c, mm/shmem.c).  read(2)/write(2) and
 * stores through a mapping therefore hit the same frames, and the file costs
 * one frame per touched page instead of a second contiguous copy of it.
 * Installing these three operations on a memfd node makes the shmap registry
 * above that file's only storage, which is what makes the two views coherent
 * (audit M5).
 *
 * They use the SECOND temp-map slot: the page-population path holds slot 1
 * across its fill, and a private mapping of a memfd page can reach vfs_read()
 * from inside that. */

/* Move `n` bytes between file page frame `phys` (at byte `in` within it) and
 * the caller's buffer, through a small bounce buffer.  The caller's buffer is
 * often a demand-paged USER page: faulting it in populates its frame through
 * the same temp-map slots, so the slot must not be held across that touch. */
static void shmem_bounce(uint32_t phys, uint32_t in, uint8_t *buf, uint32_t n,
                         int to_frame) {
    uint8_t tmp[256];
    for (uint32_t done = 0; done < n; ) {
        uint32_t k = n - done;
        if (k > sizeof tmp) k = sizeof tmp;
        if (to_frame) __builtin_memcpy(tmp, buf + done, k);
        preempt_disable();
        uint8_t *kp = (uint8_t *)paging_temp_map2(phys);
        if (to_frame) __builtin_memcpy(kp + in + done, tmp, k);
        else          __builtin_memcpy(tmp, kp + in + done, k);
        paging_temp_unmap2();
        preempt_enable();
        if (!to_frame) __builtin_memcpy(buf + done, tmp, k);
        done += k;
    }
}

static uint32_t shmem_read(vfs_node_t *node, uint32_t off, uint32_t len,
                           uint8_t *buf) {
    if (off >= node->size) return 0;
    if (len > node->size - off) len = node->size - off;
    struct shmap_entry *e = shmap_lookup(node);
    uint32_t done = 0;
    while (done < len) {
        uint32_t pos = off + done;
        uint32_t in  = pos & (PAGE_SIZE - 1);
        uint32_t n   = PAGE_SIZE - in;
        if (n > len - done) n = len - done;
        uint32_t phys = shmap_peek(e, pos / PAGE_SIZE);
        if (phys) shmem_bounce(phys, in, buf + done, n, 0);
        else      __builtin_memset(buf + done, 0, n);   /* unwritten page = hole */
        done += n;
    }
    return done;
}

static uint32_t shmem_write(vfs_node_t *node, uint32_t off, uint32_t len,
                            const uint8_t *buf) {
    struct shmap_entry *e = shmap_get(node);
    if (!e) return 0;
    uint32_t done = 0;
    while (done < len) {
        uint32_t pos = off + done;
        uint32_t in  = pos & (PAGE_SIZE - 1);
        uint32_t n   = PAGE_SIZE - in;
        if (n > len - done) n = len - done;
        uint32_t phys = shmap_frame(e, node, pos / PAGE_SIZE);
        if (!phys) break;                          /* out of frames */
        shmem_bounce(phys, in, (uint8_t *)(uintptr_t)(buf + done), n, 1);
        done += n;
    }
    if (off + done > node->size) node->size = off + done;
    return done;
}

/* Linux shmem_setattr/shmem_truncate_range: growing is lazy (the new pages are
 * holes that read as zero), shrinking drops the pages past the new end and
 * zeroes the tail of the last partial one. */
static int shmem_truncate(vfs_node_t *node, uint32_t new_size) {
    struct shmap_entry *e = shmap_lookup(node);
    if (e && e->frames && new_size < node->size) {
        uint32_t tail = new_size & (PAGE_SIZE - 1);
        if (tail) {
            uint32_t phys = shmap_peek(e, new_size / PAGE_SIZE);
            if (phys) {
                preempt_disable();
                uint8_t *kp = (uint8_t *)paging_temp_map2(phys);
                __builtin_memset(kp + tail, 0, PAGE_SIZE - tail);
                paging_temp_unmap2();
                preempt_enable();
            }
        }
        for (uint32_t pg = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;
             pg < e->npages; pg++)
            if (e->frames[pg]) {
                pmm_frame_decref(e->frames[pg]);   /* drop the registry ref */
                e->frames[pg] = 0;
            }
    }
    node->size = new_size;
    return 0;
}

/* ── Page population ─────────────────────────────────────────────────────────
 * Map one page of a PRIVATE VMA: a zeroed frame, filled from the backing file
 * (or from the file's shared frames if it also has MAP_SHARED mappers, so a
 * private read-only view of a memfd sees the data written through the shared
 * view).  Returns 1 if the page is mapped afterwards, 0 on OOM.
 *
 * FILL-BEFORE-MAP (SMP correctness): fill the new frame through a PRIVATE
 * temporary kernel mapping and only AFTER it is complete map it at the user VA.
 * Publishing the PTE first and filling through the user address races a sibling
 * thread on another CPU (it can read a half-filled page).  preempt_disable keeps
 * the shared temp-map slot ours across the BKL-held, non-sleeping vfs_read. */
static int vma_populate_page(struct vma *v, uint32_t addr) {
    addr &= ~0xFFFU;
    if ((*paging_get_pde(addr) & PAGE_PRESENT) &&
        pte_mapped(*paging_get_pte(addr))) return 1;     /* already there */
    uint32_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    pmm_frame_incref(phys);

    preempt_disable();
    uint8_t *kva = (uint8_t *)paging_temp_map(phys);
    __builtin_memset(kva, 0, PAGE_SIZE);
    if (v->file) {
        uint32_t foff = v->file_off + (addr - v->start);
        struct shmap_entry *se = shmap_lookup(v->file);
        uint32_t sf = se ? shmap_peek(se, foff / PAGE_SIZE) : 0;
        if (sf) {
            /* copy from the shared frame (temp slot 2), not the stale tmpfs buffer */
            const uint8_t *src = (const uint8_t *)paging_temp_map2(sf);
            __builtin_memcpy(kva, src, PAGE_SIZE);
            paging_temp_unmap2();
        } else if (foff < v->file_size) {
            uint32_t want = PAGE_SIZE;
            if (want > v->file_size - foff) want = v->file_size - foff;
            vfs_read(v->file, foff, want, kva);
        }
    }
    paging_temp_unmap();
    preempt_enable();

    paging_map(addr, phys, pte_flags_for(v->prot, 0));     /* publish: complete */
    return 1;
}

/* Page-fault populate.  Returns 1 if handled, 0 if the address isn't a
 * populatable VMA page (PROT_NONE, shared mapping, no VMA, OOM → SIGSEGV). */
int vma_handle_fault(uint32_t addr) {
    addr &= ~0xFFFU;
    struct vma *v = vma_find(addr);
    if (!v) return 0;
    if (v->prot == 0) return 0;                 /* PROT_NONE guard → SIGSEGV */
    if (v->flags & VMA_F_SHARED) return 0;      /* shared frames are eager-only */
    return vma_populate_page(v, addr);
}

/* Populate [start,end) of a private VMA now (small mappings).  1 on success. */
static int vma_populate_range(struct vma *v, uint32_t start, uint32_t end) {
    if (v->prot == 0) return 1;                 /* nothing to map for PROT_NONE */
    for (uint32_t a = start; a < end; a += PAGE_SIZE)
        if (!vma_populate_page(v, a)) return 0;
    return 1;
}

/* Unmap every populated page in [start,end) and release the frames.
 *
 * FLUSH-BEFORE-FREE (Linux mmu_gather rule): clear the PTEs first, then
 * tlb_shootdown(), and only AFTER the shootdown release the frames.  Freeing a
 * frame before the shootdown is a use-after-free: a sibling thread on another
 * CPU may still hold a stale TLB entry for the page and would read/write the
 * frame after it has been reclaimed and handed to another allocation.  Batched
 * so an arbitrarily large range uses bounded stack. */
static void unmap_pages(uint32_t start, uint32_t end) {
    uint32_t batch[256];
    int nb = 0;
    for (uint32_t va = start; va < end; ) {
        if (!(*paging_get_pde(va) & PAGE_PRESENT)) {
            uint32_t n = (va & ~0x3FFFFFU) + 0x400000U;
            if (n <= va) break;
            va = n;
            continue;
        }
        uint32_t *pte = paging_get_pte(va);
        if (pte_mapped(*pte)) {
            batch[nb++] = *pte & ~0xFFFU;       /* remember frame; free AFTER flush */
            *pte = 0;
            tlb_flush_single(va);
            if (nb == 256) {
                tlb_shootdown();                /* no CPU keeps a stale entry now */
                for (int i = 0; i < nb; i++) pmm_frame_decref(batch[i]);
                nb = 0;
            }
        }
        va += PAGE_SIZE;
    }
    tlb_shootdown();                            /* flush the remainder before freeing */
    for (int i = 0; i < nb; i++) pmm_frame_decref(batch[i]);
}

/* Tear down whatever occupies [start,end): VMAs and pages.  MAP_FIXED overlay
 * and munmap share this (Linux do_munmap). */
static void unmap_range(uint32_t start, uint32_t end) {
    vma_remove_range(start, end);
    unmap_pages(start, end);
}

/* ── sys_mmap2(addr,len,prot,flags,fd,pgoffset) — EAX=192 ──────────────── */
static int sys_mmap2(registers_t *regs) {
    uint32_t addr   = regs->ebx;
    uint32_t length = regs->ecx;
    uint32_t prot   = regs->edx & 0x7;           /* PROT_SEM/GROWS* ignored */
    int flags  = (int)regs->esi;
    int fd     = (int)regs->edi;
    /* mmap2's 6th arg (offset in PAGES) is passed in EBP on i386, which the
     * int-0x80 stub captures via pusha — essential for loading a shared
     * library's data segment, which sits at a nonzero file offset. */
    uint32_t pgoff = regs->ebp;

    if (length == 0) return -22;
    if (length > 0xC0000000U) return -12;
    length = (length + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);

    struct proc *owner = mmap_owner();
    if (!owner) return -12;
    int anon    = (flags & MAP_ANONYMOUS_K) != 0;
    int shared  = (flags & MAP_SHARED_K) != 0;
    int fixed   = (flags & (MAP_FIXED_K | MAP_FIXED_NOREPLACE_K)) != 0;

    /* File-backed: validate the descriptor first (EBADF before any layout work). */
    vfs_node_t *fnode = NULL;
    if (!anon) {
        if (fd < 0 || fd >= MAX_FD || current_proc->ofile[fd].type != FD_FILE ||
            !current_proc->ofile[fd].node)
            return -9;
        fnode = current_proc->ofile[fd].node;
    }

    /* ── Choose the target VA ──
     * MAP_FIXED places exactly at `addr`, replacing whatever is there (ld.so
     * maps each library LOAD segment over a reserved span; mozjemalloc commits/
     * decommits with MAP_FIXED).  MAP_FIXED_NOREPLACE (Linux 4.17+) places at
     * `addr` only if the range is free and fails with EEXIST otherwise.  A plain
     * hint is honoured if that range is entirely free, else ignored (Linux
     * get_unmapped_area).  Everything else goes through the first-fit gap
     * search, which reuses munmap()ed space. */
    uint32_t va;
    if (fixed) {
        if (addr & (PAGE_SIZE - 1)) return -22;
        if (addr + length < addr || addr + length > (uint32_t)USER_STACK_BASE) return -12;
        va = addr;
        if (flags & MAP_FIXED_NOREPLACE_K) {
            if (!vma_range_free(va, length)) return -17;   /* -EEXIST */
        } else {
            unmap_range(va, va + length);                 /* replace */
        }
    } else {
        uint32_t hint = addr & ~(uint32_t)(PAGE_SIZE - 1);
        if (hint && hint >= MMAP_FLOOR && hint + length > hint &&
            hint + length <= MMAP_TOP && vma_range_free(hint, length)) {
            va = hint;
        } else {
            /* Align large anonymous mappings (≥1 MiB) to a 1 MiB boundary.
             * SpiderMonkey's GC allocates 1 MiB chunks and locates a cell's
             * chunk header by masking the pointer to 1 MiB; it relies on the OS
             * page allocator returning suitably-aligned chunks (real Linux's
             * layout happens to satisfy this). */
            uint32_t align = (anon && length >= 0x100000U) ? 0x100000U : PAGE_SIZE;
            va = vma_gap_find(length, align);
            if (!va && align != PAGE_SIZE) va = vma_gap_find(length, PAGE_SIZE);
            if (!va) return -12;
        }
    }
    uint32_t end = va + length;

    /* ── /dev/fb0: map the real framebuffer (DOOM, links -g) ── */
    if (fnode && __builtin_strcmp(fnode->name, "fb0") == 0) {
        uint32_t fb_phys = framebuffer_phys();
        uint32_t fb_len = framebuffer_size();
        if (!fb_phys) return -19;
        if (length > ((fb_len + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1)))
            length = (fb_len + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
        end = va + length;
        if (!vma_add(va, end, prot, VMA_F_SHARED, NULL, 0)) return -12;
        for (uint32_t i = 0; i < length; i += PAGE_SIZE)
            paging_map(va + i, fb_phys + i,
                       PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
                       PAGE_SHARED | (1U << 4));   /* SHARED: no COW on fork; PCD */
        return (int)va;
    }

    /* ── MAP_SHARED on a real file: share physical frames ──
     * memfd/tmpfs shared memory — all mappers of the same node see the same
     * frames (Firefox IPC / SharedStringMap depend on this).  Mapped writable
     * iff PROT_WRITE; PAGE_SHARED so fork shares rather than COWs. */
    if (shared && fnode) {
        struct shmap_entry *e = shmap_get(fnode);
        if (!e) return -12;
        struct vma *v = vma_add(va, end, prot, VMA_F_SHARED, NULL, 0);
        if (!v) return -12;
        for (uint32_t i = 0; i < length; i += PAGE_SIZE) {
            uint32_t phys = shmap_frame(e, fnode, pgoff + i / PAGE_SIZE);
            if (!phys) { unmap_range(va, end); return -12; }
            pmm_frame_incref(phys);          /* this mapping's ref */
            paging_map(va + i, phys, pte_flags_for(prot, 1));
        }
        return (int)va;
    }

    /* ── MAP_SHARED | MAP_ANONYMOUS: zero-filled frames shared across fork ──
     * Linux backs these with shmem (shmem_zero_setup): the pages are one copy
     * that parent and child both see.  PAGE_SHARED makes fork share the frame
     * instead of COW'ing it; the frames are populated now because a shared
     * page has no per-process re-population path. */
    if (shared && anon) {
        struct vma *v = vma_add(va, end, prot, VMA_F_SHARED, NULL, 0);
        if (!v) return -12;
        for (uint32_t i = 0; i < length; i += PAGE_SIZE) {
            uint32_t phys = pmm_alloc_frame();
            if (!phys) { unmap_range(va, end); return -12; }
            pmm_frame_incref(phys);
            preempt_disable();
            __builtin_memset(paging_temp_map(phys), 0, PAGE_SIZE);
            paging_temp_unmap();
            preempt_enable();
            paging_map(va + i, phys, pte_flags_for(prot, 1));
        }
        return (int)va;
    }

    /* ── MAP_PRIVATE anonymous ──
     * Record the VMA; prot is honoured for every page (Linux: PROT_NONE ranges
     * are reservations that fault on any access, read-only ranges fault on
     * write).  Large regions fault in lazily; small ones are populated now. */
    if (anon) {
        struct vma *v = vma_add(va, end, prot, 0, NULL, 0);
        if (!v) return -12;
        if (length < VMA_DEMAND_MIN && !vma_populate_range(v, va, end)) {
            unmap_range(va, end);
            return -12;
        }
        return (int)va;
    }

    /* ── MAP_PRIVATE file-backed ──
     * Pages fault in from the file (copy-on-fault: the frame is private, so
     * writes never reach the file).  Eagerly copying a big library (libxul.so
     * is ~175 MiB) would read the whole file through the slow ATA-PIO path even
     * though startup touches a fraction of it, so large mappings are lazy. */
    {
        struct vma *v = vma_add(va, end, prot, 0, fnode, pgoff * PAGE_SIZE);
        if (!v) return -12;
        if (fnode->size > 100u * 1024u * 1024u && pgoff == 0) {
            static int logged_big = 0;
            if (!logged_big) { logged_big = 1;
                printk("[gdbaid] big mmap base=0x%08x size=%u fd=%d pid=%d\n",
                       (unsigned)va, (unsigned)fnode->size, fd, current_proc->pid);
            }
        }
        if (length < VMA_FILE_DEMAND_MIN && !vma_populate_range(v, va, end)) {
            unmap_range(va, end);
            return -12;
        }
        return (int)va;
    }
}

/* ── sys_mmap(struct mmap_arg*) — EAX=90 (old i386 variant) ─────────────── */
static int sys_mmap_old(registers_t *regs) {
    struct { uint32_t addr, len, prot, flags, fd, offset; } a;
    if (copy_from_user(&a, (void *)(uintptr_t)regs->ebx, sizeof(a)) < 0)
        return -14;
    {
        registers_t fake = *regs;
        fake.ebx = a.addr;
        fake.ecx = a.len;
        fake.edx = a.prot;
        fake.esi = a.flags;
        fake.edi = a.fd;
        /* byte offset → page offset (mmap2 semantics); must be aligned */
        if (a.offset & (PAGE_SIZE - 1)) return -22;
        fake.ebp = a.offset / PAGE_SIZE;
        return sys_mmap2(&fake);
    }
}

/* ── sys_munmap(addr, len) — EAX=91 ─────────────────────────────────────── */
static int sys_munmap(registers_t *regs) {
    uint32_t addr = regs->ebx;
    uint32_t len  = regs->ecx;
    if (addr & (PAGE_SIZE - 1)) return -22;          /* Linux: EINVAL */
    if (len == 0) return -22;
    len = (len + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
    if (addr + len < addr || addr + len > 0xC0000000U) return -22;
    /* Unmapping a range with no mapping is not an error (Linux). */
    unmap_range(addr, addr + len);
    return 0;
}

/* Apply `prot` to the page table entry of one populated user page.  Linux
 * change_protection(): PROT_NONE turns the entry into a not-present
 * _PAGE_PROTNONE entry that keeps its frame; PROT_WRITE is granted only to
 * pages that are not copy-on-write — a COW page stays read-only and the write
 * fault copies it (or reuses it when it is the last reference). */
static void pte_apply_prot(uint32_t va, uint32_t prot) {
    if (!(*paging_get_pde(va) & PAGE_PRESENT)) return;      /* not populated */
    uint32_t *pte = paging_get_pte(va);
    uint32_t old = *pte;
    if (!pte_mapped(old) || !(old & PAGE_USER)) return;
    uint32_t nw = old & ~(uint32_t)(PAGE_PRESENT | PAGE_WRITABLE | PAGE_PROTNONE);
    if (prot == 0) {
        nw |= PAGE_PROTNONE;
    } else {
        nw |= PAGE_PRESENT;
        if ((prot & PROT_WRITE_K) && !(old & PAGE_COW)) nw |= PAGE_WRITABLE;
    }
    if (nw != old) {
        *pte = nw;
        tlb_flush_single(va);
    }
}

/* ── sys_mprotect(addr, len, prot) — EAX=125 ────────────────────────────── */
static int sys_mprotect(registers_t *regs) {
    uint32_t addr = regs->ebx;
    uint32_t len  = regs->ecx;
    uint32_t prot = regs->edx;

    if (addr & (PAGE_SIZE - 1)) return -22;
    if (len == 0) return 0;
    if (prot & ~(0x7U | 0x01000000U | 0x02000000U)) return -22;   /* GROWSDOWN/UP tolerated */
    prot &= 0x7U;
    uint32_t end = addr + ((len + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1));
    if (end < addr || end > 0xC0000000U) return -22;

    /* Record the new prot on the VMAs covering this range, so pages that fault
     * in LATER get the right permissions.  Gaps are tolerated (Linux returns
     * ENOMEM): ld.so's RELRO mprotect targets the executable image, which is
     * not a VMA here, and glibc treats that failure as fatal. */
    vma_protect_range(addr, end, prot);

    for (uint32_t va = addr; va < end; ) {
        if (!(*paging_get_pde(va) & PAGE_PRESENT)) {
            uint32_t n = (va & ~0x3FFFFFU) + 0x400000U;
            if (n <= va) break;
            va = n;
            continue;
        }
        pte_apply_prot(va, prot);
        va += PAGE_SIZE;
    }
    /* SMP: permission reductions must be seen by sibling threads on other CPUs
     * before they next touch the page (W^X / JIT correctness). */
    tlb_shootdown();
    return 0;
}

/* ── sys_madvise(addr, len, advice) — EAX=219 ────────────────────────────── */
#define MADV_DONTNEED_K 4
#define MADV_FREE_K     8
static int sys_madvise(registers_t *regs) {
    uint32_t addr = regs->ebx;
    uint32_t len  = regs->ecx;
    int advice = (int)regs->edx;
    if (addr & (PAGE_SIZE - 1)) return -22;
    if (advice < 0 || advice > 25) return -22;
    if (len == 0) return 0;
    uint32_t end = (addr + len + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
    if (end < addr || end > 0xC0000000U) return -22;

    /* Linux returns ENOMEM if the range has any unmapped gap.  We approximate:
     * an entirely unmapped range is -ENOMEM, a partially mapped one is advised. */
    if (vma_range_free(addr, end - addr)) return -12;

    if (advice != MADV_DONTNEED_K) return 0;    /* MADV_FREE and hints: no-op.
                                                 * MADV_FREE is lazy on Linux: the
                                                 * data persists until reclaim,
                                                 * and we never reclaim. */

    /* MADV_DONTNEED zaps the range (mm/madvise.c zap_page_range_single): the
     * next touch of a private page gives a fresh zero page (or the file page
     * again); the other side of a COW share keeps its copy because we only
     * drop THIS address space's reference.  Pages we cannot re-populate stay
     * mapped: shared frames (no per-VMA population path), and pages outside
     * any VMA (brk heap, stack) which are zeroed in place instead — for a COW
     * page in place means a private zero frame, so the sharer is untouched. */
    uint32_t batch[256];
    int nb = 0;
    for (uint32_t va = addr; va < end; ) {
        if (!(*paging_get_pde(va) & PAGE_PRESENT)) {
            uint32_t n = (va & ~0x3FFFFFU) + 0x400000U;
            if (n <= va) break;
            va = n;
            continue;
        }
        uint32_t *pte = paging_get_pte(va);
        uint32_t old = *pte;
        if (pte_mapped(old) && (old & PAGE_USER) && !(old & PAGE_SHARED)) {
            uint32_t frame = old & ~0xFFFU;
            if (vma_find(va)) {
                batch[nb++] = frame;             /* free AFTER the shootdown */
                *pte = 0;
                tlb_flush_single(va);
                if (nb == 256) {
                    tlb_shootdown();
                    for (int i = 0; i < nb; i++) pmm_frame_decref(batch[i]);
                    nb = 0;
                }
            } else if (old & PAGE_PRESENT) {
                if (pmm_frame_refcount(frame) > 1) {
                    /* COW-shared: give this side a private zero frame */
                    uint32_t nf = pmm_alloc_frame();
                    if (nf) {
                        pmm_frame_incref(nf);
                        preempt_disable();
                        __builtin_memset(paging_temp_map(nf), 0, PAGE_SIZE);
                        paging_temp_unmap();
                        preempt_enable();
                        *pte = nf | (old & 0xFFFU & ~(uint32_t)PAGE_COW) | PAGE_WRITABLE;
                        tlb_flush_single(va);
                        batch[nb++] = frame;
                        if (nb == 256) {
                            tlb_shootdown();
                            for (int i = 0; i < nb; i++) pmm_frame_decref(batch[i]);
                            nb = 0;
                        }
                    }
                } else {
                    preempt_disable();
                    __builtin_memset(paging_temp_map(frame), 0, PAGE_SIZE);
                    paging_temp_unmap();
                    preempt_enable();
                }
            }
        }
        va += PAGE_SIZE;
    }
    tlb_shootdown();
    for (int i = 0; i < nb; i++) pmm_frame_decref(batch[i]);
    return 0;
}

/* ── sys_mincore(addr, len, vec) — EAX=218 ──────────────────────────────── */
static int sys_mincore(registers_t *regs) {
    uint32_t addr = regs->ebx;
    uint32_t len  = regs->ecx;
    uint8_t *vec  = (uint8_t *)(uintptr_t)regs->edx;
    if (addr & (PAGE_SIZE - 1)) return -22;
    uint32_t end = (addr + len + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
    if (end < addr || end > 0xC0000000U) return -12;
    uint32_t npages = (end - addr) / PAGE_SIZE;
    if (!access_ok(vec, npages)) return -14;
    uint8_t buf[256];
    uint32_t done = 0;
    while (done < npages) {
        uint32_t n = npages - done;
        if (n > sizeof(buf)) n = sizeof(buf);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t va = addr + (done + i) * PAGE_SIZE;
            int mapped = (*paging_get_pde(va) & PAGE_PRESENT) &&
                         pte_mapped(*paging_get_pte(va));
            /* Linux: ENOMEM if a page is in no mapping at all. */
            if (!mapped && !vma_find(va)) return -12;
            buf[i] = mapped ? 1 : 0;              /* resident */
        }
        if (copy_to_user(vec + done, buf, n) < 0) return -14;
        done += n;
    }
    return 0;
}

/* ── sys_mremap(old_addr, old_size, new_size, flags, new_addr) — EAX=163 ──
 * Linux mm/mremap.c: shrink in place, grow in place when the space after the
 * mapping is free, otherwise (MREMAP_MAYMOVE) move the page table entries to
 * a new range — no data is copied.  glibc/musl realloc() and JS ArrayBuffer
 * growth use this for large blocks. */
#define MREMAP_MAYMOVE_K   1
#define MREMAP_FIXED_K     2
#define MREMAP_DONTUNMAP_K 4

/* Attributes of the last VMA piece of the old range, captured before any list
 * surgery (the extension of a grown mapping continues them). */
struct mremap_tail { uint32_t prot, flags, off, fsize; vfs_node_t *file; };

/* Move the page table entries of [old,old+len) to new (both page aligned,
 * non-overlapping) and the VMA coverage with them.  The pieces are snapshotted
 * first because inserting at the destination may merge with (and free) nodes
 * of the source range. */
#define MREMAP_MAX_PIECES 32
static int mremap_move(uint32_t old, uint32_t len, uint32_t new) {
    struct proc *o = mmap_owner();
    if (!o) return -12;
    struct { uint32_t s, e, prot, flags, off, fsize; vfs_node_t *file; } pcs[MREMAP_MAX_PIECES];
    int np = 0;
    for (struct vma *v = o->vmas; v && v->start < old + len; v = v->next) {
        if (v->end <= old) continue;
        if (np == MREMAP_MAX_PIECES) {
            while (np) { np--; if (pcs[np].file) vfs_close(pcs[np].file); }
            return -12;
        }
        uint32_t cs = v->start > old ? v->start : old;
        uint32_t ce = v->end < old + len ? v->end : old + len;
        pcs[np].s = new + (cs - old); pcs[np].e = new + (ce - old);
        pcs[np].prot = v->prot; pcs[np].flags = v->flags; pcs[np].fsize = v->file_size;
        pcs[np].file = v->file; pcs[np].off = v->file ? v->file_off + (cs - v->start) : 0;
        if (v->file) vfs_retain(v->file);         /* survive the removal below */
        np++;
    }
    vma_remove_range(old, old + len);
    for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
        uint32_t va = old + off;
        if (!(*paging_get_pde(va) & PAGE_PRESENT)) continue;
        uint32_t *pte = paging_get_pte(va);
        uint32_t e = *pte;
        if (!pte_mapped(e)) continue;
        *pte = 0;
        tlb_flush_single(va);
        paging_map(new + off, e & ~0xFFFU, e & 0xFFFU);   /* same frame, same flags */
    }
    tlb_shootdown();
    for (int i = 0; i < np; i++) {
        struct vma *nv = vma_alloc(pcs[i].s, pcs[i].e, pcs[i].prot, pcs[i].flags,
                                   pcs[i].file, pcs[i].off);
        if (nv) { nv->file_size = pcs[i].fsize; vma_insert(o, nv); }
        if (pcs[i].file) vfs_close(pcs[i].file);
    }
    return 0;
}

/* Add the extension [s,e) of a grown mapping, continuing the tail piece. */
static int mremap_extend(uint32_t s, uint32_t e, const struct mremap_tail *t) {
    if ((t->flags & VMA_F_SHARED) && !t->file) return -12;   /* shared anon: no lazy path */
    struct proc *o = mmap_owner();
    struct vma *nv = o ? vma_alloc(s, e, t->prot, t->flags, t->file, t->off) : NULL;
    if (!nv) return -12;
    nv->file_size = t->fsize;
    vma_insert(o, nv);
    return 0;
}

static int sys_mremap(registers_t *regs) {
    uint32_t old_addr = regs->ebx;
    uint32_t old_size = regs->ecx;
    uint32_t new_size = regs->edx;
    uint32_t flags    = regs->esi;
    uint32_t new_addr = regs->edi;

    if (old_addr & (PAGE_SIZE - 1)) return -22;
    if (flags & ~(uint32_t)(MREMAP_MAYMOVE_K | MREMAP_FIXED_K)) return -22;
    if ((flags & MREMAP_FIXED_K) && !(flags & MREMAP_MAYMOVE_K)) return -22;
    if (new_size == 0) return -22;
    old_size = (old_size + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
    new_size = (new_size + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
    if (old_addr + old_size < old_addr || old_addr + old_size > 0xC0000000U) return -22;
    if (old_size == 0) return -22;               /* shared-dup form not supported */

    /* The old range must be fully covered by mappings (Linux: EFAULT otherwise). */
    for (uint32_t a = old_addr; a < old_addr + old_size; ) {
        struct vma *c = vma_find(a);
        if (!c) return -14;
        a = c->end;
    }
    struct mremap_tail t;
    {
        struct vma *last = vma_find(old_addr + old_size - PAGE_SIZE);
        uint32_t tail = old_addr + old_size;
        t.prot = last->prot; t.flags = last->flags; t.fsize = last->file_size;
        t.file = last->file; t.off = last->file ? last->file_off + (tail - last->start) : 0;
        if (t.file) vfs_retain(t.file);         /* outlives the list surgery below */
    }
    int ret;

    if (flags & MREMAP_FIXED_K) {
        if ((new_addr & (PAGE_SIZE - 1)) || new_addr + new_size < new_addr ||
            new_addr + new_size > (uint32_t)USER_STACK_BASE ||
            (new_addr < old_addr + old_size && old_addr < new_addr + new_size)) {
            ret = -22;
            goto out;
        }
        unmap_range(new_addr, new_addr + new_size);
        uint32_t mv = old_size < new_size ? old_size : new_size;
        ret = mremap_move(old_addr, mv, new_addr);
        if (ret < 0) goto out;
        if (mv < old_size) unmap_range(old_addr + mv, old_addr + old_size);
        if (new_size > mv) {
            ret = mremap_extend(new_addr + mv, new_addr + new_size, &t);
            if (ret < 0) goto out;
        }
        ret = (int)new_addr;
        goto out;
    }

    if (new_size <= old_size) {                  /* shrink (or same size) in place */
        if (new_size < old_size) unmap_range(old_addr + new_size, old_addr + old_size);
        ret = (int)old_addr;
        goto out;
    }

    /* grow: in place if the space right after is free */
    {
        uint32_t grow = new_size - old_size;
        uint32_t tail = old_addr + old_size;
        if (tail + grow > tail && tail + grow <= MMAP_TOP && vma_range_free(tail, grow) &&
            !((t.flags & VMA_F_SHARED) && !t.file)) {
            ret = mremap_extend(tail, tail + grow, &t);
            if (ret == 0) ret = (int)old_addr;
            goto out;
        }
    }
    if (!(flags & MREMAP_MAYMOVE_K)) { ret = -12; goto out; }
    if ((t.flags & VMA_F_SHARED) && !t.file) { ret = -12; goto out; }   /* shared anon: cannot grow */

    {
        uint32_t dest = vma_gap_find(new_size, new_size >= 0x100000U ? 0x100000U : PAGE_SIZE);
        if (!dest) dest = vma_gap_find(new_size, PAGE_SIZE);
        if (!dest) { ret = -12; goto out; }
        ret = mremap_move(old_addr, old_size, dest);
        if (ret < 0) goto out;
        ret = mremap_extend(dest + old_size, dest + new_size, &t);
        if (ret < 0) { unmap_range(dest, dest + new_size); goto out; }
        ret = (int)dest;
    }
out:
    if (t.file) vfs_close(t.file);
    return ret;
}

/* msync (144) and mlock/munlock/mlockall/munlockall (150-153): there is no
 * writeback (tmpfs/memfd frames ARE the file) and no swap, so every page is
 * always "locked" and "synced".  Accept and return 0 like Linux would after
 * doing the work. */
static int sys_msync(registers_t *regs) {
    uint32_t addr = regs->ebx;
    if (addr & (PAGE_SIZE - 1)) return -22;
    return 0;
}
static int sys_mlock_noop(registers_t *regs) {
    (void)regs;
    return 0;
}

/* ── sys_stat64(path, stat64*) — EAX=195 ────────────────────────────────── */
static int sys_stat64(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0)
        return -14;
    struct kstat64 *st = (struct kstat64 *)(uintptr_t)regs->ecx;
    if (!access_ok(st, sizeof(*st))) return -14;

    vfs_node_t *n = vfs_open_at(path);
    fx_lockdbg("stat64", path, n ? 0 : -2);
    if (!n) return -2;
    struct kstat64 kst;
    fill_kstat64(&kst, n);
    return copy_to_user(st, &kst, sizeof(kst));
}

/* forward declaration — defined below near symlink code */
static int sys_lstat64_real(registers_t *regs);

/* ── sys_lstat64(path, stat64*) — EAX=196 ─────────────────────────────────── */
static int sys_lstat64(registers_t *regs) {
    return sys_lstat64_real(regs);
}

/* ── sys_fstat64(fd, stat64*) — EAX=197 ─────────────────────────────────── */
static int sys_fstat64(registers_t *regs) {
    int fd = (int)regs->ebx;
    struct kstat64 *st = (struct kstat64 *)(uintptr_t)regs->ecx;
    if (!access_ok(st, sizeof(*st))) return -14;

    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];

    if (f->type == FD_NONE) {
        if (fd <= 2) {
            struct kstat64 kst;
            __builtin_memset(&kst, 0, sizeof(kst));
            kst.st_mode = 0020666;
            return copy_to_user(st, &kst, sizeof(kst));
        }
        return -9;
    }
    if (f->type == FD_PIPE_R || f->type == FD_PIPE_W) {
        struct kstat64 kst;
        __builtin_memset(&kst, 0, sizeof(kst));
        kst.st_mode = 0010666;
        return copy_to_user(st, &kst, sizeof(kst));
    }
    struct kstat64 kst;
    fill_kstat64(&kst, f->node);
    return copy_to_user(st, &kst, sizeof(kst));
}

/* ── sys_gettid() — EAX=224 ─────────────────────────────────────────────── */
static int sys_gettid(registers_t *regs) {
    (void)regs;
    return current_proc ? current_proc->pid : 0;
}

/* ── sys_exit_group(status) — EAX=252 ───────────────────────────────────── */
/* Linux do_group_exit(): the exit code is fixed for the whole group first, then
 * every other thread is SIGKILLed and the caller exits.  waitpid reports the
 * leader once the last thread is gone, with THIS status (not the SIGKILL the
 * siblings, or the leader itself, died of). */
static void sys_exit_group(registers_t *regs) {
    proc_group_exit(((int)regs->ebx & 0xff) << 8);
}

/* ── sys_set_thread_area(user_desc*) — EAX=243 ──────────────────────────── */
static int sys_set_thread_area(registers_t *regs) {
    /* struct user_desc { int entry_number; unsigned base_addr; ... } —
     * we always hand out GDT entry 6 (selector 0x33).  The scheduler
     * re-bases the descriptor on every context switch from tls_base. */
    struct { int entry_number; uint32_t base_addr; } ud;
    void *uptr = (void *)(uintptr_t)regs->ebx;

    if (!current_proc) return -38;
    if (copy_from_user(&ud, uptr, sizeof(ud)) < 0) return -14;
    if (ud.entry_number != -1 && ud.entry_number != 6) return -22;

    current_proc->tls_base = ud.base_addr;
    gdt_set_tls(ud.base_addr);
    ud.entry_number = 6;
    return copy_to_user(uptr, &ud, sizeof(ud));
}

/* ── sys_set_tid_address(int *tidptr) — EAX=258 ─────────────────────────── */
static int sys_set_tid_address(registers_t *regs) {
    /* Remember where to write 0 + futex-wake when this thread exits; musl's
     * pthread_join() sleeps on exactly this address. */
    if (current_proc)
        current_proc->clear_child_tid = regs->ebx;
    return current_proc ? current_proc->pid : 1;
}

/* ── sys_clock_gettime(clockid, timespec*) — EAX=265 ────────────────────── */
static int sys_clock_gettime(registers_t *regs) {
    int clk = (int)regs->ebx;
    struct ktimespec *ts = (struct ktimespec *)(uintptr_t)regs->ecx;
    int64_t sec; uint32_t nsec;
    int r = kclock_get(clk, &sec, &nsec);
    if (r < 0) return r;
    if (ts) {
        struct ktimespec kts = { (int32_t)sec, (int32_t)nsec };
        int cr = copy_to_user(ts, &kts, sizeof(kts));
        if (cr < 0) return cr;
    }
    return 0;
}

/* ── sys_clock_gettime64(clockid, timespec64*) — EAX=403 ────────────────── */
/* time64 variant: struct __kernel_timespec { int64_t tv_sec; int64_t tv_nsec; }. */
static int sys_clock_gettime64(registers_t *regs) {
    int clk = (int)regs->ebx;
    void *uts = (void *)(uintptr_t)regs->ecx;
    int64_t sec; uint32_t nsec;
    int r = kclock_get(clk, &sec, &nsec);
    if (r < 0) return r;
    if (uts) {
        struct { int64_t s; int64_t ns; } kt = { sec, (int64_t)nsec };
        int cr = copy_to_user(uts, &kt, sizeof(kt));
        if (cr < 0) return cr;
    }
    return 0;
}

/* ── sys_statx(dirfd, path, flags, mask, statxbuf) — EAX=383 ────────────── */
/* fontconfig/GLib lean on statx; map it onto our existing VFS stat. */
struct kstatx {
    uint32_t stx_mask, stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink, stx_uid, stx_gid;
    uint16_t stx_mode; uint16_t _pad1;
    uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    /* timestamps: each is { int64 sec; uint32 nsec; int32 pad } */
    struct { int64_t sec; uint32_t nsec; int32_t pad; }
        stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    uint64_t stx_mnt_id; uint64_t _spare[12];
};

static int sys_statx(registers_t *regs) {
    int dirfd = (int)regs->ebx;
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ecx, path, 256) < 0) return -14;
    void *ubuf = (void *)(uintptr_t)regs->edi;
    if (!access_ok(ubuf, sizeof(struct kstatx))) return -14;

    vfs_node_t *n;
    if (path[0] == '/' || path[0] == '\0') {
        /* empty path + AT_EMPTY_PATH(0x1000) → stat dirfd itself */
        if (path[0] == '\0' && dirfd >= 0 && dirfd < MAX_FD &&
            current_proc->ofile[dirfd].type == FD_FILE)
            n = current_proc->ofile[dirfd].node;
        else
            n = vfs_open_at(path);
    } else {
        n = vfs_open_at(path);
    }
    fx_lockdbg("statx", path, n ? 0 : -2);
    if (!n) return -2;

    struct kstat64 kst;
    fill_kstat64(&kst, n);

    struct kstatx sx;
    __builtin_memset(&sx, 0, sizeof(sx));
    sx.stx_mask    = 0x000007ff;          /* STATX_BASIC_STATS */
    sx.stx_blksize = 4096;
    sx.stx_nlink   = (uint32_t)kst.st_nlink;
    sx.stx_uid     = kst.st_uid;
    sx.stx_gid     = kst.st_gid;
    sx.stx_mode    = (uint16_t)kst.st_mode;
    sx.stx_ino     = kst.st_ino;
    sx.stx_size    = kst.st_size;
    sx.stx_blocks  = (kst.st_size + 511) / 512;
    sx.stx_mtime.sec = kst.st_mtime;
    sx.stx_atime.sec = kst.st_atime;
    sx.stx_ctime.sec = kst.st_ctime;
    return copy_to_user(ubuf, &sx, sizeof(sx));
}

/* ── sys_rt_sigaction(sig, act, oact, sigsetsize) — EAX=174 ─────────────── */
static int sys_rt_sigaction(registers_t *regs) {
    int sig = (int)regs->ebx;
    /* struct sigaction { sa_handler; sa_flags; sa_restorer; sa_mask[2] } */
    const uint32_t *act  = (const uint32_t *)(uintptr_t)regs->ecx;
    uint32_t       *oact = (uint32_t *)(uintptr_t)regs->edx;

    if (sig < 1 || sig > 64) return -22;
    if (sig == SIGKILL || sig == SIGSTOP) return -22;

    /* Real-time signals (>= NSIGS=32) — glibc/NPTL register handlers for
     * SIGCANCEL(32)/SIGSETXID(33)/SIGTIMER for thread cancellation + setxid.
     * We can't deliver them (32-bit pending mask), but rejecting the
     * registration with EINVAL makes glibc's threading init abort.  Accept it
     * (report SIG_DFL as the old action) so library init proceeds. */
    if (sig >= NSIGS) {
        if (oact) {
            uint32_t koact[8];
            __builtin_memset(koact, 0, sizeof(koact));
            if (copy_to_user(oact, koact, sizeof(koact)) < 0) return -14;
        }
        return 0;
    }

    /* The table is shared by the thread group (CLONE_SIGHAND): a change made
     * by any thread is immediately in force for all of them (Linux
     * do_sigaction writes the shared sighand_struct under siglock). */
    struct sighand *sh = current_proc->sighand;
    sighandler_t old_handler = sh->handlers[sig];
    uint32_t     old_flags   = sh->flags[sig];

    if (oact) {
        uint32_t koact[8];
        __builtin_memset(koact, 0, sizeof(koact));
        koact[0] = (uint32_t)(uintptr_t)old_handler;
        koact[1] = old_flags;
        int cr = copy_to_user(oact, koact, sizeof(koact));
        if (cr < 0) return cr;
    }
    if (act) {
        uint32_t kact[8];
        int cr = copy_from_user(kact, act, sizeof(kact));
        if (cr < 0) return cr;
        sh->handlers[sig] = (sighandler_t)(uintptr_t)kact[0];
        sh->flags[sig]    = kact[1];  /* sa_flags (SA_RESTART etc.) */
        /* Linux do_sigaction: setting SIG_IGN (or SIG_DFL for a default-ignored
         * signal) discards matching signals already pending on every thread. */
        sighandler_t nh = sh->handlers[sig];
        if (nh == SIG_IGN || (nh == SIG_DFL && (sig == SIGCHLD || sig == SIGCONT)))
            for (int i = 0; i < MAX_PROCS; i++)
                if (ptable[i].state != PROC_UNUSED && ptable[i].sighand == sh)
                    ptable[i].pending_sigs &= ~(1u << sig);
    }
    return 0;
}

/* ── sys_rt_sigprocmask(how, set, oset, sigsetsize) — EAX=175 ────────────── */
/* The user sigset_t numbers bit (sig - 1) for signal sig (Linux sigmask(sig) =
 * 1UL << ((sig) - 1), include/linux/signal.h), whereas the kernel's pending_sigs
 * and blocked_sigs use bit sig.  Convert at the boundary; only the first word
 * (signals 1..32) is honoured. */
static uint32_t sigset_from_user(uint32_t uset) { return uset << 1; }
static uint32_t sigset_to_user(uint32_t kset)   { return kset >> 1; }

static int sys_rt_sigprocmask(registers_t *regs) {
    int       how  = (int)regs->ebx;
    uint32_t *nset = (uint32_t *)(uintptr_t)regs->ecx;
    uint32_t *oset = (uint32_t *)(uintptr_t)regs->edx;

    if (oset) {
        uint32_t old = sigset_to_user(current_proc->blocked_sigs);
        int cr = copy_to_user(oset, &old, sizeof(old));
        if (cr < 0) return cr;
    }

    if (nset) {
        uint32_t set = 0;
        int cr = copy_from_user(&set, nset, sizeof(set));
        if (cr < 0) return cr;
        set = sigset_from_user(set);
        /* SIGKILL and SIGSTOP cannot be blocked */
        set &= ~((1u << SIGKILL) | (1u << SIGSTOP));
        switch (how) {
        case 0: /* SIG_BLOCK */   current_proc->blocked_sigs |=  set; break;
        case 1: /* SIG_UNBLOCK */ current_proc->blocked_sigs &= ~set; break;
        case 2: /* SIG_SETMASK */ current_proc->blocked_sigs  =  set; break;
        default: return -22;  /* EINVAL */
        }
    }
    return 0;
}

/* ── sys_access(path, mode) — EAX=33 ────────────────────────────────────── */
static int sys_access(registers_t *regs) {
    char path[256];
    int mode = (int)regs->ecx;
    if (mode & ~7) return -22;
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0)
        return -14;
    vfs_node_t *n = vfs_open_at(path);
    int rc = n ? 0 : -2;  /* 0=exists, -ENOENT=not found */
    fx_lockdbg("access", path, rc);
    return rc;
}

/* ── sys_dup(int oldfd) — EAX=41 ────────────────────────────────────────── */
static int sys_dup(registers_t *regs) {
    int oldfd = (int)regs->ebx;
    if (oldfd < 0 || oldfd >= MAX_FD) return -9;
    proc_file_t *src = &current_proc->ofile[oldfd];
    if (src->type == FD_NONE) return -9;

    for (int i = 0; i < MAX_FD; i++) {
        if (current_proc->ofile[i].type == FD_NONE) {
            current_proc->ofile[i] = *src;
            current_proc->ofile[i].cloexec = 0;
            fd_retain(&current_proc->ofile[i]);
            return i;
        }
    }
    return -24;   /* -EMFILE */
}

/* ── sys_getppid() — EAX=64 ─────────────────────────────────────────────── */
static int sys_getppid(registers_t *regs) {
    (void)regs;
    /* parent is always a group leader, so its pid is the parent's tgid. */
    return (current_proc && current_proc->parent) ? current_proc->parent->tgid : 1;
}

/* ── sys_setsid() — EAX=66 ───────────────────────────────────────────────── */
static int sys_setsid(registers_t *regs) {
    (void)regs;
    if (!current_proc) return 1;
    if (current_proc->ctty) {
        vfs_close(current_proc->ctty);
        current_proc->ctty = NULL;
    }
    current_proc->sid  = current_proc->pid;
    current_proc->pgrp = current_proc->pid;
    return current_proc->pid;
}

/* ── sys_umask(int mask) — EAX=60 ───────────────────────────────────────── */
static int sys_umask(registers_t *regs) {
    uint32_t new_mask = regs->ebx & 0777;
    uint32_t old_mask = current_proc ? current_proc->umask : 022;
    if (current_proc) current_proc->umask = new_mask;
    return (int)old_mask;
}

/* Create a FIFO / character device / regular file node at an already-resolved
 * absolute path.  Shared by mknod(14) and mknodat(297). */
static int do_mknod(const char *path, uint32_t mode) {
    char dir_path[256], base[256];
    if (path_split(path, dir_path, base) < 0) return -22;
    if (base[0] == '\0') return -22;

    vfs_node_t *dir = vfs_open_parent_at(path, dir_path);
    if (!dir || !dir->create_fn) return -2;

    /* Determine VFS flag from file type bits */
    uint32_t fmt = mode & 0xF000;  /* S_IFMT */
    uint32_t vfs_flag;
    if (fmt == 0x1000)       vfs_flag = VFS_FLAG_FIFO;    /* S_IFIFO  = 0010000 */
    else if (fmt == 0x2000)  vfs_flag = VFS_FLAG_CHARDEV; /* S_IFCHR  = 0020000 */
    else if (fmt == 0x8000)  vfs_flag = VFS_FLAG_FILE;    /* S_IFREG  = 0100000 */
    else if (fmt == 0)       vfs_flag = VFS_FLAG_FILE;    /* mode 0 = regular file */
    else return -22;  /* -EINVAL: unsupported type */

    return dir->create_fn(dir, base, vfs_flag);
}

/* ── sys_mknod(const char *path, mode_t mode, dev_t dev) — EAX=14 ────────── */
static int sys_mknod(registers_t *regs) {
    char path[256], resolved[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0) return -14;
    /* dev = regs->edx (ignored for FIFOs) */
    int r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return do_mknod(resolved, regs->ecx);
}

/* ── sys_mknodat(dirfd, path, mode, dev) — EAX=297 ───────────────────────────
 * Linux implements mknod(2) as mknodat(AT_FDCWD, …) (fs/namei.c do_mknodat)
 * and glibc's mkfifo()/mknod() issue this syscall directly. */
static int sys_mknodat(registers_t *regs) {
    char path[256], resolved[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ecx, path, 256) < 0) return -14;
    int r = resolve_path_at_fd((int)regs->ebx, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return do_mknod(resolved, regs->edx);
}

/* ── sys_reboot(magic, magic2, cmd, arg) — EAX=88 ───────────────────────── */
#define LINUX_REBOOT_CMD_POWER_OFF  0x4321FEDC
#define LINUX_REBOOT_CMD_RESTART    0x01234567
static int sys_reboot(registers_t *regs) {
    /* magic1 = ebx, magic2 = ecx, cmd = edx */
    uint32_t cmd = regs->edx;

    if (cmd == LINUX_REBOOT_CMD_POWER_OFF) {
        /* ACPI power off — try the well-known virtual-machine ports. */
        __asm__ volatile("cli");
        outw(0x604, 0x2000);     /* QEMU (>= 2.x) ACPI shutdown */
        outw(0xB004, 0x2000);    /* Bochs / older QEMU */
        outw(0x4004, 0x3400);    /* VirtualBox */
        for (;;) __asm__ volatile("hlt");
    }

    if (cmd == LINUX_REBOOT_CMD_RESTART) {
        printk("[REBOOT] restart requested — resetting CPU\n");
        __asm__ volatile("cli");

        /* 1. 0xCF9 reset-control register (QEMU/modern chipsets). */
        outb(0xCF9, 0x02);
        outb(0xCF9, 0x0E);

        /* 2. Pulse the 8042 keyboard-controller CPU reset line. */
        for (int i = 0; i < 100000; i++)
            if (!(inb(0x64) & 0x02)) break;   /* wait input buffer empty */
        outb(0x64, 0xFE);

        /* 3. Triple fault — install an empty IDT so the very next interrupt
         *    cascades #GP → #DF → triple fault → CPU reset.  Foolproof on x86;
         *    QEMU with -no-reboot exits, otherwise the machine restarts. */
        static const struct { uint16_t limit; uint32_t base; }
            __attribute__((packed)) null_idtr = { 0, 0 };
        __asm__ volatile("lidt %0" :: "m"(null_idtr) : "memory");
        __asm__ volatile("int $0x03");

        for (;;) __asm__ volatile("hlt");
    }
    return 0;
}

/* ── sys_lstat(path, stat*) — EAX=107 ───────────────────────────────────── */
static int sys_lstat(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0) return -14;
    struct kstat *st = (struct kstat *)(uintptr_t)regs->ecx;
    if (!access_ok(st, sizeof(*st))) return -14;
    /* lstat does NOT follow the final symlink */
    vfs_node_t *n;
    char abspath[256];
    if (path[0] == '/') {
        n = vfs_open_nofollow(path);
    } else {
        uint32_t cwdlen = (uint32_t)__builtin_strlen(current_proc->cwd);
        __builtin_memcpy(abspath, current_proc->cwd, cwdlen);
        if (cwdlen > 1) abspath[cwdlen++] = '/';
        __builtin_memcpy(abspath + cwdlen, path, __builtin_strlen(path) + 1);
        n = vfs_open_nofollow(abspath);
    }
    if (!n) return -2;
    struct kstat kst;
    fill_kstat(&kst, n);
    /* For symlinks, st_mode should be S_IFLNK */
    if (n->flags == VFS_FLAG_SYMLINK)
        kst.st_mode = (kst.st_mode & ~0xF000) | 0xA000;  /* S_IFLNK */
    return copy_to_user(st, &kst, sizeof(kst));
}

/* ── sys_lstat64 variant ─────────────────────────────────────────────────── */
static int sys_lstat64_real(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0) return -14;
    struct kstat64 *st = (struct kstat64 *)(uintptr_t)regs->ecx;
    if (!access_ok(st, sizeof(*st))) return -14;
    vfs_node_t *n;
    char abspath[256];
    if (path[0] == '/') {
        n = vfs_open_nofollow(path);
    } else {
        uint32_t cwdlen = (uint32_t)__builtin_strlen(current_proc->cwd);
        __builtin_memcpy(abspath, current_proc->cwd, cwdlen);
        if (cwdlen > 1) abspath[cwdlen++] = '/';
        __builtin_memcpy(abspath + cwdlen, path, __builtin_strlen(path) + 1);
        n = vfs_open_nofollow(abspath);
    }
    fx_lockdbg("lstat64", path, n ? 0 : -2);
    if (!n) return -2;
    struct kstat64 kst;
    fill_kstat64(&kst, n);
    if (n->flags == VFS_FLAG_SYMLINK)
        kst.st_mode = (kst.st_mode & ~0xF000) | 0xA000;
    return copy_to_user(st, &kst, sizeof(kst));
}

/* ── sys_symlink(target, linkpath) — EAX=83 ─────────────────────────────── */
static int sys_symlink(registers_t *regs) {
    char target[512], linkpath[512];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, target, 512) < 0) return -14;
    if (copy_user_str((const char *)(uintptr_t)regs->ecx, linkpath, 512) < 0) return -14;
    /* Resolve linkpath relative to cwd */
    char abspath[512];
    if (linkpath[0] != '/') {
        uint32_t cwdlen = (uint32_t)__builtin_strlen(current_proc->cwd);
        __builtin_memcpy(abspath, current_proc->cwd, cwdlen);
        if (cwdlen > 1) abspath[cwdlen++] = '/';
        __builtin_memcpy(abspath + cwdlen, linkpath, __builtin_strlen(linkpath) + 1);
    } else {
        __builtin_memcpy(abspath, linkpath, __builtin_strlen(linkpath) + 1);
    }
    int rc = vfs_symlink(target, abspath);
    {   /* [lockop] nsProfileLock's symlink lock: target="<host>:<pid>",
         * link="<profile>/lock".  A nonzero rc → lock fails → modal dialog. */
        static int sl = 0;
        if (sl < 20) { sl++;
            printk("[lockop] symlink '%s' -> '%s' = %d\n", abspath, target, rc); }
    }
    return rc;
}

/* ── sys_readlink(path, buf, bufsiz) — EAX=85 ───────────────────────────── */
static int sys_readlink(registers_t *regs) {
    char path[512];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 512) < 0) return -14;
    char *ubuf = (char *)(uintptr_t)regs->ecx;
    int bufsiz = (int)regs->edx;
    if (bufsiz <= 0 || !access_ok(ubuf, (uint32_t)bufsiz)) return -14;

    /* Resolve without following final symlink */
    vfs_node_t *n;
    if (path[0] == '/') {
        n = vfs_open_nofollow(path);
    } else {
        char abspath[512];
        uint32_t cwdlen = (uint32_t)__builtin_strlen(current_proc->cwd);
        __builtin_memcpy(abspath, current_proc->cwd, cwdlen);
        if (cwdlen > 1) abspath[cwdlen++] = '/';
        __builtin_memcpy(abspath + cwdlen, path, __builtin_strlen(path) + 1);
        n = vfs_open_nofollow(abspath);
    }
    fx_lockdbg("readlink", path, n ? (n->flags == VFS_FLAG_SYMLINK ? 0 : -22) : -2);
    if (!n) return -2;
    if (n->flags != VFS_FLAG_SYMLINK) return -22;   /* -EINVAL: not a symlink */

    /* Read target from the symlink node */
    char target[512];
    uint32_t tlen = vfs_read(n, 0, sizeof(target), (uint8_t *)target);
    if (tlen == 0) return -22;
    int copylen = (int)tlen < bufsiz ? (int)tlen : bufsiz;
    int cr = copy_to_user(ubuf, target, (uint32_t)copylen);
    if (cr < 0) return cr;
    return copylen;
}

/* ── sys_truncate(path, length) — EAX=92 ────────────────────────────────── */
static int sys_truncate(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0)
        return -14;
    uint32_t len = (uint32_t)regs->ecx;
    vfs_node_t *n = vfs_open_at(path);
    if (!n) return -2;
    return vfs_truncate(n, len);
}

/* ── sys_ftruncate(fd, length) — EAX=93 ─────────────────────────────────── */
static int sys_ftruncate(registers_t *regs) {
    int fd = (int)regs->ebx;
    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type != FD_FILE || !f->node) return -9;
    return vfs_truncate(f->node, (uint32_t)regs->ecx);
}

/* ── sys_truncate64(path, length_lo, length_hi) — EAX=193 ───────────────────
 * ── sys_ftruncate64(fd,  length_lo, length_hi) — EAX=194 ───────────────────
 * Linux i386 carries a 64-bit-offset (f)truncate pair alongside the legacy
 * 92/93; musl always issues these (src/unistd/{f,}truncate.c pass the length
 * as the __SYSCALL_LL_O register pair ECX:EDX).  Our files are below 4 GiB, so
 * a nonzero high word is -EFBIG, exactly what Linux reports past s_maxbytes. */
static int sys_truncate64(registers_t *regs) {
    if (regs->edx) return -27;                     /* -EFBIG */
    return sys_truncate(regs);                     /* ECX already holds the low word */
}

static int sys_ftruncate64(registers_t *regs) {
    if (regs->edx) return -27;                     /* -EFBIG */
    return sys_ftruncate(regs);
}

/* ── sys_fallocate(fd, mode, offset, len) — EAX=324 ─────────────────────────
 * Firefox sizes its memfd-backed shared memory with fallocate(fd, 0, 0, size);
 * returning -ENOSYS made it log "fallocate failed to set shm size".  On i386
 * the 64-bit offset/len are register pairs (offset=edx:esi, len=edi:ebp); shm
 * segments are well under 4 GiB so we use the low words.  mode 0 = allocate:
 * ensure the file is at least offset+len bytes (grow a tmpfs/memfd file). */
static int sys_fallocate(registers_t *regs) {
    int fd = (int)regs->ebx;
    uint32_t offset = regs->edx;
    uint32_t len    = regs->edi;
    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type != FD_FILE || !f->node) return -9;
    uint32_t want = offset + len;
    if (want < offset) return -22;                 /* -EINVAL: overflow */
    if (want > f->node->size)
        return vfs_truncate(f->node, want);        /* grow to size */
    return 0;
}

/* ── fd readiness helpers ──────────────────────────────────────────────────── */

static int fd_read_ready(int fd) {
    if (fd < 0 || fd >= MAX_FD) return 0;
    proc_file_t *f = &current_proc->ofile[fd];
    switch (f->type) {
    case FD_NONE:   return 0;
    case FD_PIPE_R: return (f->pipe->count > 0 || f->pipe->nwriters == 0) ? 1 : 0;
    case FD_SOCKET: return net_socket_read_ready(f->socket);
    case FD_USOCKET: return usocket_read_ready(f->usock);
    case FD_EVENTFD: return f->efd->count > 0 ? 1 : 0;
    case FD_FILE:
        if (f->node && f->node->read_ready_fn)
            return f->node->read_ready_fn(f->node);
        if (f->node && f->node->flags == VFS_FLAG_CHARDEV)
            return 1;
        return 1;  /* regular/proc files always ready */
    default: return 1;
    }
}

static int fd_write_ready(int fd) {
    if (fd < 0 || fd >= MAX_FD) return 0;
    proc_file_t *f = &current_proc->ofile[fd];
    switch (f->type) {
    case FD_NONE:   return 0;
    case FD_PIPE_W: return (f->pipe->count < PIPE_BUF_SIZE || f->pipe->nreaders == 0) ? 1 : 0;
    case FD_SOCKET: return net_socket_write_ready(f->socket);
    case FD_USOCKET: return usocket_write_ready(f->usock);
    case FD_EVENTFD: return f->efd->count < 0xFFFFFFFFFFFFFFFEULL ? 1 : 0;
    case FD_FILE:
        if (f->node && f->node->write_ready_fn)
            return f->node->write_ready_fn(f->node);
        return 1;
    default: return 1;
    }
}

/* ── sys_select(n, readfds, writefds, exceptfds, timeout) — EAX=82 ────────── */
/* Common body of select(2)/_newselect and pselect6.  toms < 0 blocks without
 * a deadline; 0 polls once; > 0 is the timeout in ms (fs/select.c
 * core_sys_select -> do_select with a schedule_hrtimeout deadline).  Returns
 * 0 when the deadline passes with nothing ready and -ERESTARTNOHAND when a
 * signal interrupts the wait (EINTR once its handler has run; man 7 signal
 * lists select among the calls that are never restarted by SA_RESTART). */
static int do_select(int n, uint32_t *readfds, uint32_t *writefds,
                     uint32_t *exceptfds, int toms) {
#define SELECT_WORDS ((MAX_FD + 31) / 32)
    if (n <= 0) return 0;
    if (n > MAX_FD) n = MAX_FD;

    uint32_t rd_in[SELECT_WORDS] = {0}, wr_in[SELECT_WORDS] = {0};
    uint32_t words = (uint32_t)((n + 31) / 32);
    if (readfds) {
        int cr = copy_from_user(rd_in, readfds, words * 4);
        if (cr < 0) return cr;
    }
    if (writefds) {
        int cr = copy_from_user(wr_in, writefds, words * 4);
        if (cr < 0) return cr;
    }

    struct kdeadline dl;
    if (toms > 0) deadline_set_ms(&dl, (uint32_t)toms);

    for (;;) {
        uint32_t rd_out[SELECT_WORDS] = {0}, wr_out[SELECT_WORDS] = {0};
        int ready = 0;
        for (int fd = 0; fd < n; fd++) {
            uint32_t w = (uint32_t)fd >> 5, b = 1u << (fd & 31);
            if (rd_in[w] & b) { if (fd_read_ready(fd))  { rd_out[w] |= b; ready++; } }
            if (wr_in[w] & b) { if (fd_write_ready(fd)) { wr_out[w] |= b; ready++; } }
        }
        int expired = (toms == 0) || (toms > 0 && deadline_expired(&dl));
        if (ready > 0 || expired) {
            if (readfds) {
                int cr = copy_to_user(readfds, rd_out, words * 4);
                if (cr < 0) return cr;
            }
            if (writefds) {
                int cr = copy_to_user(writefds, wr_out, words * 4);
                if (cr < 0) return cr;
            }
            if (exceptfds) {        /* exceptional conditions are not tracked */
                uint32_t ex_out[SELECT_WORDS] = {0};
                int cr = copy_to_user(exceptfds, ex_out, words * 4);
                if (cr < 0) return cr;
            }
            return ready;
        }
        if (signal_interrupt_pending(current_proc))
            return -ERESTARTNOHAND;
        uint32_t cap = 50;
        if (toms > 0) cap = deadline_sleep_ticks(&dl, cap);
        io_wait_sleep(cap);   /* woken early by any I/O activity or a signal */
        if (signal_interrupt_pending(current_proc))
            return -ERESTARTNOHAND;
    }
}

/* select(n, in, out, ex, struct timeval *) — EAX=82 (old, via struct) and 142
 * (_newselect).  Linux updates the timeval with the time left (unless
 * STICKY_TIMEOUTS); a NULL timeval blocks indefinitely. */
static int sys_select(registers_t *regs) {
    int       n        = (int)regs->ebx;
    uint32_t *readfds  = (uint32_t *)(uintptr_t)regs->ecx;
    uint32_t *writefds = (uint32_t *)(uintptr_t)regs->edx;
    uint32_t *exceptfds = (uint32_t *)(uintptr_t)regs->esi;
    struct { int32_t tv_sec; int32_t tv_usec; } *tv = (void *)(uintptr_t)regs->edi;
    int toms = -1;
    if (tv) {
        struct { int32_t tv_sec; int32_t tv_usec; } ktv;
        int cr = copy_from_user(&ktv, tv, sizeof(ktv));
        if (cr < 0) return cr;
        if (ktv.tv_sec < 0 || ktv.tv_usec < 0) return -22;
        if (ktv.tv_sec > 2000000) toms = 0x7fffffff;
        /* Round the sub-millisecond remainder UP: Linux's
         * poll_select_set_timeout() never shortens the requested interval. */
        else toms = ktv.tv_sec * 1000 + (ktv.tv_usec + 999) / 1000;
    }
    uint32_t start = pit_ticks();
    int ret = do_select(n, readfds, writefds, exceptfds, toms);
    if (tv && toms >= 0) {
        uint32_t used_ms = (pit_ticks() - start) * 10U;
        uint32_t left_ms = used_ms < (uint32_t)toms ? (uint32_t)toms - used_ms : 0;
        struct { int32_t tv_sec; int32_t tv_usec; } krem =
            { (int32_t)(left_ms / 1000U), (int32_t)((left_ms % 1000U) * 1000U) };
        copy_to_user(tv, &krem, sizeof(krem));
    }
    return ret;
}

/* pselect6(n, in, out, ex, struct timespec *, sigmask*) — EAX=308.  The
 * timeout is a TIMESPEC (nanoseconds, not microseconds) and is not updated;
 * the temporary sigmask is not applied (S6, out of scope here). */
static int sys_pselect6(registers_t *regs) {
    int       n        = (int)regs->ebx;
    uint32_t *readfds  = (uint32_t *)(uintptr_t)regs->ecx;
    uint32_t *writefds = (uint32_t *)(uintptr_t)regs->edx;
    uint32_t *exceptfds = (uint32_t *)(uintptr_t)regs->esi;
    uint32_t  tsp      = regs->edi;
    int toms = -1;
    if (tsp) {
        struct { int32_t s, ns; } ts;
        if (copy_from_user(&ts, (void *)(uintptr_t)tsp, sizeof(ts)) < 0) return -14;
        if (ts.s < 0 || ts.ns < 0) return -22;
        if (ts.s > 2000000) toms = 0x7fffffff;
        else toms = ts.s * 1000 + (ts.ns + 999999) / 1000000;   /* round up */
    }
    return do_select(n, readfds, writefds, exceptfds, toms);
}

/* ── sys_poll(fds, nfds, timeout) — EAX=168 ────────────────────────────────── */
#define POLLIN  1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLNVAL 32

/*
 * Block until I/O activity (pipes/PTYs/input/NIC producers call io_wake())
 * or the safety deadline passes.  The deadline covers any source that does
 * not signal io_activity yet.
 */
static void io_wait_sleep(uint32_t max_ticks) {
    current_proc->wake_tick = pit_ticks() + max_ticks;
    sleep_on(&io_activity);
}

/* [lockop] log a profile-relevant filesystem op + its result, gated to firefox
 * and to paths under the profile dir (/tmp/ffp), to find the exact operation
 * whose failure makes Firefox report "profile cannot be loaded / inaccessible". */
static void fx_lockdbg(const char *op, const char *path, int rc) {
    static int lk = 0;
    if (lk >= 160 || !current_proc) return;
    if (!(current_proc->name[0]=='f' && current_proc->name[1]=='i' &&
          current_proc->name[4]=='f')) return;
    /* Log the profile dir, the mozilla/home config areas, AND any FAILING op
     * (rc<0) — the failure that makes Firefox report "profile inaccessible" is
     * on some path; catch it wherever it is. */
    /* Log the full non-library op sequence (skip .so loader noise) so we can see
     * exactly what Firefox does right before it gives up loading the profile. */
    if (dbg_str_has(path, ".so") || dbg_str_has(path, "/lib/") ||
        dbg_str_has(path, "/disk/firefox/") || dbg_str_has(path, "fontconfig") ||
        dbg_str_has(path, "/etc/") || dbg_str_has(path, "/usr/"))
        return;
    lk++;
    printk("[lockop] %s '%s' -> %d\n", op, path, rc);
}

static int sys_poll(registers_t *regs) {
    uint32_t *fds  = (uint32_t *)(uintptr_t)regs->ebx;
    uint32_t  nfds = regs->ecx;
    int       toms = (int)regs->edx;
    {
        static int pll = 0;
        if (pll < 60 && current_proc && current_proc->name[0]=='f' &&
            current_proc->name[1]=='i' && current_proc->name[4]=='f') {
            pll++;
            printk("[pollarg] pid=%d nfds=%u toms=%d fds=%x\n",
                   current_proc->pid, (unsigned)nfds, toms, (unsigned)(uintptr_t)fds);
        }
    }

    if (!nfds) return 0;
    if (nfds > 1024) {
        printk("[poll-einval] pid=%d name=%s nfds=%u fds=%x toms=%d\n",
               current_proc ? current_proc->pid : -1,
               current_proc ? current_proc->name : "?",
               (unsigned)nfds, (unsigned)(uintptr_t)fds, toms);
        return -22;  /* -EINVAL */
    }
    if (!fds) return -14;  /* -EFAULT */
    size_t fds_len = (size_t)nfds * 8;
    uint32_t *kfds = (uint32_t *)kmalloc(fds_len);
    if (!kfds) return -12;
    int cr = copy_from_user(kfds, fds, fds_len);
    if (cr < 0) {
        kfree(kfds);
        return cr;
    }

    struct kdeadline dl;
    if (toms > 0) deadline_set_ms(&dl, (uint32_t)toms);

    for (;;) {
        int ready = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            int   fd     = (int)kfds[i * 2];
            short events = (short)(kfds[i * 2 + 1] & 0xffff);
            short rev    = 0;

            if (fd < 0) {
                kfds[i * 2 + 1] = (uint32_t)(uint16_t)events;
                continue;
            }
            if (fd >= MAX_FD || current_proc->ofile[fd].type == FD_NONE) {
                rev = POLLNVAL;
                {   /* A firefox process polling an INVALID fd (FD_NONE) → POLLNVAL.
                     * This is the glxtest/ManageChildProcess "poll failed" bug:
                     * the result-pipe fd went invalid.  Log fd + nfds to find it. */
                    static int pnv = 0;
                    if (pnv < 60 && current_proc && current_proc->name[0]=='f' &&
                        current_proc->name[1]=='i' && current_proc->name[4]=='f') {
                        pnv++;
                        printk("[pollnval] pid=%d t%d fd=%d nfds=%u events=%x\n",
                               current_proc->pid, current_proc->tgid, fd,
                               (unsigned)nfds, (unsigned)(uint16_t)events);
                    }
                }
            } else {
                proc_file_t *f = &current_proc->ofile[fd];
                if ((events & POLLIN)  && fd_read_ready(fd))  rev |= POLLIN;
                if ((events & POLLOUT) && fd_write_ready(fd)) rev |= POLLOUT;
                if (f->type == FD_PIPE_R && f->pipe->nwriters == 0) rev |= POLLHUP;
                if (f->type == FD_PIPE_W && f->pipe->nreaders == 0) rev |= POLLERR;
            }
            kfds[i * 2 + 1] = (kfds[i * 2 + 1] & 0xffff) | ((uint32_t)(uint16_t)rev << 16);
            if (rev) ready++;
        }
        if (ready > 0 || toms == 0) {
            cr = copy_to_user(fds, kfds, fds_len);
            kfree(kfds);
            return cr < 0 ? cr : ready;
        }
        if (toms > 0 && deadline_expired(&dl))
        {
            cr = copy_to_user(fds, kfds, fds_len);
            kfree(kfds);
            return cr < 0 ? cr : 0;
        }
        {   /* [pollfd] decisive diag: which fd is the firefox MAIN thread
             * blocking on with an infinite/long timeout, and what is that fd's
             * type + pipe/socket peer state?  Identifies the ManageChildProcess
             * (glxtest result pipe) stall. */
            if (toms < 0 && nfds >= 1 && current_proc &&
                current_proc->pid == current_proc->tgid &&
                current_proc->name[0]=='f' && current_proc->name[1]=='i' &&
                current_proc->name[4]=='f') {
                static int pf = 0;
                if (pf < 30) { pf++;
                    int fd0 = (int)kfds[0];
                    short ev0 = (short)(kfds[1] & 0xffff);
                    int ty = (fd0 >= 0 && fd0 < MAX_FD) ? (int)current_proc->ofile[fd0].type : -1;
                    int nr = -1, nw = -1;
                    if (ty == FD_PIPE_R || ty == FD_PIPE_W) {
                        nr = current_proc->ofile[fd0].pipe->nreaders;
                        nw = current_proc->ofile[fd0].pipe->nwriters;
                    }
                    /* For an AF_UNIX socket (the IPC handshake fd) report the rx
                     * buffer state: count (peer wrote?) + writer_closed (peer
                     * gone?).  Distinguishes "child alive but not writing" from
                     * an orphaned/mis-wired socket. */
                    int rxc = -1, wrc = -1;
                    uint32_t rxp = 0, txp = 0;
                    if (ty == FD_USOCKET && current_proc->ofile[fd0].usock) {
                        extern int usocket_rx_state(struct usocket *s, int *wclosed);
                        extern void usocket_dbg_ptrs(struct usocket *s, uint32_t *rx, uint32_t *tx);
                        rxc = usocket_rx_state(current_proc->ofile[fd0].usock, &wrc);
                        usocket_dbg_ptrs(current_proc->ofile[fd0].usock, &rxp, &txp);
                    }
                    printk("[pollfd] pid=%d fd=%d type=%d events=%x nr=%d nw=%d rxcount=%d wclosed=%d rx=%x tx=%x nfds=%u toms=%d\n",
                           current_proc->pid, fd0, ty, (unsigned)(uint16_t)ev0, nr, nw,
                           rxc, wrc, (unsigned)rxp, (unsigned)txp, (unsigned)nfds, toms);
                }
            }
            /* A deliverable signal interrupts the wait (fs/select.c
             * do_sys_poll returns -ERESTARTNOHAND: EINTR once the handler has
             * run, a transparent restart if none did; SA_RESTART never applies
             * to poll).  Check before AND after sleeping so a signal that
             * arrived while we scanned the fds is not slept through. */
            if (signal_interrupt_pending(current_proc)) {
                kfree(kfds);
                return -ERESTARTNOHAND;
            }
            /* Sleep until I/O activity (or the poll timeout). */
            uint32_t cap = 50;
            if (toms > 0) cap = deadline_sleep_ticks(&dl, cap);
            io_wait_sleep(cap);
            if (signal_interrupt_pending(current_proc)) {
                kfree(kfds);
                return -ERESTARTNOHAND;
            }
        }
    }
}

/* ── sys_ppoll(fds, nfds, tmo_ts, sigmask, sigsz) — EAX=309/414 ─────────────
 * glibc 2.34+ on i386 implements poll() via ppoll/ppoll_time64, so without
 * these Firefox's child-process pipe poll falls through to ENOSYS and its GPU
 * detection error path fires (then a use-after-free).  Convert the timespec
 * timeout to milliseconds and reuse the poll logic.  The sigmask is ignored
 * (we don't atomically swap it during the wait — adequate for these callers). */
static int sys_ppoll(registers_t *regs, int time64) {
    int toms = -1;                              /* NULL timeout → infinite */
    uint32_t tsp = regs->edx;
    if (tsp) {
        int32_t sec = 0, nsec = 0;              /* 32-bit math: no __divdi3 */
        if (time64) {
            struct { int64_t s, ns; } ts;
            if (copy_from_user(&ts, (void *)(uintptr_t)tsp, sizeof(ts)) < 0) return -14;
            sec  = (int32_t)ts.s;
            nsec = (int32_t)ts.ns;
        } else {
            struct { int32_t s, ns; } ts;
            if (copy_from_user(&ts, (void *)(uintptr_t)tsp, sizeof(ts)) < 0) return -14;
            sec = ts.s; nsec = ts.ns;
        }
        if (sec < 0 || sec > 2000000) toms = 0x7fffffff;   /* cap, avoid overflow */
        else toms = sec * 1000 + (nsec + 999999) / 1000000;     /* round up */
        if (toms < 0) toms = 0;
    }
    registers_t fake = *regs;
    fake.edx = (uint32_t)toms;
    return sys_poll(&fake);
}

/* ── epoll ──────────────────────────────────────────────────────────────────
 * A minimal level-triggered epoll, sufficient for Firefox/libevent's IPC I/O
 * thread.  Readiness reuses the same fd_read_ready/fd_write_ready helpers as
 * poll/select.  struct epoll_event on i386 is PACKED: { u32 events; u64 data }
 * = 12 bytes. */
#define EPOLL_MAX_ITEMS 256
#define EPOLLIN_K   0x001
#define EPOLLOUT_K  0x004
struct epoll {
    struct {
        int      fd;          /* -1 = empty slot */
        uint32_t events;      /* requested EPOLL* mask */
        uint8_t  data[8];     /* opaque epoll_data, echoed back */
    } items[EPOLL_MAX_ITEMS];
    int refcount;
};

static void epoll_retain(struct epoll *ep) {
    if (ep) ep->refcount++;
}
static void epoll_release(struct epoll *ep) {
    if (!ep) return;
    if (--ep->refcount <= 0) kfree(ep);
}

static int sys_epoll_create1(registers_t *regs) {
    int flags = (int)regs->ebx;            /* EPOLL_CLOEXEC = 0x80000 */
    struct epoll *ep = (struct epoll *)kmalloc(sizeof(struct epoll));
    if (!ep) return -12;
    __builtin_memset(ep, 0, sizeof(*ep));
    for (int i = 0; i < EPOLL_MAX_ITEMS; i++) ep->items[i].fd = -1;
    ep->refcount = 1;
    for (int i = 0; i < MAX_FD; i++) {
        if (current_proc->ofile[i].type == FD_NONE) {
            current_proc->ofile[i].type    = FD_EPOLL;
            current_proc->ofile[i].epoll   = ep;
            current_proc->ofile[i].cloexec = (flags & 0x80000) ? 1 : 0;
            return i;
        }
    }
    kfree(ep);
    return -24;  /* EMFILE */
}

static int sys_epoll_ctl(registers_t *regs) {
    int epfd = (int)regs->ebx, op = (int)regs->ecx, fd = (int)regs->edx;
    void *uev = (void *)(uintptr_t)regs->esi;
    if (epfd < 0 || epfd >= MAX_FD ||
        current_proc->ofile[epfd].type != FD_EPOLL) return -9;
    if (fd < 0 || fd >= MAX_FD || current_proc->ofile[fd].type == FD_NONE)
        return -9;
    struct epoll *ep = current_proc->ofile[epfd].epoll;

    struct { uint32_t events; uint8_t data[8]; } __attribute__((packed)) ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    if (op != 2 /* EPOLL_CTL_DEL */) {
        if (!uev || !access_ok(uev, 12)) return -14;
        if (copy_from_user(&ev, uev, 12) < 0) return -14;
    }

    switch (op) {
    case 1: /* EPOLL_CTL_ADD */
        for (int i = 0; i < EPOLL_MAX_ITEMS; i++)
            if (ep->items[i].fd == fd) return -17;  /* -EEXIST */
        for (int i = 0; i < EPOLL_MAX_ITEMS; i++)
            if (ep->items[i].fd == -1) {
                ep->items[i].fd = fd;
                ep->items[i].events = ev.events;
                __builtin_memcpy(ep->items[i].data, ev.data, 8);
                return 0;
            }
        return -28;  /* -ENOSPC */
    case 3: /* EPOLL_CTL_MOD */
        for (int i = 0; i < EPOLL_MAX_ITEMS; i++)
            if (ep->items[i].fd == fd) {
                ep->items[i].events = ev.events;
                __builtin_memcpy(ep->items[i].data, ev.data, 8);
                return 0;
            }
        return -2;   /* -ENOENT */
    case 2: /* EPOLL_CTL_DEL */
        for (int i = 0; i < EPOLL_MAX_ITEMS; i++)
            if (ep->items[i].fd == fd) { ep->items[i].fd = -1; return 0; }
        return -2;
    }
    return -22;
}

/* epoll_wait(epfd, events, maxevents, timeout) — EAX=256 (also epoll_pwait/319). */
static int sys_epoll_wait(registers_t *regs) {
    int epfd = (int)regs->ebx;
    void *uevents = (void *)(uintptr_t)regs->ecx;
    int maxevents = (int)regs->edx;
    int toms = (int)regs->esi;
    if (epfd < 0 || epfd >= MAX_FD ||
        current_proc->ofile[epfd].type != FD_EPOLL) return -9;
    if (maxevents <= 0) return -22;
    if (!uevents || !access_ok(uevents, (size_t)maxevents * 12)) return -14;
    struct epoll *ep = current_proc->ofile[epfd].epoll;

    struct kdeadline dl;
    if (toms > 0) deadline_set_ms(&dl, (uint32_t)toms);

    for (;;) {
        int n = 0;
        for (int i = 0; i < EPOLL_MAX_ITEMS && n < maxevents; i++) {
            int wfd = ep->items[i].fd;
            if (wfd < 0 || wfd >= MAX_FD ||
                current_proc->ofile[wfd].type == FD_NONE) continue;
            uint32_t want = ep->items[i].events, rev = 0;
            if ((want & EPOLLIN_K)  && fd_read_ready(wfd))  rev |= EPOLLIN_K;
            if ((want & EPOLLOUT_K) && fd_write_ready(wfd)) rev |= EPOLLOUT_K;
            if (rev) {
                struct { uint32_t events; uint8_t data[8]; } __attribute__((packed)) out;
                out.events = rev;
                __builtin_memcpy(out.data, ep->items[i].data, 8);
                if (copy_to_user((uint8_t *)uevents + (size_t)n * 12, &out, 12) < 0)
                    return -14;
                n++;
            }
        }
        if (n > 0 || toms == 0) return n;
        if (toms > 0 && deadline_expired(&dl)) return 0;
        /* Interruptible like poll (fs/eventpoll.c ep_poll: -EINTR when a
         * signal is pending, no SA_RESTART restart). */
        if (signal_interrupt_pending(current_proc)) return -ERESTARTNOHAND;
        uint32_t cap = 50;
        if (toms > 0) cap = deadline_sleep_ticks(&dl, cap);
        io_wait_sleep(cap);
        if (signal_interrupt_pending(current_proc)) return -ERESTARTNOHAND;
    }
}

/* ── sys_wait4(pid, status, options, rusage) — EAX=114 ─────────────────── */
static int sys_wait4(registers_t *regs) {
    /* Delegate to waitpid; ignore rusage (edx) */
    return sys_waitpid(regs);
}

/* ── sys_sched_yield() — EAX=159 ────────────────────────────────────────── */
static int sys_sched_yield(registers_t *regs) {
    (void)regs;
    yield();
    return 0;
}

/* ── sys_getrlimit / sys_setrlimit — EAX=76/75 (stubs) ──────────────────── */
static int sys_getrlimit(registers_t *regs) {
    /* struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; } */
    int resource = (int)regs->ebx;
    uint32_t *rl = (uint32_t *)(uintptr_t)regs->ecx;
    if (rl) {
        uint32_t krl[2] = { 0xFFFFFFFFU, 0xFFFFFFFFU }; /* RLIM_INFINITY */
        if (resource == 3 /*RLIMIT_STACK*/) krl[0] = 8U * 1024 * 1024;
        int cr = copy_to_user(rl, krl, sizeof(krl));
        if (cr < 0) return cr;
    }
    return 0;
}
static int sys_setrlimit(registers_t *regs) {
    const uint32_t *rl = (const uint32_t *)(uintptr_t)regs->ecx;
    uint32_t krl[2];
    if (!rl) return -14;
    int cr = copy_from_user(krl, rl, sizeof(krl));
    if (cr < 0) return cr;
    return 0;   /* accepted as a no-op; limits are not enforced yet */
}

/* ── sys_prlimit64(pid, resource, new_limit, old_limit) — EAX=340 ───────────
 * glibc 2.34+ uses this (not getrlimit) to size the default thread stack.  The
 * old stub returned 0 without writing old_limit, so glibc read uninitialised
 * stack as RLIMIT_STACK → bogus size → pthread_create EAGAIN.  Report a real,
 * finite RLIMIT_STACK (8 MiB) and RLIM_INFINITY for everything else. */
#define RLIMIT_STACK 3
#define RLIM64_INFINITY 0xFFFFFFFFFFFFFFFFULL
static int sys_prlimit64(registers_t *regs) {
    int resource          = (int)regs->ecx;
    const void *new_limit = (const void *)(uintptr_t)regs->edx;
    void *old_limit       = (void *)(uintptr_t)regs->esi;

    /* We don't enforce limits; accept any new_limit silently. */
    (void)new_limit;

    if (old_limit) {
        struct { uint64_t cur, max; } rl;
        if (resource == RLIMIT_STACK) {
            rl.cur = 8ULL * 1024 * 1024;     /* 8 MiB default stack */
            rl.max = RLIM64_INFINITY;
        } else {
            rl.cur = RLIM64_INFINITY;
            rl.max = RLIM64_INFINITY;
        }
        int cr = copy_to_user(old_limit, &rl, sizeof(rl));
        if (cr < 0) return cr;
    }
    return 0;
}

/* ── sys_ugetrlimit(resource, rlimit*) — EAX=191 ───────────────────────────
 * 32-bit rlimit variant some glibc paths use.  Same fix as prlimit64. */
static int sys_ugetrlimit(registers_t *regs) {
    int resource = (int)regs->ebx;
    uint32_t *rl = (uint32_t *)(uintptr_t)regs->ecx;
    if (rl) {
        uint32_t krl[2];
        if (resource == RLIMIT_STACK) {
            krl[0] = 8U * 1024 * 1024;       /* rlim_cur = 8 MiB */
            krl[1] = 0xFFFFFFFFU;            /* rlim_max = RLIM_INFINITY */
        } else {
            krl[0] = 0xFFFFFFFFU;
            krl[1] = 0xFFFFFFFFU;
        }
        int cr = copy_to_user(rl, krl, sizeof(krl));
        if (cr < 0) return cr;
    }
    return 0;
}

/* ── sys_prctl(option, arg2…) — EAX=172 (stub) ──────────────────────────── */
static int sys_prctl(registers_t *regs) {
    /* [tname] log PR_SET_NAME(15) for firefox threads so we can see which named
     * threads exist (esp. whether the "IPC Launch" thread is ever created). */
    if ((int)regs->ebx == 15 && regs->ecx && current_proc &&
        current_proc->name[0]=='f' && current_proc->name[1]=='i' &&
        current_proc->name[4]=='f') {
        char nm[20];
        __builtin_memset(nm, 0, sizeof(nm));
        if (copy_from_user(nm, (void *)(uintptr_t)regs->ecx, 16) == 0) {
            nm[16] = 0;
            printk("[tname] tid=%d name='%s'\n", current_proc->pid, nm);
        }
    }
    return 0;
}

/* ── sys_sigaltstack(ss, oss) — EAX=186 (stub) ──────────────────────────── */
static int sys_sigaltstack(registers_t *regs) {
    (void)regs;
    return 0;
}

/* ── sys_pread64(fd, buf, count, offset_lo, offset_hi) — EAX=180 ─────────── */
static int sys_pread64(registers_t *regs) {
    int      fd  = (int)regs->ebx;
    char    *buf = (char *)(uintptr_t)regs->ecx;
    int      len = (int)(uint32_t)regs->edx;
    uint32_t off = regs->esi;  /* offset low 32 bits (high 32 bits in edi = 0) */

    if (len < 0 || !access_ok(buf, (size_t)len)) return -14;
    if (fd < 0 || fd >= MAX_FD) return -9;

    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type != FD_FILE || !f->node) return -9;

    return (int)vfs_read(f->node, off, (uint32_t)len, (uint8_t *)buf);
}

/* ── sys_pwrite64(fd, buf, count, offset) — EAX=181 ─────────────────────── */
static int sys_pwrite64(registers_t *regs) {
    int         fd  = (int)regs->ebx;
    const char *buf = (const char *)(uintptr_t)regs->ecx;
    int         len = (int)(uint32_t)regs->edx;
    uint32_t    off = regs->esi;

    if (len < 0 || !access_ok(buf, (size_t)len)) return -14;
    if (fd < 0 || fd >= MAX_FD) return -9;

    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type != FD_FILE || !f->node) return -9;
    if (f->flags == O_RDONLY || !f->node->write_fn) return -9;

    return (int)vfs_write(f->node, off, (uint32_t)len, (const uint8_t *)buf);
}

/* ── sys_openat(dirfd, path, flags, mode) — EAX=295 ─────────────────────── */
static int sys_openat(registers_t *regs) {
    int dirfd = (int)regs->ebx;
    const char *upath = (const char *)(uintptr_t)regs->ecx;
    int flags = (int)regs->edx;
    char path[256], resolved[256];
    int r = copy_user_str(upath, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    int rc = sys_open_kernel_path(resolved, flags);
    fx_lockdbg("openat", path, rc);
    {   /* DEBUG: trace image/icon/theme opens (glibc open() routes here) */
        static int ic = 0;
        if (ic < 120 && (dbg_str_has(path, ".png") || dbg_str_has(path, ".svg") ||
                         dbg_str_has(path, "icon") || dbg_str_has(path, "theme") ||
                         dbg_str_has(path, "hicolor") || dbg_str_has(path, "pixmap"))) {
            ic++;
            printk("[iconopen] %s -> %d\n", path, rc);
        }
    }
    return rc;
}

static int sys_mkdir_kernel_path(const char *path) {
    char dir_path[256], base[256];
    if (path_split(path, dir_path, base) < 0)
        return -2;
    if (base[0] == '\0') return -22;

    vfs_node_t *dir = vfs_open_parent_at(path, dir_path);
    if (!dir || !dir->create_fn) { fx_lockdbg("mkdir", path, -2); return -2; }
    /* Need write+search on the parent to create a directory in it. */
    if (vfs_access_check(dir, current_proc->euid, current_proc->egid,
                         VFS_WANT_W | VFS_WANT_X) < 0)
        { fx_lockdbg("mkdir", path, -13); return -13; }
    int r = dir->create_fn(dir, base, VFS_FLAG_DIR);
    fx_lockdbg("mkdir", path, r);
    if (r < 0) return r;
    /* Stamp the creator as owner with 0777 & ~umask so the user who made
     * the directory can actually write into it (ext2 create hardcodes
     * root-owned 0755 otherwise). */
    vfs_node_t *node = vfs_open_at(path);
    if (node)
        vfs_setattr(node, (0777 & ~current_proc->umask) & 07777,
                    current_proc->uid, current_proc->gid);
    return r;
}

static int sys_unlink_kernel_path(const char *path) {
    char dir_path[256], base[256];
    if (path_split(path, dir_path, base) < 0)
        return -2;
    if (base[0] == '\0') return -22;

    vfs_node_t *dir = vfs_open_parent_at(path, dir_path);
    if (!dir) return -2;
    return vfs_unlink(dir, base);
}

/* Remove an empty directory (Linux fs/namei.c do_rmdir): -ENOTDIR when the
 * target is not a directory, -ENOTEMPTY while it still has entries. */
static int sys_rmdir_kernel_path(const char *path) {
    vfs_node_t *n = vfs_open(path);
    if (!n) return -2;
    if ((n->flags & 0x7U) != VFS_FLAG_DIR) return -20;   /* -ENOTDIR */
    vfs_dirent_t de;
    for (uint32_t i = 0; i < 65536 && vfs_readdir(n, i, &de) == 0; i++) {
        if (de.name[0] == '.' &&
            (de.name[1] == '\0' || (de.name[1] == '.' && de.name[2] == '\0')))
            continue;                                    /* "." and ".." */
        return -39;                                      /* -ENOTEMPTY */
    }
    return sys_unlink_kernel_path(path);
}

/* ── sys_rmdir(path) — EAX=40 ───────────────────────────────────────────── */
static int sys_rmdir(registers_t *regs) {
    char path[256], resolved[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, sizeof(path)) < 0)
        return -14;
    int r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return sys_rmdir_kernel_path(resolved);
}

static int sys_mkdirat(registers_t *regs) {
    int dirfd = (int)regs->ebx;
    const char *upath = (const char *)(uintptr_t)regs->ecx;
    char path[256], resolved[256];
    int r = copy_user_str(upath, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return sys_mkdir_kernel_path(resolved);
}

static int sys_unlinkat(registers_t *regs) {
    int dirfd = (int)regs->ebx;
    const char *upath = (const char *)(uintptr_t)regs->ecx;
    int flags = (int)regs->edx;
    if (flags & ~AT_REMOVEDIR) return -22;
    char path[256], resolved[256];
    int r = copy_user_str(upath, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    /* AT_REMOVEDIR turns unlinkat() into rmdir() (Linux do_unlinkat). */
    if (flags & AT_REMOVEDIR)
        return sys_rmdir_kernel_path(resolved);
    return sys_unlink_kernel_path(resolved);
}

static int sys_fstatat64(registers_t *regs) {
    int dirfd = (int)regs->ebx;
    const char *upath = (const char *)(uintptr_t)regs->ecx;
    struct kstat64 *st = (struct kstat64 *)(uintptr_t)regs->edx;
    if (!access_ok(st, sizeof(*st))) return -14;
    char path[256], resolved[256];
    int r = copy_user_str(upath, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    vfs_node_t *n = vfs_open(resolved);
    fx_lockdbg("fstatat", path, n ? 0 : -2);
    if (!n) return -2;
    struct kstat64 kst;
    fill_kstat64(&kst, n);
    return copy_to_user(st, &kst, sizeof(kst));
}

static int sys_faccessat(registers_t *regs) {
    int dirfd = (int)regs->ebx;
    const char *upath = (const char *)(uintptr_t)regs->ecx;
    int mode = (int)regs->edx;
    if (mode & ~7) return -22;
    char path[256], resolved[256];
    int r = copy_user_str(upath, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    int rc = vfs_open(resolved) ? 0 : -2;
    fx_lockdbg("faccessat", path, rc);
    return rc;
}

static int sys_readlinkat(registers_t *regs) {
    int dirfd = (int)regs->ebx;
    const char *upath = (const char *)(uintptr_t)regs->ecx;
    char *ubuf = (char *)(uintptr_t)regs->edx;
    int bufsiz = (int)regs->esi;
    if (bufsiz <= 0 || !access_ok(ubuf, (uint32_t)bufsiz)) return -14;
    char path[256], resolved[256];
    int r = copy_user_str(upath, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    vfs_node_t *n = vfs_open_nofollow(resolved);
    if (!n) return -2;
    if (n->flags != VFS_FLAG_SYMLINK) return -22;
    char target[512];
    uint32_t len = vfs_read(n, 0, sizeof(target), (uint8_t *)target);
    if ((int)len > bufsiz) len = (uint32_t)bufsiz;
    int cr = copy_to_user(ubuf, target, len);
    return cr < 0 ? cr : (int)len;
}

static int sys_rename_kernel_path(const char *oldpath, const char *newpath);

static int sys_renameat(registers_t *regs) {
    int olddirfd = (int)regs->ebx;
    const char *uold = (const char *)(uintptr_t)regs->ecx;
    int newdirfd = (int)regs->edx;
    const char *unew = (const char *)(uintptr_t)regs->esi;
    char oldpath[256], newpath[256], oldres[256], newres[256];
    int r = copy_user_str(uold, oldpath, sizeof(oldpath));
    if (r < 0) return r;
    r = copy_user_str(unew, newpath, sizeof(newpath));
    if (r < 0) return r;
    r = resolve_path_at_fd(olddirfd, oldpath, oldres, sizeof(oldres));
    if (r < 0) return r;
    r = resolve_path_at_fd(newdirfd, newpath, newres, sizeof(newres));
    if (r < 0) return r;

    return sys_rename_kernel_path(oldres, newres);
}

/* ── sys_clock_nanosleep(clkid, flags, rqtp, rmtp) — EAX=267 / 407 ────────
 * TIMER_ABSTIME sleeps until an absolute instant of the given clock (used by
 * std::this_thread::sleep_until, Rust thread::sleep_until, pthread timed
 * waits); a relative request is nanosleep on that clock.  On EINTR the
 * remainder is written for relative sleeps only (Linux common_nsleep). */
static int do_clock_nanosleep(int clk, int flags, int64_t rsec, int64_t rnsec,
                              uint32_t *rem_sec, uint32_t *rem_nsec, int *write_rem) {
    uint32_t ds, dn; int expired;
    *write_rem = 0;
    int r = knanosleep_deadline(clk, flags, rsec, rnsec, &ds, &dn, &expired);
    if (r < 0) return r;
    if (expired) return 0;
    r = ksleep_until_mono(ds, dn, rem_sec, rem_nsec);
    if (r == -4 && !(flags & TIMER_ABSTIME_K)) *write_rem = 1;
    return r;
}

static int sys_clock_nanosleep(registers_t *regs) {
    int clk = (int)regs->ebx, flags = (int)regs->ecx;
    struct ktimespec *req = (struct ktimespec *)(uintptr_t)regs->edx;
    struct ktimespec *rem = (struct ktimespec *)(uintptr_t)regs->esi;
    if (!req) return -14;
    struct ktimespec kreq;
    int cr = copy_from_user(&kreq, req, sizeof(kreq));
    if (cr < 0) return cr;
    uint32_t rs = 0, rn = 0; int wr;
    int r = do_clock_nanosleep(clk, flags, kreq.tv_sec, kreq.tv_nsec, &rs, &rn, &wr);
    if (wr && rem) {
        struct ktimespec krem = { (int32_t)rs, (int32_t)rn };
        cr = copy_to_user(rem, &krem, sizeof(krem));
        if (cr < 0) return cr;
    }
    return r;
}

static int sys_clock_nanosleep_time64(registers_t *regs) {
    int clk = (int)regs->ebx, flags = (int)regs->ecx;
    void *req = (void *)(uintptr_t)regs->edx;
    void *rem = (void *)(uintptr_t)regs->esi;
    if (!req) return -14;
    struct { int64_t s; int64_t ns; } kreq;
    int cr = copy_from_user(&kreq, req, sizeof(kreq));
    if (cr < 0) return cr;
    uint32_t rs = 0, rn = 0; int wr;
    int r = do_clock_nanosleep(clk, flags, kreq.s, kreq.ns, &rs, &rn, &wr);
    if (wr && rem) {
        struct { int64_t s; int64_t ns; } krem = { (int64_t)rs, (int64_t)rn };
        cr = copy_to_user(rem, &krem, sizeof(krem));
        if (cr < 0) return cr;
    }
    return r;
}

/* ── sys_getresuid32 / sys_getresgid32 — EAX=209/211 ────────────────────────
 * Firefox/glibc query these during privilege checks.  Report all three ids
 * equal to the effective id (we don't track a separate saved id). */
static int sys_getresuid32(registers_t *regs) {
    uint32_t *r = (uint32_t *)(uintptr_t)regs->ebx;   /* ruid */
    uint32_t *e = (uint32_t *)(uintptr_t)regs->ecx;   /* euid */
    uint32_t *s = (uint32_t *)(uintptr_t)regs->edx;   /* suid */
    uint32_t ru = current_proc->uid, eu = current_proc->euid;
    if (r && access_ok(r, 4) && copy_to_user(r, &ru, 4) < 0) return -14;
    if (e && access_ok(e, 4) && copy_to_user(e, &eu, 4) < 0) return -14;
    if (s && access_ok(s, 4) && copy_to_user(s, &eu, 4) < 0) return -14;
    return 0;
}
static int sys_getresgid32(registers_t *regs) {
    uint32_t *r = (uint32_t *)(uintptr_t)regs->ebx;
    uint32_t *e = (uint32_t *)(uintptr_t)regs->ecx;
    uint32_t *s = (uint32_t *)(uintptr_t)regs->edx;
    uint32_t rg = current_proc->gid, eg = current_proc->egid;
    if (r && access_ok(r, 4) && copy_to_user(r, &rg, 4) < 0) return -14;
    if (e && access_ok(e, 4) && copy_to_user(e, &eg, 4) < 0) return -14;
    if (s && access_ok(s, 4) && copy_to_user(s, &eg, 4) < 0) return -14;
    return 0;
}

/* ── sys_sched_getaffinity(pid, len, mask*) — EAX=242 ───────────────────────
 * Single-CPU system: report a mask with only CPU 0 set.  Firefox sizes its
 * thread pools from this; returning 1 CPU is correct for us. */
static int sys_sched_getaffinity(registers_t *regs) {
    uint32_t len  = regs->ecx;
    uint8_t *mask = (uint8_t *)(uintptr_t)regs->edx;
    if (len < 8) return -22;              /* glibc passes a multiple of 8 */
    if (!access_ok(mask, len)) return -14;
    /* Real Linux zeroes the whole user mask but RETURNS the kernel cpumask
     * size (8 bytes here, one unsigned long for ≤64 CPUs), not the user length.
     * glibc treats the return value as the significant mask size. */
    uint8_t zero[128];
    uint32_t n = len > sizeof(zero) ? sizeof(zero) : len;
    __builtin_memset(zero, 0, n);
    zero[0] = 0x01;                       /* CPU 0 online */
    if (copy_to_user(mask, zero, n) < 0) return -14;
    return 8;                             /* kernel cpumask size */
}

/* ── sys_sched_getattr(pid, attr*, size, flags) — EAX=352 ───────────────────
 * Linux struct sched_attr; report SCHED_NORMAL, nice 0, priority 0 (our only
 * scheduling class).  Firefox queries this during thread-pool/priority setup;
 * returning ENOSYS makes it fall back, but reporting the real (default) policy
 * is the Linux-faithful answer.  Linux writes attr->size = the kernel struct
 * size and zeroes the policy-specific fields for a SCHED_NORMAL task. */
static int sys_sched_getattr(registers_t *regs) {
    void    *uattr = (void *)(uintptr_t)regs->ecx;
    uint32_t size  = regs->edx;
    uint32_t flags = regs->esi;
    if (flags) return -22;                 /* -EINVAL: no flags defined */
    if (size < 48) return -22;             /* -EINVAL: buffer too small (Linux SCHED_ATTR_SIZE_VER0) */
    if (!access_ok(uattr, size)) return -14;
    /* struct sched_attr { u32 size; u32 policy; u64 flags; s32 nice;
     *   u32 priority; u64 runtime; u64 deadline; u64 period; ... } */
    struct {
        uint32_t size, policy;
        uint64_t flags;
        int32_t  nice;
        uint32_t priority;
        uint64_t runtime, deadline, period;
    } a;
    __builtin_memset(&a, 0, sizeof(a));
    a.size = 48;                           /* SCHED_ATTR_SIZE_VER0 */
    a.policy = 0;                          /* SCHED_NORMAL */
    a.nice = 0;
    a.priority = 0;
    uint32_t n = size < sizeof(a) ? size : sizeof(a);
    if (copy_to_user(uattr, &a, n) < 0) return -14;
    return 0;
}

/* ── sys_sched_setattr(pid, attr*, flags) — EAX=351 ─────────────────────────
 * We have a single SCHED_NORMAL round-robin class; accept the request and
 * ignore the parameters (like Linux silently accepting a same-policy set).
 * Reject only obviously-bad pointers and real-time policies we can't honour. */
static int sys_sched_setattr(registers_t *regs) {
    void    *uattr = (void *)(uintptr_t)regs->ecx;
    uint32_t flags = regs->edx;
    if (flags) return -22;                 /* -EINVAL */
    if (!access_ok(uattr, 8)) return -14;  /* need at least size+policy */
    uint32_t hdr[2] = { 0, 0 };
    if (copy_from_user(hdr, uattr, sizeof(hdr)) < 0) return -14;
    /* hdr[1] = sched_policy; reject real-time/deadline (1=FIFO,2=RR,6=DEADLINE) */
    if (hdr[1] == 1 || hdr[1] == 2 || hdr[1] == 6) return -1;  /* -EPERM */
    return 0;                              /* accept SCHED_NORMAL/BATCH/IDLE, ignore */
}

/* ── sys_clock_getres(clkid, timespec*) — EAX=266 / 406 ──────────────────── */
static int sys_clock_getres(registers_t *regs) {
    uint32_t ns;
    int r = kclock_res((int)regs->ebx, &ns);
    if (r < 0) return r;
    void *uts = (void *)(uintptr_t)regs->ecx;
    if (uts) {
        struct { int32_t sec; int32_t nsec; } ts = { 0, (int32_t)ns };
        if (copy_to_user(uts, &ts, sizeof(ts)) < 0) return -14;
    }
    return 0;
}
static int sys_clock_getres_time64(registers_t *regs) {
    uint32_t ns;
    int r = kclock_res((int)regs->ebx, &ns);
    if (r < 0) return r;
    void *uts = (void *)(uintptr_t)regs->ecx;
    if (uts) {
        struct { int64_t sec; int64_t nsec; } ts = { 0, (int64_t)ns };
        if (copy_to_user(uts, &ts, sizeof(ts)) < 0) return -14;
    }
    return 0;
}

/* ── sys_statfs64(path, sz, buf) / fstatfs64(fd, sz, buf) — EAX=268/269 ──────
 * Firefox checks free space / fs type for its profile + cache.  Report a
 * generic, roomy ext2-like filesystem. */
static int sys_statfs64_fill(void *ubuf, uint32_t bufsz) {
    /* struct statfs64 (i386): f_type, f_bsize, f_blocks, f_bfree, f_bavail,
     * f_files, f_ffree, f_fsid[2], f_namelen, f_frsize, f_flags, f_spare[4].
     * 64-bit count fields. */
    struct kstatfs64 {
        uint32_t f_type, f_bsize;
        uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
        uint32_t f_fsid[2];
        uint32_t f_namelen, f_frsize, f_flags, f_spare[4];
    } s;
    __builtin_memset(&s, 0, sizeof(s));
    s.f_type    = 0xEF53;              /* EXT2_SUPER_MAGIC */
    s.f_bsize   = 4096;
    s.f_blocks  = 256 * 1024;         /* ~1 GiB */
    s.f_bfree   = 192 * 1024;
    s.f_bavail  = 192 * 1024;
    s.f_files   = 65536;
    s.f_ffree   = 60000;
    s.f_namelen = 255;
    s.f_frsize  = 4096;
    if (!access_ok(ubuf, bufsz)) return -14;
    uint32_t n = bufsz < sizeof(s) ? bufsz : sizeof(s);
    if (copy_to_user(ubuf, &s, n) < 0) return -14;
    return 0;
}
static int sys_statfs64(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0) return -14;
    return sys_statfs64_fill((void *)(uintptr_t)regs->edx, regs->ecx);
}
static int sys_fstatfs64(registers_t *regs) {
    return sys_statfs64_fill((void *)(uintptr_t)regs->edx, regs->ecx);
}

/* ── sys_memfd_create(name, flags) — EAX=356 ────────────────────────────────
 * Firefox/Chromium IPC use memfd_create for shared memory.  We back it with an
 * anonymous file in the /tmp tmpfs: a real fd that supports ftruncate + mmap.
 * (Cross-process sharing via SCM_RIGHTS fd-passing is a separate step; this
 * makes the single-process path work so the parent stops aborting on shm.) */
static int sys_open_kernel_path(const char *path, int flags);
static int sys_memfd_create(registers_t *regs) {
    static uint32_t memfd_seq = 0;
    char name[64];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, name, sizeof(name)) < 0)
        name[0] = '\0';
    /* Build a unique anonymous path under /tmp (tmpfs, writable). */
    char path[96];
    uint32_t seq = ++memfd_seq;
    const char *pfx = "/tmp/.memfd-";
    int p = 0;
    for (const char *s = pfx; *s; s++) path[p++] = *s;
    /* append decimal seq */
    char num[12]; int n = 0;
    if (seq == 0) num[n++] = '0';
    while (seq) { num[n++] = '0' + (seq % 10); seq /= 10; }
    while (n) path[p++] = num[--n];
    path[p] = '\0';
    int fd = sys_open_kernel_path(path, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0) return fd;
    /* MFD_CLOEXEC = 0x0001 (Linux mm/memfd.c hands O_CLOEXEC to get_unused_fd). */
    current_proc->ofile[fd].cloexec = (regs->ecx & 0x1) ? 1 : 0;
    /* Back the file with the shared page registry rather than a contiguous
     * tmpfs buffer, so the file's pages and every MAP_SHARED mapping of it are
     * the same frames (Linux shmem — see shmem_read above). */
    vfs_node_t *mn = current_proc->ofile[fd].node;
    if (mn) {
        mn->read_fn     = shmem_read;
        mn->write_fn    = shmem_write;
        mn->truncate_fn = shmem_truncate;
        mn->size        = 0;
    }
    return fd;
}

/* Helper: find a process by pid in the global table. */
static struct proc *proc_find_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (ptable[i].state != PROC_UNUSED && ptable[i].pid == pid)
            return &ptable[i];
    return NULL;
}

/* ── sys_setpgid(pid, pgid) — EAX=57 ─────────────────────────────────────── */
static int sys_setpgid(registers_t *regs) {
    int pid  = (int)regs->ebx;
    int pgid = (int)regs->ecx;
    if (pgid < 0) return -22;   /* -EINVAL */
    struct proc *p = (pid == 0) ? current_proc : proc_find_by_pid(pid);
    if (!p) return -3;           /* -ESRCH */
    p->pgrp = (pgid == 0) ? p->pid : pgid;
    return 0;
}

/* ── sys_getpgrp() — EAX=65 ──────────────────────────────────────────────── */
static int sys_getpgrp(registers_t *regs) {
    (void)regs;
    return current_proc ? current_proc->pgrp : 1;
}

/* ── sys_getpgid(pid) — EAX=132 ──────────────────────────────────────────── */
static int sys_getpgid(registers_t *regs) {
    int pid = (int)regs->ebx;
    struct proc *p = (pid == 0) ? current_proc : proc_find_by_pid(pid);
    if (!p) return -3;   /* -ESRCH */
    return p->pgrp;
}

/* Thread-directed signal (Linux do_tkill -> do_send_specific): queued on
 * exactly the thread `tid`, which must belong to thread group `tgid` (or any
 * group when tgid is -1, i.e. tkill).  Whether it is fatal for the whole group
 * is decided at delivery: a SIG_DFL fatal signal ends the group; a handled one
 * runs the handler on this thread; a blocked one stays pending on it. */
static int do_tkill(int tgid, int tid, int sig) {
    if (sig < 0 || sig >= NSIGS) return -22;
    if (tid <= 0) return -22;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) continue;
        if (p->pid != tid) continue;
        if (tgid > 0 && p->tgid != tgid) return -3;   /* -ESRCH */
        if (sig) signal_send(p, sig);
        return 0;
    }
    return -3;  /* -ESRCH */
}

/* ── sys_tgkill(tgid, tid, sig) — EAX=270 ───────────────────────────────── */
static int sys_tgkill(registers_t *regs) {
    int tgid = (int)regs->ebx;
    if (tgid <= 0) return -22;
    return do_tkill(tgid, (int)regs->ecx, (int)regs->edx);
}

/* ── sys_tkill(tid, sig) — EAX=238 (musl raise()/pthread_kill use this) ──── */
static int sys_tkill(registers_t *regs) {
    return do_tkill(-1, (int)regs->ebx, (int)regs->ecx);
}

/* ── sys_pipe2(fds[2], flags) — EAX=331 ─────────────────────────────────── */
static int sys_pipe2(registers_t *regs) {
    int flags = (int)regs->ecx;
    /* Create the pipe using sys_pipe */
    registers_t fake = *regs;
    int r = sys_pipe(&fake);
    if (r < 0) return r;
    /* Apply flags: O_CLOEXEC = 0x80000, O_NONBLOCK = 0x800 */
    int *fds = (int *)(uintptr_t)regs->ebx;
    if (flags & (O_CLOEXEC | 0x800)) {
        int kfds[2];
        int cr = copy_from_user(kfds, fds, sizeof(kfds));
        if (cr < 0) return cr;
        if (flags & O_CLOEXEC) {
            current_proc->ofile[kfds[0]].cloexec = 1;
            current_proc->ofile[kfds[1]].cloexec = 1;
        }
        if (flags & 0x800) {
            current_proc->ofile[kfds[0]].flags |= 0x800;
            current_proc->ofile[kfds[1]].flags |= 0x800;
        }
    }
    return 0;
}

/* ── sys_readv(fd, iov, iovcnt) — EAX=145 ───────────────────────────────── */
static int sys_readv(registers_t *regs) {
    int fd     = (int)regs->ebx;
    const uint32_t *iov = (const uint32_t *)(uintptr_t)regs->ecx;
    int iovcnt = (int)regs->edx;

    if (iovcnt < 0 || iovcnt > 1024) return -22;
    if (!access_ok(iov, (size_t)iovcnt * 8)) return -14;
    if (fd < 0 || fd >= MAX_FD) return -9;

    int total = 0;
    for (int i = 0; i < iovcnt; i++) {
        uint32_t kiov[2];
        int cr = copy_from_user(kiov, &iov[i * 2], sizeof(kiov));
        if (cr < 0) return cr;
        char *base = (char *)(uintptr_t)kiov[0];
        int   len  = (int)kiov[1];
        if (len <= 0) continue;
        if (!access_ok(base, (size_t)len)) return -14;
        registers_t fake = *regs;
        fake.ebx = (uint32_t)fd;
        fake.ecx = (uint32_t)(uintptr_t)base;
        fake.edx = (uint32_t)len;
        int n = sys_read(&fake);
        if (n < 0) return n;
        total += n;
        if (n < len) break;  /* short read — don't continue */
    }
    return total;
}

/* ── sys_flock(fd, operation) — EAX=143 (stub) ──────────────────────────── */
static int sys_flock(registers_t *regs) {
    int fd = (int)regs->ebx;
    int op = (int)regs->ecx;
    if (fd < 0 || fd >= MAX_FD) return -9;
    if (current_proc->ofile[fd].type == FD_NONE) return -9;
    if (op & ~(1 | 2 | 4 | 8)) return -22;
    return 0;
}

/* ── sys__llseek(fd, off_high, off_low, loff_t *result, whence) — EAX=140 ─ */
static int sys_llseek(registers_t *regs) {
    int      fd       = (int)regs->ebx;
    /* off_high in ecx, off_low in edx; we only support 32-bit offsets */
    uint32_t off_low  = regs->edx;
    uint64_t *result  = (uint64_t *)(uintptr_t)regs->esi;
    int       whence  = (int)regs->edi;

    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type == FD_NONE) return -9;
    if (f->type == FD_PIPE_R || f->type == FD_PIPE_W) return -29;

    uint32_t fsize = f->node ? f->node->size : 0;
    uint32_t new_off;
    switch (whence) {
    case 0: new_off = off_low; break;
    case 1: new_off = f->offset + off_low; break;
    case 2: new_off = fsize + off_low; break;
    default: return -22;
    }
    f->offset = new_off;
    if (result) {
        uint64_t kres = (uint64_t)new_off;
        int cr = copy_to_user(result, &kres, sizeof(kres));
        if (cr < 0) return cr;
    }
    return 0;
}

/* ── sys_sysinfo(struct sysinfo *) — EAX=116 (stub) ─────────────────────── */
static int sys_sysinfo(registers_t *regs) {
    /* struct sysinfo is 64 bytes; zero it and fill plausible values */
    uint32_t *si = (uint32_t *)(uintptr_t)regs->ebx;
    if (!si || !access_ok(si, 64)) return -14;
    uint32_t ksi[16];
    __builtin_memset(ksi, 0, sizeof(ksi));
    ksi[0] = 0;         /* uptime seconds */
    ksi[4] = 128*1024*1024; /* totalram */
    ksi[5] = 64*1024*1024;  /* freeram */
    ksi[11] = 1;        /* mem_unit = 1 byte */
    return copy_to_user(si, ksi, sizeof(ksi));
}

/* Resolve the physical address of a user futex word (page + offset), or 0 if the
 * page is not present.  Used to key process-SHARED futexes by physical page so a
 * cross-process (shm) primitive matches across address spaces, exactly like Linux
 * get_futex_key for the shared case. */
static uint32_t futex_resolve_phys(uint32_t uaddr) {
    uint32_t page = uaddr & ~0xFFFU;
    if (page >= 0xC0000000U) return 0;              /* kernel space — not a futex */
    if (!(*paging_get_pde(page) & PAGE_PRESENT)) return 0;
    uint32_t pte = *paging_get_pte(page);
    if (!(pte & PAGE_PRESENT)) return 0;
    return (pte & ~0xFFFU) | (uaddr & 0xFFF);
}

/* Address-space-scoped futex wake — replaces the bare wake_up_n(uaddr) for futex
 * ops.  THE FIX: private futexes are matched by (tgid, uaddr) and shared futexes
 * by physical page, so a private condvar signal in one process is NEVER delivered
 * to a same-virtual-address waiter in another process (ASLR is off, so all Firefox
 * processes share virtual addresses — the unscoped wake_up_n mis-routed the launch-
 * completion condvar signal to a foreign process's waiter and deadlocked the
 * content-process launch).  Mirrors Linux get_futex_key: (mm,addr) private / phys
 * shared.  Wakes up to n waiters, OLDEST sleep_seq first (per-key FIFO, preserving
 * glibc's happens-before condvar ordering). */
static int futex_wake_n(uint32_t uaddr, int is_private, uint32_t phys,
                        int tgid, int n) {
    if (n <= 0) return 0;
    /* A waiter matches this wake iff it is a futex waiter on the same key. */
    #define FUTEX_MATCH(p)                                                     \
        ((p)->state == PROC_SLEEPING && (p)->futex_wait &&                     \
         (is_private                                                          \
            ? (!(p)->futex_shared && (p)->tgid == tgid &&                      \
               (p)->sleep_chan == (void *)(uintptr_t)uaddr)                    \
            : ((p)->futex_shared && phys && (p)->futex_phys == phys)))

    int matches = 0;
    for (int i = 0; i < MAX_PROCS; i++)
        if (FUTEX_MATCH(&ptable[i])) matches++;

    int woken = 0;
    if (n >= matches) {                 /* wake all matching (single pass) */
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            if (FUTEX_MATCH(p)) {
                p->sleep_chan = (void *)0; p->wake_tick = 0;
                p->futex_wait = 2;         /* woken by a FUTEX_WAKE */
                p->state = PROC_RUNNABLE; woken++;
            }
        }
    } else {                            /* wake the n OLDEST (min sleep_seq) */
        while (woken < n) {
            struct proc *best = (void *)0;
            for (int i = 0; i < MAX_PROCS; i++) {
                struct proc *p = &ptable[i];
                if (FUTEX_MATCH(p) && (!best || p->sleep_seq < best->sleep_seq))
                    best = p;
            }
            if (!best) break;
            best->sleep_chan = (void *)0; best->wake_tick = 0;
            best->futex_wait = 2;          /* woken by a FUTEX_WAKE */
            best->state = PROC_RUNNABLE; woken++;
        }
    }
    #undef FUTEX_MATCH
    if (woken > 0) { extern volatile int g_resched_pending; g_resched_pending = 1; }
    return woken;
}

/* ── sys_futex(uaddr, op, val, timeout, uaddr2, val3) — EAX=240 ─────────── */
static int sys_futex(registers_t *regs, int time64) {
    uint32_t *uaddr = (uint32_t *)(uintptr_t)regs->ebx;
    int       raw_op = (int)regs->ecx;
    int       clock_realtime = (raw_op & 256) ? 1 : 0;  /* FUTEX_CLOCK_REALTIME */
    int       is_private = (raw_op & 128) ? 1 : 0;      /* FUTEX_PRIVATE_FLAG */
    int       op    = raw_op & ~(128 | 256);            /* strip PRIVATE+CLOCK */
    uint32_t  val   = regs->edx;
    uint32_t  utmo  = regs->esi;                         /* timeout timespec* */

    if (!access_ok(uaddr, sizeof(uint32_t))) return -14;  /* -EFAULT */

    /* FUTEX_WAIT(0, relative timeout) and FUTEX_WAIT_BITSET(9, ABSOLUTE timeout)
     * both block until woken or the timeout fires.  CRITICAL: the BITSET timeout
     * MUST be honoured — pthread_cond_timedwait uses WAIT_BITSET, and ignoring
     * its timeout makes a thread that's waiting "up to N ms for the next frame"
     * sleep FOREVER (the compositor/refresh-driver hang).  Convert the timeout
     * to a relative tick deadline (wake_tick) so scheduler_tick wakes us. */
    if (op == 0 || op == 9) {  /* FUTEX_WAIT / FUTEX_WAIT_BITSET */
        uint32_t cur = 0;
        int cr = copy_from_user(&cur, uaddr, sizeof(cur));
        if (cr < 0) return cr;
        if (cur != val) return -11;  /* -EAGAIN */

        if (utmo) {
            int64_t tsec = 0; int32_t tnsec = 0;
            if (time64) {       /* futex_time64: 64-bit { s64 sec; s64 nsec } */
                struct { int64_t s, ns; } ts;
                if (copy_from_user(&ts, (void *)(uintptr_t)utmo, sizeof(ts)) < 0)
                    return -14;
                tsec = ts.s; tnsec = (int32_t)ts.ns;
            } else {            /* 32-bit { s32 sec; s32 nsec } */
                struct { int32_t s, ns; } ts;
                if (copy_from_user(&ts, (void *)(uintptr_t)utmo, sizeof(ts)) < 0)
                    return -14;
                tsec = ts.s; tnsec = ts.ns;
            }
            /* Both forms become an ABSOLUTE deadline on the fine-grained
             * monotonic clock, and the wake tick is the first tick at or
             * AFTER it (Linux arms an hrtimer on the absolute deadline, so a
             * futex timeout never fires early).  The old code converted an
             * absolute deadline through the 10 ms tick counter and then
             * TRUNCATED the relative milliseconds to whole ticks, which could
             * wake a waiter up to a tick early. */
            struct kdeadline dl;
            if (op == 9) {      /* WAIT_BITSET → timeout is ABSOLUTE */
                int64_t ds = tsec;
                if (clock_realtime) ds -= (int64_t)rtc_boot_epoch();
                if (ds < 0) return -110;                    /* -ETIMEDOUT */
                if (ds > 2000000) ds = 2000000;             /* clamp */
                dl.sec  = (uint32_t)ds;
                dl.nsec = (uint32_t)tnsec;
                if (dl.nsec >= 1000000000U) return -22;     /* -EINVAL */
                if (deadline_expired(&dl)) return -110;     /* already past */
            } else {            /* WAIT → timeout is RELATIVE */
                if (tsec > 2000000) tsec = 2000000;
                if (tnsec < 0 || tnsec >= 1000000000) return -22;
                if (tsec == 0 && tnsec == 0) return -110;   /* zero: immediate */
                deadline_set(&dl, (uint32_t)tsec, (uint32_t)tnsec);
            }
            current_proc->wake_tick = deadline_wake_tick(&dl);
        }
        current_proc->futex_wait = 1;
        /* Address-space-scoped futex key (mirrors Linux get_futex_key): private
         * futexes match by (tgid, uaddr); shared by physical page.  A shared futex
         * whose page can't be resolved falls back to private-by-(tgid,uaddr) so we
         * never drop the waiter.  THIS is what stops a private condvar signal in
         * one process from being mis-delivered to a same-vaddr waiter in another. */
        {
            uint32_t fphys = is_private ? 0
                           : futex_resolve_phys((uint32_t)(uintptr_t)uaddr);
            current_proc->futex_shared = (!is_private && fphys) ? 1 : 0;
            current_proc->futex_phys   = fphys;
        }
        int timed_out = sleep_on((void *)uaddr);
        int woken = (current_proc->futex_wait == 2);
        current_proc->futex_wait = 0;
        /* Linux kernel/futex/waitwake.c futex_wait(): a wake by FUTEX_WAKE
         * returns 0 (checked first: "If we were woken (and unqueued), we
         * succeeded"); deadline expiry returns -ETIMEDOUT; a signal returns
         * -ERESTARTSYS (EINTR to the caller unless SA_RESTART re-issues the
         * call).  Nothing else ends the wait, so glibc never sees a bare 0
         * without a real wake. */
        if (woken) return 0;
        if (timed_out) return -110;                        /* -ETIMEDOUT */
        if (signal_interrupt_pending(current_proc)) return -4;  /* -EINTR */
        return 0;
    }
    if (op == 1 || op == 10) {  /* FUTEX_WAKE / FUTEX_WAKE_BITSET */
        if ((int)val <= 0) return 0;
        {
            uint32_t wphys = is_private ? 0
                           : futex_resolve_phys((uint32_t)(uintptr_t)uaddr);
            int woke = futex_wake_n((uint32_t)(uintptr_t)uaddr,
                                    is_private || !wphys, wphys,
                                    current_proc->tgid, (int)val);
            return woke;
        }
    }
    if (op == 3 || op == 4) {  /* FUTEX_REQUEUE / FUTEX_CMP_REQUEUE
        * glibc's pre-2.34 pthread_cond_broadcast/signal moves condvar waiters to
        * the mutex futex with CMP_REQUEUE.  We don't physically requeue; instead
        * we WAKE both the `val` to-wake and the `val2` to-requeue waiters — they
        * re-evaluate their predicate and re-block on the correct (mutex) futex.
        * Over-waking is safe; returning ENOSYS here deadlocks every cond-waiter. */
        uint32_t val2 = regs->esi;                 /* # to requeue (timeout slot) */
        if (op == 4) {                             /* CMP_REQUEUE: check *uaddr */
            uint32_t cur = 0;
            if (copy_from_user(&cur, uaddr, sizeof(cur)) < 0) return -14;
            if (cur != regs->ebp) return -11;      /* val3 in ebp; -EAGAIN */
        }
        int64_t n = (int64_t)(int)val + (int64_t)(int)val2;
        if (n < 0) n = 0;
        if (n > 0x7fffffff) n = 0x7fffffff;
        uint32_t rphys = is_private ? 0
                       : futex_resolve_phys((uint32_t)(uintptr_t)uaddr);
        int woke = futex_wake_n((uint32_t)(uintptr_t)uaddr, is_private || !rphys,
                                rphys, current_proc->tgid, (int)n);
        return woke;
    }
    if (op == 5) {  /* FUTEX_WAKE_OP — wake both futexes conservatively */
        uint32_t uaddr2 = regs->edi;
        uint32_t val2   = regs->esi;
        uint32_t p1 = is_private ? 0 : futex_resolve_phys((uint32_t)(uintptr_t)uaddr);
        int woke = futex_wake_n((uint32_t)(uintptr_t)uaddr, is_private || !p1, p1,
                                current_proc->tgid, (int)val);
        if (uaddr2) {
            uint32_t p2 = is_private ? 0 : futex_resolve_phys(uaddr2);
            woke += futex_wake_n(uaddr2, is_private || !p2, p2,
                                 current_proc->tgid, (int)val2);
        }
        return woke;
    }
    return -38;  /* -ENOSYS for anything else */
}

/* ── sys_getrandom(buf, buflen, flags) — EAX=355 ────────────────────────── */
static int sys_getrandom(registers_t *regs) {
    uint8_t *buf    = (uint8_t *)(uintptr_t)regs->ebx;
    uint32_t buflen = regs->ecx;
    uint32_t flags  = regs->edx;
    uint8_t tmp[64];

    if (flags & ~7U) return -22;   /* allow GRND_NONBLOCK|GRND_RANDOM|GRND_INSECURE */
    if (!access_ok(buf, buflen)) return -14;

    uint32_t done = 0;
    while (done < buflen) {
        uint32_t chunk = buflen - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        random_get_bytes(tmp, chunk);
        int cr = copy_to_user(buf + done, tmp, chunk);
        if (cr < 0) return cr;
        done += chunk;
    }
    return (int)buflen;
}

/* ── sys_rename(oldpath, newpath) — EAX=38 ──────────────────────────────── */
static int sys_rename_kernel_path(const char *oldpath, const char *newpath) {
    char old_dir[256], old_base[256];
    char new_dir[256], new_base[256];
    if (path_split(oldpath, old_dir, old_base) < 0) return -2;
    if (path_split(newpath, new_dir, new_base) < 0) return -2;
    if (old_base[0] == '\0' || new_base[0] == '\0') return -22;

    vfs_node_t *src_dir = vfs_open_parent_at(oldpath, old_dir);
    vfs_node_t *dst_dir = vfs_open_parent_at(newpath, new_dir);
    if (!src_dir || !dst_dir) return -2;

    /* Find the source node */
    vfs_node_t *src = vfs_finddir(src_dir, old_base);
    if (!src) return -2;

    /* If the target already exists, unlink it */
    vfs_unlink(dst_dir, new_base);

    /* Copy data into a new node then unlink the old */
    if (!dst_dir->create_fn) return -1;
    if (dst_dir->create_fn(dst_dir, new_base, src->flags) < 0) return -1;
    vfs_node_t *dst = vfs_finddir(dst_dir, new_base);
    if (!dst) return -1;

    if (!(src->flags & VFS_FLAG_DIR) && src->size > 0 && src->read_fn && dst->write_fn) {
        /* Copy file contents in 4KiB chunks */
        uint8_t tmp_buf[4096];
        uint32_t copied = 0;
        while (copied < src->size) {
            uint32_t chunk = src->size - copied;
            if (chunk > 4096) chunk = 4096;
            uint32_t got = vfs_read(src, copied, chunk, tmp_buf);
            if (!got) break;
            vfs_write(dst, copied, got, tmp_buf);
            copied += got;
        }
        if (dst->truncate_fn) dst->truncate_fn(dst, src->size);
    }

    /* Remove old entry */
    vfs_unlink(src_dir, old_base);
    return 0;
}

static int sys_rename(registers_t *regs) {
    char oldpath[256], newpath[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, oldpath, 256) < 0)
        return -14;
    if (copy_user_str((const char *)(uintptr_t)regs->ecx, newpath, 256) < 0)
        return -14;

    char oldres[256], newres[256];
    int r = resolve_path_at_fd(AT_FDCWD, oldpath, oldres, sizeof(oldres));
    if (r < 0) return r;
    r = resolve_path_at_fd(AT_FDCWD, newpath, newres, sizeof(newres));
    if (r < 0) return r;
    return sys_rename_kernel_path(oldres, newres);
}

/* ── chmod / fchmod / chown / fchown / lchown ───────────────────────────── */
/* chmod: only the file owner or root may change the mode. */
static int do_chmod_node(vfs_node_t *n, uint32_t mode) {
    if (!n) return -2;
    if (current_proc->euid != 0 && current_proc->euid != n->uid)
        return -1;  /* -EPERM */
    return vfs_setattr(n, mode & 07777, n->uid, n->gid);
}

/* chown: changing the owning uid is root-only; the owner may chgrp to a
 * group (we accept any here).  uid/gid of -1 (0xFFFFFFFF) means "unchanged". */
static int do_chown_node(vfs_node_t *n, uint32_t uid, uint32_t gid) {
    uint32_t new_uid = n->uid, new_gid = n->gid;
    if (!n) return -2;
    if (uid != 0xFFFFFFFFU && uid != n->uid) {
        if (current_proc->euid != 0) return -1; /* -EPERM */
        new_uid = uid;
    }
    if (gid != 0xFFFFFFFFU && gid != n->gid) {
        if (current_proc->euid != 0 && current_proc->euid != n->uid)
            return -1;
        new_gid = gid;
    }
    return vfs_setattr(n, n->mask, new_uid, new_gid);
}

static int sys_chmod(registers_t *regs) {
    char path[256], resolved[256];
    int r = copy_user_str((const char *)(uintptr_t)regs->ebx, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return do_chmod_node(vfs_open(resolved), (uint32_t)regs->ecx);
}

static int sys_fchmod(registers_t *regs) {
    int fd = (int)regs->ebx;
    if (fd < 0 || fd >= MAX_FD) return -9;
    if (current_proc->ofile[fd].type == FD_NONE) return -9;
    return do_chmod_node(current_proc->ofile[fd].node, (uint32_t)regs->ecx);
}

static int sys_chown(registers_t *regs) {
    char path[256], resolved[256];
    int r = copy_user_str((const char *)(uintptr_t)regs->ebx, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return do_chown_node(vfs_open(resolved), (uint32_t)regs->ecx,
                         (uint32_t)regs->edx);
}

static int sys_fchown(registers_t *regs) {
    int fd = (int)regs->ebx;
    if (fd < 0 || fd >= MAX_FD) return -9;
    if (current_proc->ofile[fd].type == FD_NONE) return -9;
    return do_chown_node(current_proc->ofile[fd].node, (uint32_t)regs->ecx,
                         (uint32_t)regs->edx);
}

static int sys_lchown(registers_t *regs) {
    char path[256], resolved[256];
    int r = copy_user_str((const char *)(uintptr_t)regs->ebx, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return do_chown_node(vfs_open_nofollow(resolved), (uint32_t)regs->ecx,
                         (uint32_t)regs->edx);
}

static int sys_fchmodat(registers_t *regs) {
    int dirfd = (int)regs->ebx;
    const char *upath = (const char *)(uintptr_t)regs->ecx;
    char path[256], resolved[256];
    int r = copy_user_str(upath, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return do_chmod_node(vfs_open(resolved), (uint32_t)regs->edx);
}

/* ── sys_dup3(oldfd, newfd, flags) — EAX=330 ────────────────────────────── */
static int sys_dup3(registers_t *regs) {
    int oldfd = (int)regs->ebx;
    int newfd = (int)regs->ecx;
    int flags = (int)regs->edx;

    if (oldfd == newfd) return -22;  /* dup3 requires oldfd != newfd */
    registers_t fake = *regs;
    fake.ebx = (uint32_t)oldfd;
    fake.ecx = (uint32_t)newfd;
    int r = sys_dup2(&fake);
    if (r < 0) return r;
    /* Apply O_CLOEXEC if requested */
    if ((flags & O_CLOEXEC) && newfd >= 0 && newfd < MAX_FD)
        current_proc->ofile[newfd].cloexec = 1;
    return r;
}

/* ── sys_writev(fd, iov, iovcnt) — EAX=146 ─────────────────────────────── */
static int sys_writev(registers_t *regs) {
    int fd     = (int)regs->ebx;
    /* struct iovec { void *iov_base; size_t iov_len; } */
    const uint32_t *iov = (const uint32_t *)(uintptr_t)regs->ecx;
    int iovcnt = (int)regs->edx;

    if (iovcnt < 0 || iovcnt > 1024) return -22;
    if (!access_ok(iov, (size_t)iovcnt * 8)) return -14;
    if (fd < 0 || fd >= MAX_FD) return -9;

    int total = 0;
    for (int i = 0; i < iovcnt; i++) {
        uint32_t kiov[2];
        int cr = copy_from_user(kiov, &iov[i * 2], sizeof(kiov));
        if (cr < 0) return cr;
        const char *base = (const char *)(uintptr_t)kiov[0];
        int         len  = (int)kiov[1];
        if (len <= 0) continue;
        if (!access_ok(base, (size_t)len)) return -14;

        /* Reuse sys_write logic via direct dispatch */
        registers_t fake = *regs;
        fake.ebx = (uint32_t)fd;
        fake.ecx = (uint32_t)(uintptr_t)base;
        fake.edx = (uint32_t)len;
        int n = sys_write(&fake);
        if (n < 0) return n;
        total += n;
    }
    return total;
}

/* ── sys_rt_sigsuspend(mask_ptr, sigsetsize) — EAX=179 ───────────────────── */
/*
 * Atomically replace the blocked signal mask with *mask_ptr, sleep until
 * any unblocked signal arrives, restore the old mask, and return -EINTR.
 * signal_send() already wakes sleeping processes, so sleep_on() suffices.
 */
static int sys_rt_sigsuspend(registers_t *regs) {
    const uint32_t *mask = (const uint32_t *)(uintptr_t)regs->ebx;
    uint32_t old_mask = current_proc->blocked_sigs;

    if (mask) {
        /* SIGKILL (9) and SIGSTOP (19) cannot be blocked */
        uint32_t raw = 0;
        int cr = copy_from_user(&raw, mask, sizeof(raw));
        if (cr < 0) return cr;
        uint32_t m = sigset_from_user(raw) & ~((1u << SIGKILL) | (1u << SIGSTOP));
        current_proc->blocked_sigs = m;
    }

    /* Sleep until a DELIVERABLE signal wakes us (signal_send only wakes for
     * those); if one is already pending under the new mask, do not sleep.
     * Linux sigsuspend returns -ERESTARTNOHAND: EINTR once a handler has run,
     * a transparent restart if the signal turned out to do nothing. */
    if (!signal_interrupt_pending(current_proc))
        sleep_on((void *)&sys_rt_sigsuspend);

    current_proc->blocked_sigs = old_mask;
    return -ERESTARTNOHAND;
}

/* ── sys_clone(flags, child_stack, ...) — EAX=120 ──────────────────────── */
/* Linux i386 clone(flags, stack, ptid, newtls, ctid) — args in EBX,ECX,EDX,ESI,EDI. */

/*
 * Thread-group model (kernel/fork.c copy_process):
 *   - no CLONE_VM            : fork.  COW copy of the address space, own group.
 *   - CLONE_VM, no THREAD    : own thread group (tgid = pid, own signal state,
 *                              parent = creating process) that RUNS IN THE
 *                              CREATOR'S ADDRESS SPACE: glibc posix_spawn
 *                              (CLONE_VM|CLONE_VFORK), vfork, Breakpad.  An
 *                              exit_group() there ends only the child.
 *   - CLONE_THREAD           : a thread of the creator's group (tgid inherited,
 *                              parent inherited from the group).  Requires
 *                              CLONE_SIGHAND, which requires CLONE_VM.
 * The handler table is shared under CLONE_SIGHAND and copied otherwise; the fd
 * table is shared under CLONE_FILES and copied otherwise.
 */
static int sys_clone(registers_t *regs) {
    uint32_t flags       = regs->ebx;
    uint32_t child_stack = regs->ecx;
    uint32_t uptid       = regs->edx;   /* CLONE_PARENT_SETTID target */
    uint32_t newtls      = regs->esi;   /* CLONE_SETTLS: struct user_desc* */
    uint32_t uctid       = regs->edi;   /* CLONE_CHILD_(SET|CLEAR)TID target */

    if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND)) return -22;
    if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) return -22;

    if (!(flags & CLONE_VM)) {
        /* No CLONE_VM → COW copy of the address space (like fork).  But if the
         * caller supplied a child stack (e.g. Breakpad's crash dumper clones a
         * frozen snapshot onto a fresh stack and runs an entry fn), the child
         * must run on THAT stack, not the parent's — else its clone trampoline
         * pops garbage and jumps into the weeds. */
        return do_fork(regs, child_stack, flags, uptid, uctid);
    }

    struct proc *parent = current_proc;
    struct proc *child;

    if (!parent) return -1;
    /* Validate the child SP as an ADDRESS RANGE, not by page presence:
     * child_stack is the EXCLUSIVE top of the stack, so the byte at child_stack
     * is legitimately unmapped (guard / demand-paged) — requiring it present
     * (the old access_ok) wrongly rejected posix_spawn's vfork stack, failing
     * every child-process launch with EINVAL.  Stack pages fault in on demand. */
    if (!child_stack || child_stack > 0xC0000000U)
        return -22;                     /* threads need a plausible stack */

    child = allocproc();
    if (!child) return -11;

    __builtin_memcpy(child->tf, parent->tf, sizeof(registers_t));
    child->tf->eax = 0;                 /* clone returns 0 in the child */
    child->tf->useresp = child_stack;

    __builtin_memcpy(child->name, parent->name, sizeof(parent->name));
    child->pending_sigs  = 0;
    child->blocked_sigs  = parent->blocked_sigs;
    child->sigframe_addr = 0;
    if (flags & CLONE_SIGHAND) {
        /* One handler table for the group (Linux copy_sighand: refcount++). */
        sighand_put(child->sighand);
        child->sighand = parent->sighand;
        child->sighand->refcount++;
    } else if (parent->sighand) {
        __builtin_memcpy(child->sighand->handlers, parent->sighand->handlers,
                         sizeof(child->sighand->handlers));
        __builtin_memcpy(child->sighand->flags, parent->sighand->flags,
                         sizeof(child->sighand->flags));
    }
    __builtin_memcpy(child->cwd, parent->cwd, sizeof(parent->cwd));
    child->heap_end  = parent->heap_end;
    child->umask     = parent->umask;
    child->uid = parent->uid; child->gid = parent->gid;
    child->euid = parent->euid; child->egid = parent->egid;
    child->mmap_next = parent->mmap_next;
    child->pgrp      = parent->pgrp;
    child->sid       = parent->sid;
    child->ctty      = parent->ctty;
    if (child->ctty)
        vfs_retain(child->ctty);

    child->pgdir_phys = parent->pgdir_phys;
    pgdir_retain(child->pgdir_phys);

    if (flags & CLONE_THREAD) {
        /* Same group as the creator; the group's parent is the parent of every
         * thread (copy_process: p->real_parent = current->real_parent), so a
         * thread's exit is never a "child exit" for the creating thread. */
        child->tgid   = parent->tgid;
        child->parent = parent->parent;
    } else {
        /* Own group in a shared address space.  Its parent is the creating
         * PROCESS; its VMA list and mmap cursor stay with the address-space
         * owner (the creator's group leader), which mmap_owner() follows. */
        child->tgid     = child->pid;
        child->parent   = proc_group_leader(parent);
        child->vm_owner = mmap_owner();
    }

    /* CLONE_SETTLS: each thread gets its OWN TLS base (the thread pointer that
     * %gs:0 resolves to).  Read it from the user_desc the caller passed; the
     * scheduler reloads GDT entry 6 from tls_base on every context switch. */
    child->tls_base = parent->tls_base;
    if ((flags & CLONE_SETTLS) && newtls) {
        struct { int entry_number; uint32_t base_addr; } ud;
        if (copy_from_user(&ud, (void *)(uintptr_t)newtls, sizeof(ud)) == 0)
            child->tls_base = ud.base_addr;
    }

    /* tid notifications — we share the address space, so the current CR3 maps
     * both the parent's and child's view; write the user words directly. */
    child->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? uctid : 0;
    if ((flags & CLONE_PARENT_SETTID) && uptid &&
        access_ok((void *)(uintptr_t)uptid, 4))
        *(uint32_t *)(uintptr_t)uptid = (uint32_t)child->pid;
    if ((flags & CLONE_CHILD_SETTID) && uctid &&
        access_ok((void *)(uintptr_t)uctid, 4))
        *(uint32_t *)(uintptr_t)uctid = (uint32_t)child->pid;

    if (flags & CLONE_FILES) {
        /* Share the parent's fd table (Linux CLONE_FILES) — an fd opened by any
         * thread is instantly visible to all.  Drop the fresh table allocproc
         * gave the child and point at the shared one. */
        fdtable_put(child);
        child->fdt = parent->fdt;
        child->fdt->refcount++;
        child->ofile = child->fdt->f;
    } else {
        /* Private copy (CLONE_VM without CLONE_FILES: posix_spawn, vfork). */
        for (int i = 0; i < MAX_FD; i++) {
            child->ofile[i] = parent->ofile[i];
            fd_retain(&parent->ofile[i]);
        }
    }
    shm_proc_fork(parent, child);

    /* CLONE_VFORK: the child shares our address space; block until it execs
     * (which gives it a fresh pgdir, un-sharing) or exits.  Without this the
     * child runs concurrently in the shared address space and races us.  The
     * wait is killable only (kernel/fork.c wait_for_vfork_done: TASK_KILLABLE),
     * other signals are delivered once the child has gone its own way. */
    if (flags & CLONE_VFORK) {
        child->vfork_parent = parent;
        parent->vfork_waiting = 1;
        child->state = PROC_RUNNABLE;
        while (parent->vfork_waiting) {
            if (parent->pending_sigs & (1u << SIGKILL)) break;
            sleep_on((void *)&parent->vfork_waiting);
        }
        return child->pid;
    }

    child->state = PROC_RUNNABLE;
    return child->pid;
}

/* Wake a vfork parent (called from exec/exit when the child stops sharing). */
static void vfork_wake_parent(void) {
    if (current_proc && current_proc->vfork_parent) {
        struct proc *vp = current_proc->vfork_parent;
        current_proc->vfork_parent = NULL;
        vp->vfork_waiting = 0;
        wake_up((void *)&vp->vfork_waiting);
    }
}

/* ── sys_clone3(struct clone_args *, size) — EAX=435 ────────────────────────
 * glibc 2.34+ tries clone3 first and only falls back to legacy clone(120) on
 * ENOSYS.  That fallback path on i386 sets up the child's thread function on
 * the stack incorrectly here, so the child calls a NULL fn pointer and dies.
 * Implementing clone3 natively keeps glibc on its primary path.  We translate
 * the clone_args struct into the same thread-creation logic as sys_clone. */
static int sys_clone3(registers_t *regs) {
    uint32_t uargs = regs->ebx;
    uint32_t size  = regs->ecx;
    /* struct clone_args (Linux): all fields u64, little-endian. */
    struct clone_args {
        uint64_t flags, pidfd, child_tid, parent_tid, exit_signal,
                 stack, stack_size, tls, set_tid, set_tid_size, cgroup;
    } ca;
    if (size < 64) return -22;                 /* need at least through tls */
    if (!access_ok((void *)(uintptr_t)uargs, size < sizeof(ca) ? size : sizeof(ca)))
        return -14;
    __builtin_memset(&ca, 0, sizeof(ca));
    uint32_t copy = size < sizeof(ca) ? size : (uint32_t)sizeof(ca);
    if (copy_from_user(&ca, (void *)(uintptr_t)uargs, copy) < 0) return -14;

    /* For clone3 the child SP is stack + stack_size (kernel sets it); glibc's
     * __clone3 child path then pops its fn/arg from that stack top. */
    uint32_t child_stack = (uint32_t)ca.stack + (uint32_t)ca.stack_size;

    registers_t fake = *regs;
    fake.ebx = (uint32_t)ca.flags;             /* CLONE_* bits (exit_signal is separate) */
    fake.ecx = child_stack;
    fake.edx = (uint32_t)ca.parent_tid;        /* CLONE_PARENT_SETTID target */
    fake.esi = (uint32_t)ca.tls;               /* CLONE_SETTLS user_desc* (i386) */
    fake.edi = (uint32_t)ca.child_tid;         /* CLONE_CHILD_*TID target */
    return sys_clone(&fake);
}

/* ── sys_socketcall(call, args) — EAX=102 ────────────────────────────────── */
#define AF_UNIX_K  1    /* local IPC sockets (X11 / Wayland / D-Bus) */

static int socket_arg_count(int call) {
    switch (call) {
    case 1:  return 3; /* socket(domain, type, protocol) */
    case 2:  return 3; /* bind */
    case 3:  return 3; /* connect */
    case 4:  return 2; /* listen */
    case 5:  return 3; /* accept */
    case 6:  return 3; /* getsockname */
    case 7:  return 3; /* getpeername */
    case 8:  return 4; /* socketpair */
    case 9:  return 4; /* send */
    case 10: return 4; /* recv */
    case 11: return 6; /* sendto */
    case 12: return 6; /* recvfrom */
    case 13: return 2; /* shutdown */
    case 14: return 5; /* setsockopt */
    case 15: return 5; /* getsockopt */
    case 16: return 3; /* sendmsg(fd, msghdr*, flags) */
    case 17: return 3; /* recvmsg(fd, msghdr*, flags) */
    case 18: return 4; /* accept4(fd, addr, addrlen, flags) */
    default: return -1;
    }
}

/* Core of the socket API, working on an already-fetched kernel args array.
 * Shared by socketcall(102) and the direct i386 socket syscalls (359-373). */
#define SOCK_CLOEXEC_K   0x80000
#define SOCK_NONBLOCK_K  0x800
static int socketcall_core(int call, uint32_t *kargs) {
    /* accept4(fd, addr, addrlen, flags) is socketcall index 18 (Linux
     * SYS_ACCEPT4).  Its SOCK_CLOEXEC/SOCK_NONBLOCK apply to the NEW
     * descriptor only (net/socket.c __sys_accept4), so carry them aside and
     * run the ordinary accept path. */
    int accept4_flags = 0;
    if (call == 18) { accept4_flags = (int)kargs[3]; call = 5; }

    /* musl ORs SOCK_NONBLOCK (0x800) / SOCK_CLOEXEC (0x80000) into type */
    if (call == 1) { /* socket(domain, type, protocol) */
        int domain   = (int)kargs[0];
        int type     = (int)kargs[1] & 0xFF;
        int nonblock = ((int)kargs[1] & SOCK_NONBLOCK_K) != 0;
        int cloexec  = ((int)kargs[1] & SOCK_CLOEXEC_K) != 0;

        if (domain == AF_UNIX_K) {           /* AF_UNIX local socket */
            usocket_t *us = usocket_create(type);
            if (!us) return -12;
            for (int fd = 0; fd < MAX_FD; fd++) {
                if (current_proc->ofile[fd].type == FD_NONE) {
                    current_proc->ofile[fd].type  = FD_USOCKET;
                    current_proc->ofile[fd].usock = us;
                    current_proc->ofile[fd].flags =
                        O_RDWR | (nonblock ? O_NONBLOCK : 0);
                    current_proc->ofile[fd].cloexec = (uint8_t)cloexec;
                    return fd;
                }
            }
            usocket_release(us);
            return -24;
        }

        net_socket_t *sock = NULL;
        int ret = net_socket_create(domain, type, (int)kargs[2], &sock);
        if (ret < 0)
            return ret;
        for (int fd = 0; fd < MAX_FD; fd++) {
            if (current_proc->ofile[fd].type == FD_NONE) {
                current_proc->ofile[fd].type = FD_SOCKET;
                current_proc->ofile[fd].socket = sock;
                current_proc->ofile[fd].flags =
                    O_RDWR | (nonblock ? O_NONBLOCK : 0);
                current_proc->ofile[fd].cloexec = (uint8_t)cloexec;
                return fd;
            }
        }
        net_socket_release(sock);
        return -24;
    }

    if (call == 8) { /* socketpair(domain, type, protocol, sv[2]) */
        int domain = (int)kargs[0];
        if (domain != AF_UNIX_K) return -95;     /* only AF_UNIX pairs */
        uint32_t *usv = (uint32_t *)(uintptr_t)kargs[3];
        if (!access_ok(usv, 2 * sizeof(uint32_t))) return -14;
        usocket_t *a = NULL, *b = NULL;
        if (usocket_socketpair(&a, &b) < 0) return -12;
        int fda = -1, fdb = -1;
        for (int fd = 0; fd < MAX_FD; fd++)
            if (current_proc->ofile[fd].type == FD_NONE) {
                if (fda < 0) fda = fd;
                else { fdb = fd; break; }
            }
        if (fda < 0 || fdb < 0) {
            usocket_release(a); usocket_release(b);
            return -24;
        }
        int nb = ((int)kargs[1] & SOCK_NONBLOCK_K) != 0;
        int ce = ((int)kargs[1] & SOCK_CLOEXEC_K) != 0;
        current_proc->ofile[fda].type = FD_USOCKET;
        current_proc->ofile[fda].usock = a;
        current_proc->ofile[fda].flags = O_RDWR | (nb ? O_NONBLOCK : 0);
        current_proc->ofile[fda].cloexec = (uint8_t)ce;
        current_proc->ofile[fdb].type = FD_USOCKET;
        current_proc->ofile[fdb].usock = b;
        current_proc->ofile[fdb].flags = O_RDWR | (nb ? O_NONBLOCK : 0);
        current_proc->ofile[fdb].cloexec = (uint8_t)ce;
        uint32_t sv[2] = { (uint32_t)fda, (uint32_t)fdb };
        if (copy_to_user(usv, sv, sizeof(sv)) < 0) return -14;
        return 0;
    }

    int fd = (int)kargs[0];
    if (fd < 0 || fd >= MAX_FD)
        return -9;
    proc_file_t *f = &current_proc->ofile[fd];

    /* ── AF_UNIX local sockets ─────────────────────────────────────────── */
    if (f->type == FD_USOCKET && f->usock) {
        usocket_t *us = f->usock;
        int nb = (f->flags & O_NONBLOCK) != 0;

        if (call == 2 || call == 3) {        /* bind / connect (sockaddr_un) */
            /* struct sockaddr_un { u16 sun_family; char sun_path[108]; } */
            uint32_t ua = kargs[1], alen = kargs[2];
            char path[110];
            if (alen < 3 || alen > 2 + 108) return -22;
            if (!access_ok((void *)(uintptr_t)ua, alen)) return -14;
            uint32_t plen = alen - 2;
            if (plen > 108) plen = 108;
            if (copy_from_user(path, (void *)(uintptr_t)(ua + 2), plen) < 0)
                return -14;
            /* Abstract namespace (Linux): sun_path[0]==0, name follows.  libxcb
             * tries "@/tmp/.X11-unix/X0" before the filesystem path.  Key it as
             * '@'+name so it can't collide with a real path (which starts '/'). */
            if (plen >= 1 && path[0] == '\0') {
                path[0] = '@';
                path[plen] = '\0';
            } else {
                path[plen] = '\0';           /* filesystem namespace */
            }
            return (call == 2) ? usocket_bind(us, path)
                               : usocket_connect(us, path);
        }
        if (call == 4)                       /* listen(fd, backlog) */
            return usocket_listen(us, (int)kargs[1]);
        if (call == 5) {                     /* accept(fd, addr, addrlen) */
            int err = 0;
            usocket_t *ns = usocket_accept(us, nb, &err);
            if (!ns) return err;
            for (int nfd = 0; nfd < MAX_FD; nfd++)
                if (current_proc->ofile[nfd].type == FD_NONE) {
                    current_proc->ofile[nfd].type  = FD_USOCKET;
                    current_proc->ofile[nfd].usock = ns;
                    current_proc->ofile[nfd].flags = O_RDWR |
                        ((accept4_flags & SOCK_NONBLOCK_K) ? O_NONBLOCK : 0);
                    current_proc->ofile[nfd].cloexec =
                        (accept4_flags & SOCK_CLOEXEC_K) ? 1 : 0;
                    return nfd;
                }
            usocket_release(ns);
            return -24;
        }
        if (call == 6 || call == 7) {        /* getsockname / getpeername */
            return 0;                        /* enough for X's checks */
        }
        if (call == 9 || call == 11) {       /* send / sendto */
            const void *buf = (const void *)(uintptr_t)kargs[1];
            uint32_t len = kargs[2];
            if (!access_ok(buf, len)) return -14;
            return usocket_write(us, buf, (int)len, nb);
        }
        if (call == 10 || call == 12) {      /* recv / recvfrom */
            void *buf = (void *)(uintptr_t)kargs[1];
            uint32_t len = kargs[2];
            if (!access_ok(buf, len)) return -14;
            return usocket_read(us, buf, (int)len, nb);
        }
        if (call == 16 || call == 17) {      /* sendmsg / recvmsg */
            /* struct msghdr { name,namelen,iov,iovlen,control,controllen,flags }.
             * Firefox's multiprocess IPC passes its channel + shared-memory file
             * descriptors as SCM_RIGHTS ancillary data here; without honouring it
             * the child processes never get their fds and Firefox exits(1). */
            uint32_t umsg = kargs[1];
            uint32_t mh[7];
            if (copy_from_user(mh, (void *)(uintptr_t)umsg, sizeof(mh)) < 0)
                return -14;
            uint32_t iov = mh[2], iovlen = mh[3];
            uint32_t uctrl = mh[4], uctrllen = mh[5];
            if (iovlen > 1024) return -22;

            /* sendmsg: parse SCM_RIGHTS cmsg(s), retain the named fds. */
            proc_file_t pass[SCM_MAX_FDS];
            int npass = 0;
            if (call == 16 && uctrl && uctrllen >= 12 && uctrllen <= 256) {
                uint8_t cbuf[256];
                if (copy_from_user(cbuf, (void *)(uintptr_t)uctrl, uctrllen) >= 0) {
                    uint32_t off = 0;
                    while (off + 12 <= uctrllen) {
                        uint32_t clen; int level, ctype;
                        __builtin_memcpy(&clen,  cbuf + off,     4);
                        __builtin_memcpy(&level, cbuf + off + 4, 4);
                        __builtin_memcpy(&ctype, cbuf + off + 8, 4);
                        if (clen < 12 || off + clen > uctrllen) break;
                        if (level == 1 /*SOL_SOCKET*/ && ctype == 1 /*SCM_RIGHTS*/) {
                            int cnt = (int)((clen - 12) / 4);
                            for (int k = 0; k < cnt && npass < SCM_MAX_FDS; k++) {
                                int sfd;
                                __builtin_memcpy(&sfd, cbuf + off + 12 + k * 4, 4);
                                if (sfd < 0 || sfd >= MAX_FD) continue;
                                proc_file_t *src = &current_proc->ofile[sfd];
                                if (src->type == FD_NONE) continue;
                                pass[npass] = *src;
                                fd_retain(&pass[npass]);
                                npass++;
                            }
                        }
                        off += (clen + 3u) & ~3u;
                    }
                }
            }

            int total = 0, eagain = 0;
            for (uint32_t i = 0; i < iovlen; i++) {
                uint32_t iv[2];
                if (copy_from_user(iv, (void *)(uintptr_t)(iov + i * 8),
                                   sizeof(iv)) < 0) {
                    if (total) break;
                    for (int k = 0; k < npass; k++) fd_release(&pass[k]);
                    return -14;
                }
                int len = (int)iv[1];
                if (len <= 0) continue;
                if (!access_ok((void *)(uintptr_t)iv[0], (size_t)len)) {
                    if (total) break;
                    for (int k = 0; k < npass; k++) fd_release(&pass[k]);
                    return -14;
                }
                int n = (call == 16)
                    ? usocket_write(us, (void *)(uintptr_t)iv[0], len, nb)
                    : usocket_read(us, (void *)(uintptr_t)iv[0], len, nb);
                if (n < 0) {
                    if (total) break;
                    if (call == 16) {
                        for (int k = 0; k < npass; k++) fd_release(&pass[k]);
                        return n;
                    }
                    /* recvmsg with no data: maybe fds are still deliverable */
                    if (n == -11) { eagain = 1; break; }
                    return n;
                }
                total += n;
                if (n < len) break;          /* short read/write — stop */
            }

            /* sendmsg: enqueue the retained fds, tagged after the data. */
            if (call == 16 && npass > 0) {
                if (usocket_send_fds(us, pass, npass) < 0)
                    for (int k = 0; k < npass; k++) fd_release(&pass[k]);
            }

            /* recvmsg: deliver any ready fds + build the control buffer. */
            if (call == 17) {
                uint32_t ctrl_used = 0;
                proc_file_t got[SCM_MAX_FDS];
                int ngot = (uctrl && uctrllen >= 12)
                         ? usocket_recv_fds(us, got, SCM_MAX_FDS) : 0;
                if (ngot > 0) {
                    int cloex = (kargs[2] & 0x40000000) ? 1 : 0; /* MSG_CMSG_CLOEXEC */
                    int newfds[SCM_MAX_FDS]; int ninst = 0;
                    for (int k = 0; k < ngot; k++) {
                        int slot = -1;
                        for (int j = 0; j < MAX_FD; j++)
                            if (current_proc->ofile[j].type == FD_NONE) { slot = j; break; }
                        if (slot < 0) { printk("[scm] fd table FULL on recv (pid %d)\n",
                                               current_proc ? current_proc->pid : -1);
                                        fd_release(&got[k]); continue; }
                        current_proc->ofile[slot] = got[k];
                        current_proc->ofile[slot].cloexec = (uint8_t)cloex;
                        newfds[ninst++] = slot;
                    }
                    uint32_t clen = 12 + (uint32_t)ninst * 4;
                    if (ninst > 0 && uctrllen >= clen) {
                        uint8_t cbuf[256];
                        uint32_t lvl = 1, typ = 1;       /* SOL_SOCKET, SCM_RIGHTS */
                        __builtin_memcpy(cbuf + 0, &clen, 4);
                        __builtin_memcpy(cbuf + 4, &lvl, 4);
                        __builtin_memcpy(cbuf + 8, &typ, 4);
                        for (int k = 0; k < ninst; k++)
                            __builtin_memcpy(cbuf + 12 + k * 4, &newfds[k], 4);
                        copy_to_user((void *)(uintptr_t)uctrl, cbuf, clen);
                        ctrl_used = clen;
                    }
                }
                copy_to_user((void *)(uintptr_t)(umsg + 20), &ctrl_used, 4);
                uint32_t zero = 0;
                copy_to_user((void *)(uintptr_t)(umsg + 24), &zero, 4);
                if (total == 0 && ngot == 0 && eagain) return -11;
            }
            return total;
        }
        if (call == 13) return 0;            /* shutdown */
        if (call == 14) return 0;            /* setsockopt: ignore */
        if (call == 15) {                    /* getsockopt */
            int       level   = (int)kargs[1];
            int       optname = (int)kargs[2];
            void     *optval = (void *)(uintptr_t)kargs[3];
            uint32_t *optlen = (uint32_t *)(uintptr_t)kargs[4];
            uint32_t  l = 0;
            if (optlen && copy_from_user(&l, optlen, sizeof(l)) < 0) return -14;
            if (optval && l >= 4) {
                /* SOL_SOCKET(1): SO_SNDBUF(7)/SO_RCVBUF(8) must report a POSITIVE
                 * buffer size — Firefox's IPC Channel::SetPipe
                 * (ipc_channel_posix.cc:181) does CHECK(buf_len > 0) on
                 * getsockopt(SO_SNDBUF) and ABORTS the IPC I/O thread if it's 0.
                 * Report our per-direction unix-socket ring size (64 KiB).
                 * Everything else (SO_ERROR etc.) reports 0 = "no error". */
                uint32_t v = 0;
                if (level == 1 && (optname == 7 || optname == 8)) v = 65536;
                if (copy_to_user(optval, &v, 4) < 0) return -14;
                l = 4;
                if (optlen && copy_to_user(optlen, &l, sizeof(l)) < 0) return -14;
            }
            return 0;
        }
        return -22;
    }

    if (f->type != FD_SOCKET || !f->socket)
        return -88;

    if (call == 2 || call == 3) { /* bind/connect */
        net_sockaddr_in_t *uaddr = (net_sockaddr_in_t *)(uintptr_t)kargs[1];
        uint32_t addrlen = kargs[2];
        if (addrlen < sizeof(net_sockaddr_in_t) ||
            !access_ok(uaddr, sizeof(net_sockaddr_in_t)))
            return -14;
        net_sockaddr_in_t kaddr;
        if (copy_from_user(&kaddr, uaddr, sizeof(kaddr)) < 0)
            return -14;
        return (call == 2) ? net_socket_bind(f->socket, &kaddr)
                           : net_socket_connect(f->socket, &kaddr);
    }

    if (call == 9) { /* send(fd, buf, len, flags) */
        const void *buf = (const void *)(uintptr_t)kargs[1];
        uint32_t len = kargs[2];
        if (!access_ok(buf, len))
            return -14;
        return net_socket_sendto(f->socket, buf, len, NULL);
    }

    if (call == 10) { /* recv(fd, buf, len, flags) */
        void *buf = (void *)(uintptr_t)kargs[1];
        uint32_t len = kargs[2];
        if (!access_ok(buf, len))
            return -14;
        return net_socket_recvfrom(f->socket, buf, len, NULL);
    }

    if (call == 11) { /* sendto */
        const void *buf = (const void *)(uintptr_t)kargs[1];
        uint32_t len = kargs[2];
        net_sockaddr_in_t *uaddr = (net_sockaddr_in_t *)(uintptr_t)kargs[4];
        uint32_t addrlen = kargs[5];
        if (!access_ok(buf, len))
            return -14;
        if (!uaddr)
            return net_socket_sendto(f->socket, buf, len, NULL);
        if (addrlen < sizeof(net_sockaddr_in_t) ||
            !access_ok(uaddr, sizeof(net_sockaddr_in_t)))
            return -14;
        net_sockaddr_in_t kaddr;
        if (copy_from_user(&kaddr, uaddr, sizeof(kaddr)) < 0)
            return -14;
        return net_socket_sendto(f->socket, buf, len, &kaddr);
    }

    if (call == 12) { /* recvfrom */
        void *buf = (void *)(uintptr_t)kargs[1];
        uint32_t len = kargs[2];
        net_sockaddr_in_t *uaddr = (net_sockaddr_in_t *)(uintptr_t)kargs[4];
        uint32_t *ulen = (uint32_t *)(uintptr_t)kargs[5];
        net_sockaddr_in_t kaddr;
        if (!access_ok(buf, len))
            return -14;
        if (uaddr) {
            uint32_t klen = 0;
            if (!ulen || copy_from_user(&klen, ulen, sizeof(klen)) < 0 ||
                klen < sizeof(net_sockaddr_in_t) ||
                !access_ok(uaddr, sizeof(net_sockaddr_in_t)))
                return -14;
        }
        int ret = net_socket_recvfrom(f->socket, buf, len,
                                      uaddr ? &kaddr : NULL);
        if (ret >= 0 && uaddr) {
            uint32_t klen = sizeof(net_sockaddr_in_t);
            int cr = copy_to_user(uaddr, &kaddr, sizeof(kaddr));
            if (cr < 0) return cr;
            cr = copy_to_user(ulen, &klen, sizeof(klen));
            if (cr < 0) return cr;
        }
        return ret;
    }

    if (call == 13)
        return net_socket_shutdown(f->socket, (int)kargs[1]);

    if (call == 14)                 /* setsockopt: accept and ignore */
        return 0;

    if (call == 15) {               /* getsockopt */
        int       level   = (int)kargs[1];
        int       optname = (int)kargs[2];
        void     *optval = (void *)(uintptr_t)kargs[3];
        uint32_t *optlen = (uint32_t *)(uintptr_t)kargs[4];
        uint32_t  len = 0;
        if (optlen) {
            if (copy_from_user(&len, optlen, sizeof(len)) < 0) return -14;
        }
        if (optval && len >= 4) {
            uint32_t v = 0;         /* SO_ERROR=0: connection succeeded */
            if (level == 1 && (optname == 7 || optname == 8)) v = 65536; /* SO_SNDBUF/RCVBUF */
            if (copy_to_user(optval, &v, 4) < 0) return -14;
            len = 4;
            if (optlen && copy_to_user(optlen, &len, sizeof(len)) < 0)
                return -14;
        }
        return 0;
    }

    if (call == 4 || call == 5 || call == 6 || call == 7 || call == 8)
        return -95;

    return -22;
}

/* socketcall(102): fetch the args array from user space, then run the core. */
static int sys_socketcall(registers_t *regs) {
    int call = (int)regs->ebx;
    uint32_t *args = (uint32_t *)(uintptr_t)regs->ecx;
    int argc = socket_arg_count(call);
    if (argc < 0) return -22;
    uint32_t kargs[6] = {0};
    if (copy_from_user(kargs, args, (size_t)argc * sizeof(uint32_t)) < 0)
        return -14;
    return socketcall_core(call, kargs);
}

/*
 * Direct i386 socket syscalls (359-373).  Modern musl prefers these over
 * socketcall(102); without them GTK/GLib spin on recvmsg(372).  Args arrive in
 * registers (ebx,ecx,edx,esi,edi,ebp) — marshal them and reuse the core.
 * Maps the direct numbers onto the socketcall "call" indices.
 */
static int sys_socket_direct(registers_t *regs, int which) {
    uint32_t kargs[6] = {
        regs->ebx, regs->ecx, regs->edx, regs->esi, regs->edi, regs->ebp
    };
    switch (which) {
    case 359: return socketcall_core(1,  kargs);   /* socket */
    case 360: return socketcall_core(8,  kargs);   /* socketpair */
    case 361: return socketcall_core(2,  kargs);   /* bind */
    case 362: return socketcall_core(3,  kargs);   /* connect */
    case 363: return socketcall_core(4,  kargs);   /* listen */
    case 364: return socketcall_core(18, kargs);   /* accept4 (flags honoured) */
    case 365: return socketcall_core(15, kargs);   /* getsockopt */
    case 366: return socketcall_core(14, kargs);   /* setsockopt */
    case 367: return socketcall_core(6,  kargs);   /* getsockname */
    case 368: return socketcall_core(7,  kargs);   /* getpeername */
    case 369: return socketcall_core(11, kargs);   /* sendto */
    case 370: return socketcall_core(16, kargs);   /* sendmsg */
    case 371: return socketcall_core(12, kargs);   /* recvfrom */
    case 372: return socketcall_core(17, kargs);   /* recvmsg */
    case 373: return socketcall_core(13, kargs);   /* shutdown */
    default:  return -38;
    }
}

/* ── Dispatch table ───────────────────────────────────────────────────────── */

void syscall_dispatch(registers_t *regs) {
    uint32_t num = regs->eax;
    int ret = -38;   /* -ENOSYS */

    if (current_proc) current_proc->last_syscall = (int)num;
    {   /* diagnostic: periodic live-process snapshot to spot stalls */
        static uint32_t last_snap = 0;
        uint32_t now = pit_ticks();
        if (now - last_snap >= 300) { last_snap = now; proc_debug_snapshot(); }
    }
    /* Bochs watchpoint hunt: when gtkprobe has the corrupting arena slot
     * (0x40073000) mapped, print its physical address and hit a Bochs MAGIC
     * BREAKPOINT (xchg %bx,%bx) ONCE — the debugger then sets `watch w <phys>`
     * to catch the instruction that writes 0x80000000.  A no-op under QEMU. */
    {
        static int gtk_broke = 0;
        if (!gtk_broke && current_proc &&
            __builtin_strcmp(current_proc->name, "gtkprobe") == 0 &&
            (*paging_get_pde(0x40073000U) & 0x1) &&
            (*paging_get_pte(0x40073000U) & 0x1)) {
            uint32_t ph = (*paging_get_pte(0x40073000U) & ~0xFFFU) |
                          (0x40073000U & 0xFFFU);
            printk("[bochs-hunt] gtkprobe 0x40073000 -> phys 0x%08x (MAGIC BREAK)\n",
                   (unsigned)ph);
            gtk_broke = 1;
            __asm__ volatile("xchgw %bx, %bx");   /* Bochs magic breakpoint */
        }
    }

    switch (num) {
    case 1:   sys_exit(regs);                  break;  /* noreturn */
    case 2:   ret = sys_fork(regs);            break;
    case 3:   ret = sys_read(regs);            break;
    case 4:   ret = sys_write(regs);           break;
    case 5:   ret = sys_open(regs);            break;
    case 6:   ret = sys_close(regs);           break;
    case 7:   ret = sys_waitpid(regs);         break;
    case 10:  ret = sys_unlink(regs);          break;
    case 14:  ret = sys_mknod(regs);           break;
    case 11:  ret = sys_exec(regs);            break;
    case 12:  ret = sys_chdir(regs);           break;
    case 19:  ret = sys_lseek(regs);           break;
    case 20:  ret = sys_getpid(regs);          break;
    case 24:  ret = sys_getuid(regs);          break;
    case 33:  ret = sys_access(regs);          break;
    case 37:  ret = sys_kill(regs);            break;
    case 38:  ret = sys_rename(regs);          break;
    case 39:  ret = sys_mkdir(regs);           break;
    case 41:  ret = sys_dup(regs);             break;
    case 42:  ret = sys_pipe(regs);            break;
    case 43:  ret = sys_times(regs);           break;
    case 45:  ret = sys_brk(regs);             break;
    case 47:  ret = sys_getgid(regs);          break;
    case 48:  ret = sys_signal(regs);          break;
    case 49:  ret = sys_geteuid(regs);         break;  /* geteuid */
    case 50:  ret = sys_getegid(regs);         break;  /* getegid */
    case 23:  ret = sys_setuid(regs);          break;  /* setuid16 */
    case 46:  ret = sys_setgid(regs);          break;  /* setgid16 */
    case 213: ret = sys_setuid(regs);          break;  /* setuid32 (musl) */
    case 214: ret = sys_setgid(regs);          break;  /* setgid32 (musl) */
    case 138: ret = sys_seteuid(regs);         break;  /* seteuid helper */
    case 54:  ret = sys_ioctl(regs);           break;
    case 55:  ret = sys_fcntl(regs);           break;
    case 57:  ret = sys_setpgid(regs);         break;
    case 60:  ret = sys_umask(regs);           break;
    case 63:  ret = sys_dup2(regs);            break;
    case 64:  ret = sys_getppid(regs);         break;
    case 65:  ret = sys_getpgrp(regs);         break;
    case 66:  ret = sys_setsid(regs);          break;
    case 75:  ret = sys_setrlimit(regs);       break;
    case 76:  ret = sys_getrlimit(regs);       break;
    case 78:  ret = sys_gettimeofday(regs);    break;
    case 82:  ret = sys_select(regs);          break;
    case 142: ret = sys_select(regs);          break;  /* _newselect */
    case 308: ret = sys_pselect6(regs);        break;  /* pselect6 */
    case 83:  ret = sys_symlink(regs);         break;
    case 85:  ret = sys_readlink(regs);        break;
    case 88:  ret = sys_reboot(regs);          break;
    case 107: ret = sys_lstat(regs);           break;
    case 91:  ret = sys_munmap(regs);          break;
    case 92:  ret = sys_truncate(regs);        break;
    case 93:  ret = sys_ftruncate(regs);       break;
    case 324: ret = sys_fallocate(regs);       break;  /* fallocate */
    case 106: ret = sys_stat(regs);            break;
    case 108: ret = sys_fstat(regs);           break;
    case 114: ret = sys_wait4(regs);           break;
    case 119: sys_sigreturn(regs);             return; /* regs fully restored —
        the dispatcher must NOT stomp the restored eax with its ret value */
    case 120: ret = sys_clone(regs);           break;
    case 435: ret = sys_clone3(regs);          break;  /* clone3 */
    case 122: ret = sys_uname(regs);           break;
    case 125: ret = sys_mprotect(regs);        break;
    case 132: ret = sys_getpgid(regs);         break;
    case 141: ret = sys_getdents(regs);        break;
    case 146: ret = sys_writev(regs);          break;
    case 159: ret = sys_sched_yield(regs);     break;
    case 162: ret = sys_nanosleep(regs);       break;
    case 168: ret = sys_poll(regs);            break;
    case 309: ret = sys_ppoll(regs, 0);        break;  /* ppoll */
    case 414: ret = sys_ppoll(regs, 1);        break;  /* ppoll_time64 */
    case 172: ret = sys_prctl(regs);           break;
    case 174: ret = sys_rt_sigaction(regs);    break;
    case 175: ret = sys_rt_sigprocmask(regs);  break;
    case 176: ret = 0;                         break;  /* rt_sigpending stub */
    /* fsync(118)/fdatasync(148): our filesystems are RAM/simple-backed and every
     * write is already durable to our backing store, so a sync is a correct
     * no-op.  MUST return 0 (success), not -ENOSYS — glibc's fsync propagates
     * ENOSYS to the app, and code that treats a failed fsync as a failed write
     * (Firefox's startupCache / sqlite / prefs) can then error out. */
    case 118: ret = 0;                         break;  /* fsync */
    case 148: ret = 0;                         break;  /* fdatasync */
    /* posix_fadvise (250=fadvise64, 272=fadvise64_64): pure advisory hints; safe
     * and correct to accept as a no-op success. */
    case 250: ret = 0;                         break;  /* fadvise64 */
    case 272: ret = 0;                         break;  /* fadvise64_64 */
    case 179: ret = sys_rt_sigsuspend(regs);   break;
    case 180: ret = sys_pread64(regs);         break;
    case 181: ret = sys_pwrite64(regs);        break;
    case 183: ret = sys_getcwd(regs);          break;
    case 186: ret = sys_sigaltstack(regs);     break;
    case 192: ret = sys_mmap2(regs);           break;
    case 195: ret = sys_stat64(regs);          break;
    case 196: ret = sys_lstat64(regs);         break;
    case 197: ret = sys_fstat64(regs);         break;
    case 220: ret = sys_getdents64(regs);      break;
    case 224: ret = sys_gettid(regs);          break;
    case 243: ret = sys_set_thread_area(regs); break;
    case 252: sys_exit_group(regs);            break;  /* noreturn */
    case 267: ret = sys_clock_nanosleep(regs); break;  /* clock_nanosleep */
    case 209: ret = sys_getresuid32(regs);     break;  /* getresuid32 */
    case 211: ret = sys_getresgid32(regs);     break;  /* getresgid32 */
    case 225: ret = 0;                         break;  /* readahead: no-op */
    case 242: ret = sys_sched_getaffinity(regs); break;
    case 266: ret = sys_clock_getres(regs);    break;  /* clock_getres */
    case 406: ret = sys_clock_getres_time64(regs); break;
    case 268: ret = sys_statfs64(regs);        break;  /* statfs64 */
    case 269: ret = sys_fstatfs64(regs);       break;  /* fstatfs64 */
    case 96:  ret = 20;                        break;  /* getpriority: nice 0 → 20-0 */
    case 97:  ret = 0;                         break;  /* setpriority: accept, no-op */
    case 158: yield(); ret = 0;                break;  /* sched_yield */
    case 351: ret = sys_sched_setattr(regs);   break;  /* sched_setattr */
    case 352: ret = sys_sched_getattr(regs);   break;  /* sched_getattr */
    case 9:   ret = -1;                        break;  /* link: -EPERM → FF falls back */
    case 356: ret = sys_memfd_create(regs);    break;  /* memfd_create */
    case 258: ret = sys_set_tid_address(regs); break;
    case 311:                                          /* set_robust_list(head, len) */
        if (current_proc) current_proc->robust_list_head = regs->ebx;
        ret = 0; break;
    case 312: ret = 0;                         break;  /* get_robust_list */
    case 386: ret = -38;                       break;  /* rseq: ENOSYS (glibc falls back) */
    case 265: ret = sys_clock_gettime(regs);   break;
    case 295: ret = sys_openat(regs);          break;
    case 330: ret = sys_dup3(regs);            break;
    case 355: ret = sys_getrandom(regs);       break;
    case 383: ret = sys_statx(regs);           break;  /* statx (fontconfig) */
    case 403: ret = sys_clock_gettime64(regs); break;  /* clock_gettime64 */
    case 407: ret = sys_clock_nanosleep_time64(regs); break;
    case 422: ret = sys_futex(regs, 1);        break;  /* futex_time64 (64-bit ts) */
    /* chmod/chown stubs */
    case 15:  ret = sys_chmod(regs);           break;
    case 16:  ret = sys_lchown(regs);          break;
    case 94:  ret = sys_fchmod(regs);          break;
    case 182: ret = sys_chown(regs);           break;
    case 207: ret = sys_fchown(regs);          break;
    case 240: ret = sys_futex(regs, 0);        break;  /* futex (32-bit ts) */
    case 116: ret = sys_sysinfo(regs);         break;
    case 90:  ret = sys_mmap_old(regs);        break;
    case 163: ret = sys_mremap(regs);          break;
    case 191: ret = sys_ugetrlimit(regs);      break;  /* ugetrlimit */
    case 199: ret = sys_getuid(regs);          break;  /* getuid32 */
    case 200: ret = sys_getgid(regs);          break;  /* getgid32 */
    case 201: ret = sys_geteuid(regs);         break;  /* geteuid32 */
    case 202: ret = sys_getegid(regs);         break;  /* getegid32 */
    case 208: ret = sys_setuid(regs);          break;  /* setresuid32≈setuid */
    case 221: ret = sys_fcntl(regs);           break;  /* fcntl64 → fcntl */
    case 340: ret = sys_prlimit64(regs);       break;  /* prlimit64 */
    case 140: ret = sys_llseek(regs);          break;
    case 143: ret = sys_flock(regs);           break;
    case 145: ret = sys_readv(regs);           break;
    case 218: ret = sys_mincore(regs);         break;
    case 144: ret = sys_msync(regs);           break;
    case 150: case 151: case 152: case 153:
              ret = sys_mlock_noop(regs);      break;  /* mlock/munlock/mlockall/munlockall */
    case 13:  ret = sys_time(regs);            break;
    case 219: ret = sys_madvise(regs);         break;
    case 270: ret = sys_tgkill(regs);          break;
    case 238: ret = sys_tkill(regs);           break;
    case 331: ret = sys_pipe2(regs);           break;
    case 102: ret = sys_socketcall(regs);      break;
    /* Direct i386 socket syscalls (modern musl uses these, not socketcall). */
    case 359: case 360: case 361: case 362: case 363: case 364:
    case 365: case 366: case 367: case 368: case 369: case 370:
    case 371: case 372: case 373:
        ret = sys_socket_direct(regs, (int)num); break;
    /* epoll */
    case 254: ret = sys_epoll_create1(regs); break; /* epoll_create(size) — size ignored */
    case 329: ret = sys_epoll_create1(regs); break; /* epoll_create1 (musl/i386) */
    case 255: ret = sys_epoll_ctl(regs);     break; /* epoll_ctl */
    case 256: ret = sys_epoll_wait(regs);    break; /* epoll_wait */
    case 319: ret = sys_epoll_wait(regs);    break; /* epoll_pwait (extra args ignored) */
    /* eventfd */
    case 323: ret = sys_eventfd(regs->ebx, 0);          break;  /* eventfd(initval) */
    case 328: ret = sys_eventfd(regs->ebx, regs->ecx);  break;  /* eventfd2(initval,flags) */
    /* inotify (i386: init=291, add_watch=292, rm_watch=293, init1=332).  We do
     * NOT implement file-change notification.  CRITICAL: these MUST return
     * -ENOSYS so GLib's inotify backend detects "unsupported" and falls back to
     * its polling backend.  Previously 291 was WRONGLY routed to epoll_create1,
     * so GLib got a working-looking fd, polled it, then read() it → EBADF →
     * GLib's FATAL "GLib-GIO-ERROR: inotify read(): Bad file descriptor" aborted
     * the process (killed Firefox content children during startup). */
    case 291: case 292: case 293: case 332: ret = -38; break;  /* -ENOSYS */
    /* AT family syscalls */
    /* MaeroOS shared memory (custom numbers, outside the Linux table) */
    case 500: ret = shm_sys_create(regs->ebx); break;
    case 501: ret = shm_sys_map((int)regs->ebx);   break;
    case 502: ret = shm_sys_unmap((int)regs->ebx); break;
    case 505:  /* register Ctrl+Alt+Backspace kill target (desktop only) */
        keyboard_set_kill_target((int)regs->ebx);
        ret = 0;
        break;

    case 40:  ret = sys_rmdir(regs);           break;  /* rmdir */
    case 193: ret = sys_truncate64(regs);      break;  /* truncate64 */
    case 194: ret = sys_ftruncate64(regs);     break;  /* ftruncate64 */
    case 297: ret = sys_mknodat(regs);         break;  /* mknodat */
    case 296: ret = sys_mkdirat(regs);         break;
    case 300: ret = sys_fstatat64(regs);       break;
    case 301: ret = sys_unlinkat(regs);        break;
    case 302: ret = sys_renameat(regs);        break;
    case 305: ret = sys_readlinkat(regs);      break;
    case 306: ret = sys_fchmodat(regs);        break;
    case 307: ret = sys_faccessat(regs);       break;
    default:
        /* Don't spam the log for common no-op syscalls */
        if (num != 174 && num != 175 && num != 176 &&
            num != 57 && num != 65 && num != 66 && num != 82 &&
            num != 85 && num != 132 && num != 172 && num != 186)
            printk("[SYSCALL] unimplemented %u from pid %d\n",
                   (unsigned)num, current_proc ? current_proc->pid : -1);
        break;
    }

    regs->eax = (uint32_t)(int32_t)ret;

    /* [ftrace] raw firefox MAIN-thread syscall sequence during startup, to find
     * the exact call whose failure triggers ProfileMissingDialog.  Skip the
     * high-frequency noise (futex, mmap/mprotect, r/w, time, sigaction). */
    if (current_proc && current_proc->pid == current_proc->tgid &&
        current_proc->name[0]=='f' && current_proc->name[1]=='i' &&
        current_proc->name[4]=='f') {
        switch (num) {
        case 240: case 403: case 192: case 125: case 3: case 4: case 6:
        case 146: case 145: case 265: case 78: case 174: case 175: case 13:
        case 14: case 90: case 91: case 45: case 197: case 108: case 33:
            break;
        default: {
            static int ft = 0;
            if (ft < 260) { ft++;
                /* Decode the path arg for path-based syscalls so we can see
                 * which file resolution leads to the profile error.  The at-fd
                 * variants (openat 295, statx 383, faccessat 307, fstatat64 300,
                 * readlinkat 305, mkdirat 296) take path at ECX (arg2, after the
                 * dirfd); the plain variants take path at EBX. */
                int at = (num==295||num==383||num==307||num==300||num==305||num==296);
                uint32_t pptr = at ? regs->ecx : regs->ebx;
                char pbuf[96]; pbuf[0]=0;
                int haspath = (num==5||num==295||num==195||num==196||num==106||
                               num==33||num==307||num==383||num==300||num==85||
                               num==305||num==39||num==296||num==83||num==38);
                if (haspath) copy_user_str((const void*)(uintptr_t)pptr, pbuf, sizeof(pbuf));
                /* skip library-loader noise to keep the profile ops visible */
                if (!(haspath && (dbg_str_has(pbuf, ".so") || dbg_str_has(pbuf, "/lib") ||
                                  dbg_str_has(pbuf, "/disk/firefox/") ||
                                  dbg_str_has(pbuf, "/usr/") || dbg_str_has(pbuf, "/etc/"))))
                    printk("[ftrace] sys=%u ret=%d path='%s'\n",
                           (unsigned)num, ret, haspath ? pbuf : "");
            }
        }
        }
    }

    /* [einval-trace] catch any EINVAL returned to a firefox thread (the glxtest
     * "poll failed: Invalid argument" culprit). */
    if (ret == -22 && current_proc && current_proc->name[0]=='f' &&
        current_proc->name[1]=='i' && current_proc->name[4]=='f') {
        static int ev = 0;
        if (ev < 40) { ev++;
            printk("[einval] pid=%d syscall=%u ebx=%x ecx=%x edx=%x esi=%x\n",
                   current_proc->pid, (unsigned)num, (unsigned)regs->ebx,
                   (unsigned)regs->ecx, (unsigned)regs->edx, (unsigned)regs->esi);
        }
    }

    /* Deliver any pending signals before returning to user mode; an
     * interrupted blocking call is restarted or fails with EINTR here (the
     * syscall number lets the restart re-issue it). */
    signal_return_to_user(regs, (int)num);

    /* Linux-style wakeup preemption: if this syscall woke another thread, yield
     * at the return-to-user boundary so the woken thread runs promptly (closes
     * the glibc-2.36 condvar signal-steal window for the IPC Launch thread). */
    resched_on_return();
}
