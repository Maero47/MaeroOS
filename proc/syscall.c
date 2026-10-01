#include "syscall.h"
#include "scheduler.h"
#include "process.h"
#include "signal.h"
#include "ktimer.h"
#include "pipe.h"
#include "usocket.h"
#include "shm.h"
#include "elf.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/kstack.h"
#include "../arch/i686/mm/paging.h"
#include "../arch/i686/mm/tlb.h"
#include "../drivers/serial.h"
#include "../drivers/vga.h"
#include "../drivers/framebuffer.h"
#include "../drivers/acpi.h"
#include "../arch/i686/cpu/smp.h"
#include "../drivers/keyboard.h"
#include "../drivers/nvme.h"
#include "../kernel/random.h"
#include "../kernel/panic.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/cpu/tsc.h"
#include "../arch/i686/cpu/gdt.h"
#include "../arch/i686/cpu/fpu.h"
#include "../arch/i686/cpu/cpuid.h"
#include "../drivers/rtc.h"
#include "../arch/i686/include/io.h"
#include "../kernel/printk.h"
#include "../fs/vfs.h"
#include "../fs/mount.h"
#include "../fs/devfs.h"
#include "../fs/ext2.h"
#include "../net/socket.h"
#include "../net/xsock.h"
#include "flock.h"
#include "../lib/printf.h"
#include <registers.h>
#include <kernel/config.h>
#include <stdint.h>
#include <stddef.h>
#include <kernel/kprof.h>

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
    char     d_name[1];   /* variable; d_type is the record's last byte */
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
        pte_t pte = pte_read((uint32_t)page);
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
 * Bounce buffers for user data handed to code outside proc/.  vfs_read(),
 * vfs_write() and the network stack move data with plain memcpy, and they can
 * sleep before doing it (a pty or tcp read waits for input), so no check made
 * beforehand can hold: a sibling thread may mprotect() or munmap() the buffer
 * in the meantime, and a plain ring-0 store into it faults outside __ex_table
 * — a panic.  So they never see a user pointer: the data goes through a
 * kernel buffer, and only copy_to_user()/copy_from_user() touch user memory.
 */
#define BOUNCE_MAX (64U * 1024U)   /* one call's worth; also covers any UDP datagram */

static uint8_t *bounce_alloc(uint32_t want, uint32_t *size) {
    uint32_t n = want < BOUNCE_MAX ? want : BOUNCE_MAX;
    if (n == 0) n = 1;
    uint8_t *b = (uint8_t *)kmalloc(n);
    if (!b && n > PAGE_SIZE) {                 /* heap tight: settle for a page */
        n = PAGE_SIZE;
        b = (uint8_t *)kmalloc(n);
    }
    *size = n;
    return b;
}

/* Only a regular file can be read in several chunks: its read never blocks
 * and consumes nothing.  A device, pty or socket gets one call, whose data is
 * gone once read, so it is never asked for more than can be delivered. */
static int node_is_regular(vfs_node_t *n) {
    return (n->flags & 0x7U) == VFS_FLAG_FILE;
}

/* vfs_read() at `off` into user `ubuf`.  Returns bytes delivered, or a
 * negative errno if none were.  A fault copying out after a device read
 * loses that data, as on Linux; for a regular file nothing is lost — the
 * caller advances the offset only by what was delivered. */
static int vfs_read_user(vfs_node_t *n, uint32_t off, char *ubuf, uint32_t len) {
    if (len == 0) return 0;
    uint32_t bsz;
    uint8_t *kbuf = bounce_alloc(len, &bsz);
    if (!kbuf) return -12;                                 /* -ENOMEM */
    int regular = node_is_regular(n);
    uint32_t done = 0;
    int err = 0;
    while (done < len) {
        uint32_t want = len - done < bsz ? len - done : bsz;
        int32_t  got  = (int32_t)vfs_read(n, off + done, want, kbuf);
        if (got < 0)  { err = got; break; }
        if (got == 0) break;
        if ((uint32_t)got > want) got = (int32_t)want;
        if (copy_to_user(ubuf + done, kbuf, (uint32_t)got) < 0) { err = -14; break; }
        done += (uint32_t)got;
        if (!regular || (uint32_t)got < want) break;
    }
    kfree(kbuf);
    return done ? (int)done : err;
}

/* vfs_write() of user `ubuf` at `off`, chunk by chunk, stopping at the first
 * short write.  Returns bytes written, or a negative errno if none were. */
static int vfs_write_user(vfs_node_t *n, uint32_t off, const char *ubuf, uint32_t len) {
    if (len == 0) return 0;
    /* File offsets are 32-bit: nothing can be written at or past 4 GiB, and a
     * write straddling it is shortened rather than wrapping to offset 0. */
    if (off == 0xFFFFFFFFU) return -27;                    /* -EFBIG */
    if (len > 0xFFFFFFFFU - off) len = 0xFFFFFFFFU - off;
    uint32_t bsz;
    uint8_t *kbuf = bounce_alloc(len, &bsz);
    if (!kbuf) return -12;
    uint32_t done = 0;
    int err = 0;
    while (done < len) {
        uint32_t want = len - done < bsz ? len - done : bsz;
        if (copy_from_user(kbuf, ubuf + done, want) < 0) { err = -14; break; }
        uint32_t w = vfs_write(n, off + done, want, kbuf);
        /* Out of kernel memory for the file body.  Linux tmpfs returns ENOMEM
         * from shmem_alloc_and_acct_folio for exactly this; returning 0 would
         * spin any libc write loop. */
        if (w == VFS_WRITE_ENOMEM) { err = -12; break; }
        if (w == VFS_WRITE_EFBIG)  { err = -27; break; }
        if ((int32_t)w < 0)        { err = (int32_t)w; break; }
        if (w > want) w = want;
        done += w;
        if (w < want) break;
    }
    kfree(kbuf);
    return done ? (int)done : err;
}

/* MSG_NOSIGNAL as seen by sock_send_user (net/socket.h has the other bits). */
#define SOCK_MSG_NOSIGNAL 0x4000

/* One net_socket_recvfrom() into user `ubuf` (a datagram must arrive whole,
 * and a stream read may block, so exactly one call) — except that
 * MSG_WAITALL keeps reading bounce-buffer-sized pieces until `len` is filled
 * or a read comes up short.  `flags` are NET_MSG_*. */
static int sock_recv_user(net_socket_t *s, void *ubuf, uint32_t len,
                          net_sockaddr_in_t *addr, int flags) {
    uint32_t bsz;
    uint8_t *kbuf = bounce_alloc(len, &bsz);
    if (!kbuf) return -12;
    uint32_t done = 0;
    int r;
    do {
        uint32_t want = len - done < bsz ? len - done : bsz;
        r = net_socket_recvfrom(s, kbuf, want, addr, flags);
        if (r > 0 && copy_to_user((uint8_t *)ubuf + done, kbuf, (uint32_t)r) < 0)
            r = -14;
        if (r <= 0) break;
        done += (uint32_t)r;
        if ((uint32_t)r < want) break;
    } while ((flags & NET_MSG_WAITALL) && !(flags & NET_MSG_PEEK) && done < len);
    kfree(kbuf);
    return done ? (int)done : r;
}

/* net_socket_sendto() of user `ubuf`, in chunks (any valid datagram fits in
 * the first), stopping at a short send. */
static int sock_send_user(net_socket_t *s, const void *ubuf, uint32_t len,
                          const net_sockaddr_in_t *addr, int flags) {
    uint32_t bsz;
    uint8_t *kbuf = bounce_alloc(len, &bsz);
    if (!kbuf) return -12;
    uint32_t done = 0;
    int err = 0;
    do {
        uint32_t want = len - done < bsz ? len - done : bsz;
        if (copy_from_user(kbuf, (const uint8_t *)ubuf + done, want) < 0) { err = -14; break; }
        int r = net_socket_sendto(s, kbuf, want, addr, flags);
        if (r < 0) { err = r; break; }
        if ((uint32_t)r > want) r = (int)want;
        done += (uint32_t)r;
        if ((uint32_t)r < want) break;
    } while (done < len);
    kfree(kbuf);
    /* Linux sk_stream_error: EPIPE raises SIGPIPE unless MSG_NOSIGNAL. */
    if (!done && err == -32 && !(flags & SOCK_MSG_NOSIGNAL))
        signal_send(current_proc, SIGPIPE);
    return done ? (int)done : err;
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
    /* si_code/si_addr for the trap, as arch/x86/kernel/traps.c fills them: a
     * fault in the instruction stream reports the instruction's address, a
     * #GP (and friends) is a kernel-raised SIGSEGV with no address. */
    int      code = SI_KERNEL;
    uint32_t addr = 0;
    switch (regs->int_no) {
    case 0:  code = FPE_INTDIV; addr = regs->eip; break;  /* divide error */
    case 6:  code = ILL_ILLOPN; addr = regs->eip; break;  /* invalid opcode */
    case 17: code = BUS_ADRALN; addr = regs->eip; break;  /* alignment check */
    case 16: case 19: code = SI_KERNEL; addr = regs->eip; break;  /* x87/SIMD */
    default: break;
    }
    signal_send_fault(current_proc, sig, code, addr);
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
 * vfs_lookup_at — vfs_lookup() for a path that may be relative to the cwd.
 * cwd + '/' + path is joined with a length check (-ENAMETOOLONG past the VFS
 * path limit) and, on failure, *err (if given) says why: -ENOENT, -ELOOP or
 * -ENAMETOOLONG, so callers can report the real error instead of -ENOENT.
 */
#define LOOKUP_PATH_MAX 512
static vfs_node_t *vfs_lookup_at(const char *path, int follow, int *err) {
    if (err) *err = -2;                                       /* -ENOENT */
    if (!path) return NULL;
    if (path[0] == '/') return vfs_lookup(path, follow, err);
    if (!current_proc) return NULL;
    char abspath[LOOKUP_PATH_MAX];
    uint32_t cwdlen = (uint32_t)__builtin_strlen(current_proc->cwd);
    uint32_t pathlen = (uint32_t)__builtin_strlen(path);
    if (cwdlen + 1 + pathlen >= sizeof(abspath)) {
        if (err) *err = -36;                                  /* -ENAMETOOLONG */
        return NULL;
    }
    __builtin_memcpy(abspath, current_proc->cwd, cwdlen);
    if (cwdlen > 1) abspath[cwdlen++] = '/';
    __builtin_memcpy(abspath + cwdlen, path, pathlen + 1);
    return vfs_lookup(abspath, follow, err);
}

/* vfs_open_at — like vfs_open() but handles relative paths by prepending cwd.
 * Requires current_proc to be set. */
static vfs_node_t *vfs_open_at(const char *path) {
    return vfs_lookup_at(path, 1, NULL);
}

static int path_is_root(const char *path) {
    return path && path[0] == '/' && path[1] == '\0';
}

static vfs_node_t *vfs_open_parent_at(const char *full_path,
                                      const char *dir_path) {
    if (full_path && full_path[0] == '/' && path_is_root(dir_path) &&
        !current_proc->root_node && vfs_path_uses_root_overlay(full_path)) {
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

    if (!type && n->flags == VFS_FLAG_SOCK) type = 0140000U;   /* S_IFSOCK */
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
    if (t == VFS_FLAG_SOCK) return 12;  /* DT_SOCK */
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
    st->st_dev     = (uint16_t)n->dev;
    st->st_rdev    = (uint16_t)n->rdev;
    st->st_ino     = n->inode;
    st->st_mode    = (uint16_t)vnode_mode(n);
    st->st_nlink   = n->nlink ? (uint16_t)n->nlink : 1;
    /* Legacy 16-bit ids: anything wider reads as overflowuid (65534). */
    st->st_uid     = n->uid > 0xFFFFU ? 65534U : (uint16_t)n->uid;
    st->st_gid     = n->gid > 0xFFFFU ? 65534U : (uint16_t)n->gid;
    st->st_size    = n->size;
    st->st_blksize = 4096;
    st->st_blocks  = (n->size + 511) / 512;
    st->st_atime   = n->atime;
    st->st_mtime   = n->mtime;
    st->st_ctime   = n->ctime;
}

static void fill_kstat64(struct kstat64 *st, vfs_node_t *n) {
    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev     = n->dev;
    st->st_rdev    = n->rdev;
    st->st_ino     = n->inode;
    st->__st_ino   = n->inode;
    st->st_mode    = vnode_mode(n);
    st->st_nlink   = n->nlink ? n->nlink : 1;
    st->st_uid     = n->uid;     /* the owner the permission checks use */
    st->st_gid     = n->gid;
    st->st_size    = (int64_t)n->size;
    st->st_blksize = 4096;
    st->st_blocks  = (n->size + 511) / 512;
    st->st_atime   = n->atime;
    st->st_mtime   = n->mtime;
    st->st_ctime   = n->ctime;
}

/* stat as fstat(2) / statx(AT_EMPTY_PATH) see a DESCRIPTOR.  Only FD_FILE has
 * a vfs_node_t behind it; every other kind of descriptor gets the mode Linux
 * reports for that object (S_IFIFO for a pipe, S_IFSOCK for a socket, an
 * anonymous inode for eventfd/epoll), which is what makes fstat() on a pipe
 * received over SCM_RIGHTS report S_ISFIFO instead of the root directory. */
static int fd_kstat64(int fd, struct kstat64 *kst) {
    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    __builtin_memset(kst, 0, sizeof(*kst));
    kst->st_nlink   = 1;
    kst->st_blksize = 4096;
    switch (f->type) {
    case FD_FILE:
        if (!f->node) return -9;
        fill_kstat64(kst, f->node);
        return 0;
    case FD_PIPE_R: case FD_PIPE_W:
        kst->st_mode = 0010666;                 /* S_IFIFO  */
        return 0;
    case FD_SOCKET: case FD_USOCKET:
        kst->st_mode = 0140777;                 /* S_IFSOCK */
        return 0;
    case FD_NONE:
        if (fd > 2) return -9;                  /* -EBADF */
        kst->st_mode = 0020666;                 /* stdio: a char device */
        return 0;
    default:
        kst->st_mode = 0100600;                 /* anon inode: eventfd, epoll */
        return 0;
    }
}

static int sys_mkdir_kernel_path(const char *path, uint32_t mode);
static int sys_unlink_kernel_path(const char *path);
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
    /* Copy the value out before consuming it: buf is a user pointer, and a
     * bad one must fail with -EFAULT and leave the counter as it was. */
    uint64_t out = (e->flags & EFD_SEMAPHORE) ? 1 : e->count;
    if (copy_to_user(buf, &out, 8) < 0) return -14;
    e->count -= out;
    wake_up(e);                                    /* wake blocked writers */
    io_wake();                                     /* wake pollers (space avail) */
    return 8;
}

static int eventfd_write(struct eventfd_obj *e, const char *buf, int len, int nonblock) {
    if (len < 8) return -22;                        /* -EINVAL */
    uint64_t add;
    if (copy_from_user(&add, buf, 8) < 0) return -14;
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

/* The descriptor's access mode (Linux FMODE_READ/FMODE_WRITE).  f->flags also
 * carries status flags (O_APPEND, O_NONBLOCK via F_SETFL), so the mode must be
 * masked out — comparing the whole word against O_RDONLY let a read-only
 * descriptor write once any status flag was set. */
static inline int fd_readable(const proc_file_t *f) {
    return (f->flags & O_ACCMODE) != O_WRONLY;
}
static inline int fd_writable(const proc_file_t *f) {
    uint32_t m = f->flags & O_ACCMODE;
    return m == O_WRONLY || m == O_RDWR;
}

/* Remember which mount(2) mount the file `path` names lives on. */
static void fd_set_mnt(proc_file_t *f, const char *path) {
    f->mnt = NULL;
    f->mnt_seq = 0;
    if (!vfs_mounts_active() || !path || path[0] != '/') return;
    int err;
    vfs_mnt_t *m = NULL;
    if (vfs_lookup_mnt(path, 1, &err, &m) && m) {
        f->mnt = m;
        f->mnt_seq = m->seq;
    }
}

/* Changes made through a descriptor (fchmod, fchown, ftruncate, fallocate)
 * are -EROFS on a read-only mount, as the path-based ones are. */
static int fd_rofs(const proc_file_t *f) {
    return f->type != FD_NONE && vfs_mnt_rdonly(f->mnt, f->mnt_seq);
}

/* Permission check of `node` for the caller's effective ids and supplementary
 * groups (Linux inode_permission with current_fsuid()/in_group_p()). */
static int proc_access_check(vfs_node_t *node, int want) {
    struct proc *p = current_proc;
    return vfs_access_check_groups(node, p->euid, p->egid, p->groups,
                                   p->ngroups, want);
}

/* Linux in_group_p(): `g` is the caller's effective gid or one of its
 * supplementary groups. */
static int proc_in_group(uint32_t g) {
    struct proc *p = current_proc;
    if (g == p->egid) return 1;
    for (uint32_t i = 0; i < p->ngroups; i++)
        if (p->groups[i] == g) return 1;
    return 0;
}

/* Every copy of an open file (fork, dup, SCM_RIGHTS) must carry the same
 * epoll identity, so the source gets one BEFORE it is copied: an fid handed
 * out only at EPOLL_CTL_ADD time would reach the adder's entry alone, and a
 * copy made earlier (a forked sharer of the epoll, say) would never match. */
uint32_t fd_new_fid(void) {
    static uint32_t next_fid = 1;
    uint32_t id;
    do { id = next_fid++; } while (!id);
    return id;
}

/* *dst = *src plus a reference: the one way to duplicate a descriptor. */
void fd_copy(proc_file_t *dst, proc_file_t *src) {
    if (src->type != FD_NONE && !src->fid) src->fid = fd_new_fid();
    *dst = *src;
    fd_retain(dst);
}

void fd_retain(proc_file_t *f) {
    if (f->type == FD_FILE)    vfs_retain(f->node);
    /* A named pipe's descriptor also holds the FIFO's vfs node; an anonymous
     * pipe leaves node NULL and vfs_retain/vfs_close ignore it. */
    if (f->type == FD_PIPE_R)  { f->pipe->nreaders++; vfs_retain(f->node); }
    if (f->type == FD_PIPE_W)  { f->pipe->nwriters++; vfs_retain(f->node); }
    if (f->type == FD_SOCKET)  net_socket_retain(f->socket);
    if (f->type == FD_USOCKET) usocket_retain(f->usock);
    if (f->type == FD_EPOLL)   epoll_retain(f->epoll);
    if (f->type == FD_EVENTFD) f->efd->refcount++;
}

void fd_release(proc_file_t *f) {
    flock_fd_closed(f);        /* before the node and fid are gone */
    if (f->type == FD_FILE)    vfs_close(f->node);
    if (f->type == FD_PIPE_R)  { pipe_close_read(f->pipe);  vfs_close(f->node); }
    if (f->type == FD_PIPE_W)  { pipe_close_write(f->pipe); vfs_close(f->node); }
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
    f->fid     = 0;
    f->path[0] = '\0';
    f->mnt     = NULL;
    f->mnt_seq = 0;
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
    flock_owner_gone(t);                 /* its POSIX record locks */
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

/* /proc/<pid>/{cmdline,environ,auxv,exe} describe the ADDRESS SPACE on Linux,
 * so a child reads the same as its creator — a forked one until it execs and
 * replaces them, a thread for as long as it lives.  Neither path used to carry
 * them: before allocproc() started clearing the slot, a new task reported
 * whatever the slot's previous occupant had left there. */
static void proc_copy_image_ids(struct proc *child, struct proc *parent) {
    __builtin_memcpy(child->cmdline, parent->cmdline, sizeof(child->cmdline));
    child->cmdline_len = parent->cmdline_len;
    __builtin_memcpy(child->environ, parent->environ, sizeof(child->environ));
    child->environ_len = parent->environ_len;
    __builtin_memcpy(child->auxv_data, parent->auxv_data, sizeof(child->auxv_data));
    child->auxv_bytes = parent->auxv_bytes;
    __builtin_memcpy(child->exe, parent->exe, sizeof(child->exe));
}

/* Core fork.  child_stack==0 → child shares the parent's stack pointer (classic
 * fork).  child_stack!=0 → child runs on that user stack instead (clone without
 * CLONE_VM but WITH a stack — e.g. Google Breakpad's crash dumper, which clones
 * a frozen copy of the address space onto a fresh stack and runs an entry fn).
 * Honouring child_stack here is essential: otherwise the child runs the glibc
 * clone trampoline on the parent's stack, pops garbage as its entry fn, and
 * jumps into the weeds. */
/* Undo a half-built fork child: everything allocproc and do_fork gave it
 * before the address-space copy failed (the fds are copied only after that
 * point, so its table is still empty).  Leaking these cost ~150 KiB of heap
 * per failed fork, which made the next fork under pressure likelier to fail. */
static void fork_abort(struct proc *child) {
    vma_clear(child);
    fdtable_put(child);
    if (child->sighand) { sighand_put(child->sighand); child->sighand = NULL; }
    sigshared_put(child->sigshared);
    child->sigshared = NULL;
    if (child->ctty) { vfs_close(child->ctty); child->ctty = NULL; }
    if (child->root_node) { vfs_close(child->root_node); child->root_node = NULL; }
    kstack_free(child->kstack);
    child->kstack = NULL;
    child->state = PROC_UNUSED;
}

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
    proc_copy_image_ids(child, parent);

    /* Inherit a private COPY of the handler table (Linux copy_sighand without
     * CLONE_SIGHAND); clear pending signals in child, keep the blocked mask. */
    if (parent->sighand) {
        __builtin_memcpy(child->sighand->handlers, parent->sighand->handlers,
                         sizeof(child->sighand->handlers));
        __builtin_memcpy(child->sighand->flags, parent->sighand->flags,
                         sizeof(child->sighand->flags));
        __builtin_memcpy(child->sighand->mask, parent->sighand->mask,
                         sizeof(child->sighand->mask));
    }
    child->pending_sigs  = 0;
    child->blocked_sigs  = parent->blocked_sigs;
    child->sigframe_addr = 0;
    child->sas_sp        = parent->sas_sp;
    child->sas_size      = parent->sas_size;

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
    child->root_node = parent->root_node;
    if (child->root_node) vfs_retain(child->root_node);
    child->heap_end  = fowner->heap_end;
    child->read_implies_exec = parent->read_implies_exec;
    child->exec_stack        = parent->exec_stack;
    child->umask     = parent->umask;
    child->uid = parent->uid; child->gid = parent->gid;
    child->euid = parent->euid; child->egid = parent->egid;
    child->suid = parent->suid; child->sgid = parent->sgid;
    child->ngroups = parent->ngroups;
    __builtin_memcpy(child->groups, parent->groups, sizeof(child->groups));
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
    if ((clone_flags & CLONE_PARENT_SETTID) && uptid) {
        /* Best effort, like Linux put_user() here: a bad pointer is ignored,
         * never stored through raw (a read-only page would fault in ring 0). */
        uint32_t tid = (uint32_t)child->pid;
        (void)copy_to_user((void *)(uintptr_t)uptid, &tid, sizeof(tid));
    }
    child->set_child_tid   = (clone_flags & CLONE_CHILD_SETTID)   ? uctid : 0;
    child->clear_child_tid = (clone_flags & CLONE_CHILD_CLEARTID) ? uctid : 0;

    vma_clone(parent, child);     /* fork: child gets its own copy of the VMAs */

    /* Create child's page directory with kernel mappings */
    child->pgdir_phys = pgdir_create();
    if (!child->pgdir_phys) {
        fork_abort(child);
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
    volatile void *parent_pd = (volatile void *)(uintptr_t)PAGE_DIR_VIRT;

    for (uint32_t pde_idx = 0; pde_idx < USER_PT_COUNT; pde_idx++) {
        if (!(tbl_get(parent_pd, pde_idx) & PAGE_PRESENT)) continue;

        /* Allocate a new page table for the child */
        uint32_t child_pt_phys = pmm_alloc_frame();
        if (!child_pt_phys) {
            pgdir_free_user(child->pgdir_phys);
            child->pgdir_phys = 0;
            fork_abort(child);
            return -11;
        }

        /* Access the child's PT via TEMP_MAP_VIRT, zero it, then populate */
        volatile void *child_pt = paging_temp_map(child_pt_phys);
        __builtin_memset((void *)child_pt, 0, PAGE_SIZE);

        /* Access parent's PT via the recursive mapping */
        volatile void *parent_pt = pt_window(pde_idx);

        for (uint32_t pte_idx = 0; pte_idx < PT_ENTRIES; pte_idx++) {
            pte_t pte = tbl_get(parent_pt, pte_idx);
            /* PAGE_PROTNONE entries own a frame too (mprotect(PROT_NONE)). */
            if (!(pte & (PAGE_PRESENT | PAGE_PROTNONE)))
                continue;

            phys_t frame_phys = pte_frame(pte);

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
                pte_t cow_pte = (pte & ~(pte_t)PAGE_WRITABLE) | PAGE_COW;
                if (cow_pte != pte) tbl_set(parent_pt, pte_idx, cow_pte);
                tbl_set(child_pt, pte_idx, cow_pte);
            } else {
                /* Read-only or shared-memory page: share directly.  Shared
                 * pages must stay writable in both — COW would silently
                 * un-share them. */
                tbl_set(child_pt, pte_idx, pte);
            }
            pmm_frame_incref(frame_phys);
        }

        paging_temp_unmap();   /* release child_pt (TEMP_MAP_VIRT) */

        /* Install child's PT in child's directory */
        pgdir_install_pt(child->pgdir_phys, pde_idx,
                         child_pt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

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
        fd_copy(&child->ofile[i], &parent->ofile[i]);
    }

    /* Inherit shared-memory mapping records (PTEs already cloned above) */
    shm_proc_fork(parent, child);

    /* clone() with an explicit child stack: run the child there (see do_fork
     * banner).  Set BEFORE marking RUNNABLE — under SMP the other CPU may
     * dispatch the child the instant it becomes runnable. */
    if (child_stack)
        child->tf->useresp = child_stack;

    child->state = PROC_RUNNABLE;

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
        if (!fd_readable(f)) return -9;    /* opened write-only */
        int n = vfs_read_user(f->node, f->offset, buf, (uint32_t)len);
        if (n > 0) f->offset += (uint32_t)n;
        return n;
    }

    /* Fallback: serial stdin for fd=0 when no fd entry */
    if (fd == 0 && f->type == FD_NONE) {
        int n = 0;
        while (n < len) {
            char c = console_serial_getc();
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
            if (copy_to_user(buf + n, &c, 1) < 0) return n ? n : -14;
            n++;
            if (c == '\n') break;
        }
        return n;
    }

    if (f->type == FD_SOCKET)
        return sock_recv_user(f->socket, buf, (uint32_t)len, NULL,
                              (f->flags & O_NONBLOCK) ? NET_MSG_DONTWAIT : 0);

    if (f->type == FD_USOCKET) {
        /* Pinned across the sleep: a sibling thread may close the fd. */
        usocket_t *us = f->usock;
        usocket_pin(us);
        int r = usocket_read(us, buf, len, (f->flags & O_NONBLOCK) != 0);
        usocket_unpin(us);
        return r;
    }

    return -9;  /* -EBADF */
}

/* ── sys_write(int fd, const void *buf, size_t len) — EAX=4 ──────────── */
static int sys_write(registers_t *regs) {
    int         fd  = (int)regs->ebx;
    const char *buf = (const char *)(uintptr_t)regs->ecx;
    int         len = (int)(uint32_t)regs->edx;

    if (len < 0 || !access_ok(buf, (size_t)len)) {
        ktrace("[SYSCALL] sys_write: bad user ptr 0x%08x\n", (unsigned)regs->ecx);
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
        if (!fd_writable(f)) return -9;        /* opened read-only */
        if (!f->node->write_fn)   return -9;   /* node not writable */
        /* O_APPEND: every write goes to the current end of file (Linux
         * generic_write_checks), whether it was set at open or by F_SETFL. */
        if (f->flags & O_APPEND) f->offset = f->node->size;
        int written = vfs_write_user(f->node, f->offset, buf, (uint32_t)len);
        if (written > 0) f->offset += (uint32_t)written;
        return written;
    }

    /* Fallback: fd=1/2 with no entry → serial+VGA stdout */
    if (f->type == FD_NONE && (fd == 1 || fd == 2)) {
        for (int i = 0; i < len; i++) {
            char c;
            if (copy_from_user(&c, buf + i, 1) < 0) return i ? i : -14;
            serial_putc(c);
            vga_putchar(c);
        }
        return len;
    }

    if (f->type == FD_SOCKET)   /* write() on a socket: SIGPIPE on EPIPE */
        return sock_send_user(f->socket, buf, (uint32_t)len, NULL,
                              (f->flags & O_NONBLOCK) ? NET_MSG_DONTWAIT : 0);

    if (f->type == FD_USOCKET) {
        usocket_t *us = f->usock;
        usocket_pin(us);
        int r = usocket_write(us, buf, len, (f->flags & O_NONBLOCK) != 0);
        usocket_unpin(us);
        return r;
    }

    return -9;  /* -EBADF */
}

/* ── sys_waitpid(pid_t pid, int *status, int options) — EAX=7 ─────────── */
#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

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

            /* A zombie leader whose siblings still run is not reapable yet, but
             * the process can still be stopped or continued (below). */
            if (p->state == PROC_ZOMBIE && proc_group_empty(p)) {
                int child_pid = p->pid;
                if (status_ptr) {
                    int status = p->exit_status;
                    int cr = copy_to_user(status_ptr, &status, sizeof(status));
                    if (cr < 0) return cr;
                }
                proc_release(p);
                return child_pid;
            }

            /* WUNTRACED: also report a stopped child — once per stop, with
             * the signal that stopped it (Linux wait_task_stopped clears the
             * group's stop code as it reports it).  The PROCESS is stopped
             * once every live thread of it has stopped (group stop). */
            if ((options & WUNTRACED) && proc_group_stopped(p) &&
                !p->stop_reported) {
                if (status_ptr) {
                    int sig = p->stop_sig ? p->stop_sig : SIGSTOP;
                    int status = ((sig & 0xff) << 8) | 0x7f;
                    int cr = copy_to_user(status_ptr, &status, sizeof(status));
                    if (cr < 0) return cr;
                }
                p->stop_reported = 1;
                return p->pid;
            }

            /* WCONTINUED: a SIGCONT ended a stop — reported once, as 0xffff
             * (Linux wait_task_continued). */
            if ((options & WCONTINUED) && p->group_continued) {
                if (status_ptr) {
                    int status = 0xffff;
                    int cr = copy_to_user(status_ptr, &status, sizeof(status));
                    if (cr < 0) return cr;
                }
                p->group_continued = 0;
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

/* Owner, group and mode of a node the caller has just created in `dir`
 * (Linux inode_init_owner + mode_strip_sgid).  `mode` is the requested mode
 * with the umask already applied.  The owner is the EFFECTIVE uid (Linux
 * fsuid): a set-uid-root program creates root-owned files.  The group is the
 * effective gid, unless the directory is set-group-ID: then it is the
 * directory's group, a new subdirectory inherits the set-group-ID bit, and a
 * set-group-ID executable the caller could not have made by chmod (not a
 * member of that group, not root) loses the bit. */
static void init_new_node(vfs_node_t *dir, vfs_node_t *node, uint32_t mode) {
    struct proc *p = current_proc;
    uint32_t gid = p->egid;
    mode &= 07777;
    if (dir && (dir->mask & 02000)) {
        gid = dir->gid;
        if (node->flags == VFS_FLAG_DIR)
            mode |= 02000;
        else if ((mode & 02010) == 02010 && p->euid != 0 && !proc_in_group(gid))
            mode &= ~02000u;
    }
    vfs_setattr(node, mode, p->euid, gid);
}

static int sys_open_kernel_path(const char *path, int flags, uint32_t mode) {
    /* Anything that could change the object needs a writable mount. */
    if (((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) &&
        vfs_path_rdonly(path, 0))
        return -30;                                           /* -EROFS */
    /* O_CREAT|O_EXCL: the name must not exist in any form — not even as a
     * dangling symlink, which is why the final component is not followed
     * (Linux open(2); mkstemp and lock files rely on this). */
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        char abspath[256];
        if (canonicalize_path_at_cwd(path, abspath, sizeof(abspath)) == 0 &&
            vfs_open_nofollow(abspath))
            return -17;   /* -EEXIST */
    }
    int lerr;
    vfs_node_t *node = vfs_lookup_at(path, 1, &lerr);
    if (!node) {
        /* O_CREAT: create the file if missing — only when it is missing,
         * not when the lookup hit a symlink loop or an over-long path. */
        if (!(flags & O_CREAT) || lerr != -2)
            return lerr;

        char dir_path[256], base[256];
        if (path_split(path, dir_path, base) < 0)
            return -2;

        vfs_node_t *dir = vfs_open_parent_at(path, dir_path);
        if (!dir || !dir->create_fn)
            return -2;

        /* Need write+search on the parent directory to create here. */
        if (proc_access_check(dir, VFS_WANT_W | VFS_WANT_X) < 0)
            return -13;

        int cr = dir->create_fn(dir, base, VFS_FLAG_FILE);
        if (cr < 0)
            return cr == -1 ? -12 : cr;   /* -EEXIST etc. pass through */

        node = vfs_open_at(path);
        if (!node) return -2;
        /* The mode asked for, less the umask (Linux open(2)).  A file created
         * by this open is opened with the access mode requested even when
         * its new mode would not allow that (open(O_CREAT|O_RDWR, 0444)). */
        init_new_node(dir, node, mode & ~current_proc->umask);
    } else {
        /* Linux do_open: a directory can only be opened for reading. */
        if (node->flags == VFS_FLAG_DIR &&
            ((flags & O_CREAT) || (flags & O_ACCMODE) != O_RDONLY))
            return -21;   /* -EISDIR */
        /* Existing node: check requested access mode. */
        int rw = flags & 3;
        int want = (rw == O_WRONLY) ? VFS_WANT_W
                 : (rw == O_RDWR)   ? (VFS_WANT_R | VFS_WANT_W)
                 : VFS_WANT_R;
        if (flags & O_TRUNC) want |= VFS_WANT_W;
        if (proc_access_check(node, want) < 0)
            return -13;   /* -EACCES */
    }

    /* A socket inode is reached with connect(), never opened (Linux
     * no_open → -ENXIO). */
    if (node->flags == VFS_FLAG_SOCK) return -6;

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
                vfs_retain(node);              /* see the FD_FILE case below */
                current_proc->ofile[i].pipe    = pb;
                current_proc->ofile[i].node    = node;
                current_proc->ofile[i].flags   = flags;  /* incl. O_NONBLOCK */
                current_proc->ofile[i].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
                __builtin_memcpy(current_proc->ofile[i].path, path,
                                 __builtin_strlen(path) + 1);
                fd_set_mnt(&current_proc->ofile[i], path);
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
            /* Cloning device (/dev/ptmx): the descriptor holds a fresh node,
             * not the one the lookup returned.  This runs here, after the
             * permission check and after the slot is known to exist, so the
             * only lookups that reserve a pty are the ones that become an open
             * — a stat(), an access() or a failed open() reserves nothing.
             * See the open_fn comment in fs/vfs.h. */
            if (node->open_fn) {
                vfs_node_t *clone = node->open_fn(node);
                if (!clone) return -2;   /* -ENOENT: no capacity left */
                node = clone;
            }
            /* The descriptor is a long-lived reference to the node, so it takes
             * one: fd_release() drops it again, and fd_retain() adds one per
             * dup/fork.  Without this a tmpfs file unlinked while open was freed
             * under the descriptor and the next close jumped through a recycled
             * function pointer. */
            vfs_retain(node);
            current_proc->ofile[i].type    = FD_FILE;
            current_proc->ofile[i].node    = node;
            current_proc->ofile[i].offset  = (flags & O_APPEND) ? node->size : 0;
            /* Access mode plus the status flags F_GETFL reports and write()
             * honours (O_APPEND re-seeks to EOF before every write). */
            current_proc->ofile[i].flags   = flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
            current_proc->ofile[i].cloexec = (flags & O_CLOEXEC) ? 1 : 0;
            __builtin_memcpy(current_proc->ofile[i].path, path,
                             __builtin_strlen(path) + 1);
            fd_set_mnt(&current_proc->ofile[i], path);
            return i;
        }
    }
    return -24;  /* -EMFILE: too many open files */
}

/* ── sys_open(const char *path, int flags, int mode) — EAX=5 ─────────────── */

static int sys_open(registers_t *regs) {
    const char *upath = (const char *)(uintptr_t)regs->ebx;
    int         flags = (int)regs->ecx;
    uint32_t    mode  = regs->edx;

    if (!access_ok(upath, 1))
        return -14;  /* -EFAULT */

    char path[256];
    int r = copy_user_str(upath, path, 256);
    if (r < 0) return r;

    char resolved[256];
    r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    int rc = sys_open_kernel_path(resolved, flags, mode);
    return rc;
}

/* ── sys_creat(const char *path, mode_t mode) — EAX=8 ───────────────────────
 * open(path, O_CREAT|O_WRONLY|O_TRUNC, mode) (Linux fs/open.c). */
static int sys_creat(registers_t *regs) {
    char path[256], resolved[256];
    int r = copy_user_str((const char *)(uintptr_t)regs->ebx, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return sys_open_kernel_path(resolved, O_CREAT | O_WRONLY | O_TRUNC, regs->ecx);
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

static void unmap_pages(uint32_t start, uint32_t end);

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
            if (va < old_brk && pde_present(va) &&
                (pte_get(va) & PAGE_PRESENT))
                continue;

            phys_t phys = pmm_alloc_user_frame();
            if (!phys) return -12;  /* -ENOMEM */
            pmm_frame_incref(phys);
            if (paging_map(va, phys,
                           PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
                           page_nx_unless(current_proc->read_implies_exec)) != 0) {
                pmm_frame_decref(phys);
                return -12;         /* -ENOMEM; heap_end unchanged, so the
                                     * pages mapped so far are simply spare */
            }
            /* Zero the new page */
            __builtin_memset((void *)va, 0, PAGE_SIZE);
        }
    } else if (new_brk < old_brk) {
        /* Shrink heap: unmap and free pages from new_brk up to old_brk.  The
         * frames are released only after the TLB shootdown (unmap_pages): a
         * sibling thread on another CPU may still hold a stale entry for them,
         * and freeing first let it write into a frame already reallocated. */
        uint32_t va  = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint32_t end = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (va < end) unmap_pages(va, end);
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
 * byte by byte — the strings Linux allows are up to 128 KiB.  Each chunk is
 * clamped to what is left of the budget, so a short string near ARG_MAX is not
 * refused for the page-sized chunk it was read in, and every exit that does
 * not keep the string refunds all it charged. */
static int es_push_user(struct exec_strings *v, const char *up) {
    uint32_t start = v->used, got = 0, charged = 0;
    int rc;
    for (;;) {
        uint32_t chunk = PAGE_SIZE - (((uint32_t)(uintptr_t)up + got) & (PAGE_SIZE - 1));
        uint32_t left  = EXEC_ARG_MAX - *v->total;
        if (chunk > left) chunk = left;
        /* Charge FIRST: the budget has to stop us before the allocation, not
         * after the whole vector has been copied. */
        if (chunk == 0 || es_charge(v, chunk) < 0) { rc = -7; break; }  /* -E2BIG */
        charged += chunk;
        if (es_reserve(v, chunk) < 0) { rc = -12; break; }
        if (copy_from_user(v->buf + start + got, up + got, chunk) < 0) {
            rc = -14;
            break;
        }
        for (uint32_t i = 0; i < chunk; i++)
            if (v->buf[start + got + i] == '\0') {
                uint32_t len = got + i + 1;
                *v->total -= charged - len;   /* refund the unused tail */
                charged = len;
                v->used = start + len;
                rc = es_index(v, start);
                if (rc == 0) return 0;
                goto fail;
            }
        got += chunk;
        v->used = start + got;             /* es_reserve() appends after used */
        if (got > EXEC_MAX_ARG_STRLEN) { rc = -7; break; }     /* -E2BIG */
    }
fail:
    *v->total -= charged;
    v->used = start;
    return rc;
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
    if (me->sigshared) me->sigshared->pending &= ~(1u << SIGKILL);
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
        ktrace("[execfail] '%s' pid=%d ENOENT (open failed)\n", path, current_proc->pid);
        EXEC_FAIL(-2);   /* -ENOENT */
    }

    /* Linux search_binary_handler(): a "#!" script names an interpreter that
     * is itself exec'd — and may be a script in turn — up to
     * BINPRM_MAX_RECURSION (4) levels deep before -ELOOP.  Every file in the
     * chain has to be a regular file the caller may execute, and the set-ID
     * bits that count are the ones on the file finally loaded: a set-uid
     * script is run by its interpreter with the caller's own ids. */
    for (int depth = 0; ; depth++) {
        if (node->flags != VFS_FLAG_FILE) {
            ktrace("[execfail] '%s' pid=%d EACCES (not a regular file)\n",
                   path, current_proc->pid);
            EXEC_FAIL(-13);   /* -EACCES */
        }
        if (proc_access_check(node, VFS_WANT_X) < 0) {
            ktrace("[execfail] '%s' pid=%d EACCES (uid=%d gid=%d mode=%o)\n",
                   path, current_proc->pid, (int)current_proc->euid,
                   (int)current_proc->egid, (unsigned)node->mask);
            EXEC_FAIL(-13);   /* -EACCES */
        }

        char shebang_line[512];
        uint32_t shebang_read = vfs_read(node, 0, sizeof(shebang_line) - 1,
                                         (uint8_t *)shebang_line);
        shebang_line[shebang_read] = '\0';
        if (shebang_read < 2 || shebang_line[0] != '#' || shebang_line[1] != '!')
            break;                                 /* not a script: load it */

        const char *lp = shebang_line + 2;
        /* skip leading spaces */
        while (*lp == ' ') lp++;
        /* extract interpreter path */
        char interp[256];
        int ii = 0;
        while (*lp && *lp != '\n' && *lp != ' ' && *lp != '\r' && ii < 255)
            interp[ii++] = *lp++;
        interp[ii] = '\0';
        if (ii == 0) EXEC_FAIL(-8);                 /* -ENOEXEC: "#!" alone */
        if (depth >= 4) EXEC_FAIL(-40);             /* -ELOOP */
        /* extract optional interpreter argument */
        while (*lp == ' ') lp++;
        char interp_arg[256];
        int ai = 0;
        while (*lp && *lp != '\n' && *lp != '\r' && ai < 255)
            interp_arg[ai++] = *lp++;
        interp_arg[ai] = '\0';

        /* Rebuild argv: [interp, interp_arg?, script_path, orig_argv[1..]].
         * From the second level on, `av` is itself a rebuilt vector charged to
         * shebang_budget; its charge is returned once it has been freed. */
        uint32_t prev_charge = depth ? shebang_budget : 0;
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
        shebang_budget -= prev_charge;

        /* Re-resolve to interpreter */
        __builtin_memcpy(path, interp, (size_t)ii + 1);
        node = vfs_open_at(path);
        if (!node) EXEC_FAIL(-2);
    }

    /* set-user-ID / set-group-ID bit of the file being loaded: run with the
     * file owner's effective id (e.g. doas, passwd are owned by root mode
     * 04755).  The real id is unchanged; the saved id follows the new
     * effective id below, as Linux commit_creds() leaves it after exec. */
    uint32_t new_euid = current_proc->euid;
    uint32_t new_egid = current_proc->egid;
    if (node->mask & 04000) new_euid = node->uid;
    if (node->mask & 02000) new_egid = node->gid;

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
    /* Execute policy of the new image (Linux load_elf_binary): no
     * PT_GNU_STACK → READ_IMPLIES_EXEC; PT_GNU_STACK with PF_X → an
     * executable stack. */
    int new_rie        = einfo.stack_flags < 0;
    int new_stack_exec = new_rie || (einfo.stack_flags & PF_X);
    if (paging_map_sigpage(new_pgdir) != 0) {
        pgdir_free_user(new_pgdir);
        EXEC_FAIL(-12);
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
            phys_t stack_phys = pmm_alloc_user_frame();
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
            if (pgdir_map(new_pgdir, va, stack_phys,
                          PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
                          page_nx_unless(new_stack_exec)) != 0) {
                pmm_frame_decref(stack_phys);
                kfree(uargv_ptrs);
                kfree(uenvp_ptrs);
                kfree(kstack);
                pgdir_free_user(new_pgdir);
                EXEC_FAIL(-12);
            }
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
    current_proc->read_implies_exec = (uint8_t)new_rie;
    current_proc->exec_stack        = (uint8_t)(new_stack_exec && !new_rie);
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
    current_proc->suid       = new_euid;
    current_proc->sgid       = new_egid;
    current_proc->did_exec   = 1;
    current_proc->tgid       = current_proc->pid;  /* exec → new thread-group leader */
    current_proc->vm_owner   = NULL;               /* own address space from here */
    /* Linux begin_new_exec(): the new image inherits none of the old thread's
     * per-thread user-memory hooks — they point into an address space that no
     * longer exists, and honouring them at exit would scribble on the new one. */
    current_proc->clear_child_tid  = 0;
    current_proc->set_child_tid    = 0;
    current_proc->robust_list_head = 0;
    /* The itimers (alarm, setitimer) carry over to the new image; the POSIX
     * timers do not (Linux begin_new_exec -> exit_itimers). */
    ktimer_exec(current_proc->tgid);
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

    /* Reset caught signals to SIG_DFL (fs/exec.c flush_signal_handlers):
     * a handler address means nothing in the new image, but SIG_IGN does and
     * is kept — nohup, `cmd &` (SIGINT/SIGQUIT ignored), getty->login and
     * daemons rely on it.  sa_flags and sa_mask go with the handler.  The
     * blocked mask and the pending set are the TASK's and survive exec
     * (execve(2), signal(7)); a pending signal whose handler was just reset
     * gets its default action once unblocked.  If the table is still shared
     * with other tasks (CLONE_SIGHAND without CLONE_THREAD), unshare it first
     * as a private copy (Linux unshare_sighand) so their dispositions are
     * untouched and our SIG_IGN entries carry over. */
    if (current_proc->sighand && current_proc->sighand->refcount > 1) {
        struct sighand *fresh = sighand_copy(current_proc->sighand);
        if (fresh) {
            sighand_put(current_proc->sighand);
            current_proc->sighand = fresh;
        }
    }
    if (current_proc->sighand) {
        struct sighand *sh = current_proc->sighand;
        for (int i = 0; i < NSIGS; i++) {
            if (sh->handlers[i] != SIG_IGN) sh->handlers[i] = SIG_DFL;
            sh->flags[i] = 0;
            sh->mask[i]  = 0;
        }
    }
    current_proc->sigframe_addr  = 0;
    current_proc->restore_sigmask = 0;   /* no sigsuspend mask survives exec */
    current_proc->fault_sig      = 0;
    /* The alternate signal stack belonged to the old image (fs/exec.c
     * begin_new_exec: sas_ss_sp = sas_ss_size = 0). */
    current_proc->sas_sp         = 0;
    current_proc->sas_size       = 0;

    /* Update trapframe to run new program */
    registers_t *tf = current_proc->tf;
    tf->eip     = entry;
    tf->useresp = user_esp;

    /* The new image starts with a fresh mmap region.  Its shm attachments
     * belong to the old page directory and go with it (pgdir_free_user below,
     * or at the last co-owner's reap when it is shared). */
    current_proc->mmap_next = mmap_floor;
    fpu_state_init(fpu_area(current_proc));

    /* Switch to new page directory before freeing old one */
    __asm__ volatile("mov %0, %%cr3" :: "r"(new_pgdir) : "memory");

    /* Free old address space */
    if (!pgdir_release(old_pgdir))
        pgdir_free_user(old_pgdir);

    printk("[SYSCALL] exec '%s' pid=%d entry=0x%08x argc=%d\n",
           path, current_proc->pid, (unsigned)entry, argc);
    es_free(&av);
    es_free(&ev);
#undef KARGV
#undef KENVP
#undef EXEC_FAIL
    return 0;  /* trapret irets to entry */
}

/* Linux check_kill_permission(): may the caller signal `t`?  Always within
 * its own thread group and always as root; otherwise the sender's real or
 * effective uid has to match the target's real or saved uid, except that
 * SIGCONT may be sent anywhere in the sender's session (job control). */
static int kill_permitted(struct proc *t, int sig) {
    struct proc *me = current_proc;
    if (!me || t->tgid == me->tgid) return 1;
    if (me->euid == 0) return 1;
    if (me->euid == t->suid || me->euid == t->uid ||
        me->uid  == t->suid || me->uid  == t->uid)
        return 1;
    if (sig == SIGCONT && t->sid == me->sid) return 1;
    return 0;
}

/* Signal every process (one per thread group) that `match` selects.  Linux
 * __kill_pgrp_info()/kill_something_info(-1): success if at least one was
 * signalled; otherwise -EPERM if some were refused, -ESRCH if none matched. */
#define KILL_PGRP 0
#define KILL_ALL  1
static int kill_many(int how, int pg, int sig) {
    int matched = 0, sent = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q->state == PROC_UNUSED) continue;
        if (q->pid != q->tgid) continue;                   /* one per process */
        if (how == KILL_PGRP && q->pgrp != pg) continue;
        if (how == KILL_ALL &&
            (q->pid == 1 || q->tgid == current_proc->tgid)) continue;
        if (!q->pgdir_phys) continue;                      /* kernel thread */
        matched++;
        if (!kill_permitted(q, sig)) continue;
        /* An unreaped zombie is still a member (Linux: signalling it succeeds
         * and does nothing) — and a zombie LEADER whose other threads still run
         * is a live process: signal_send_group reaches them and does nothing
         * for a group with no live thread left. */
        if (sig) signal_send_group(q, sig);
        sent++;
    }
    if (sent) return 0;
    return matched ? -1 : -3;                              /* -EPERM : -ESRCH */
}

/* ── sys_kill(pid_t pid, int sig) — EAX=37 ──────────────────────────────── */
static int sys_kill(registers_t *regs) {
    int pid = (int)regs->ebx;
    int sig = (int)regs->ecx;

    if (sig < 0 || sig >= NSIGS) return -22;  /* -EINVAL */

    /* pid=0 -> current process group; pid=-1 -> every process the caller may
     * signal except init and itself; pid<-1 -> process group -pid.  One
     * signal per PROCESS (Linux __kill_pgrp_info -> group_send_sig_info for
     * each process in the group), delivered to a thread that does not block
     * it.  sig 0 performs the same existence and permission checks. */
    if (pid == 0)
        return kill_many(KILL_PGRP, current_proc ? current_proc->pgrp : 0, sig);
    if (pid == -1)
        return kill_many(KILL_ALL, 0, sig);
    if (pid < 0)
        return kill_many(KILL_PGRP, -pid, sig);

    /* kill(pid): pid may name any thread of a process (Linux kill_pid_info
     * uses the thread group of the task with that pid).  The signal is
     * process-directed: complete_signal() picks one thread that does not block
     * it; a fatal default disposition then ends the whole group at delivery. */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (ptable[i].pid == pid && ptable[i].state != PROC_UNUSED) {
            if (!kill_permitted(&ptable[i], sig)) return -1;   /* -EPERM */
            if (sig == 0) return 0;               /* existence check */
            if (sig == SIGKILL) {
                /* Kill the thread group AND the entire process subtree.  The
                 * subtree part is not Linux behaviour (Linux kills only the
                 * group); it is kept deliberately so a watchdog killing a
                 * misbehaving multiprocess application does not leak its
                 * forked children into the 128-slot process table.  Marking is
                 * done before any victim runs (signal_send only sets a pending
                 * bit), so parent links are still intact for the walk.  Each
                 * descendant must itself be one the caller may signal: the walk
                 * stops at a child that changed credentials (a set-uid program
                 * started by the target), exactly where kill(2) would refuse. */
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
                        if (p->parent && (p->parent->pending_sigs & (1u << SIGKILL)) &&
                            kill_permitted(p, SIGKILL)) {
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


    /* Copy the entry and bump refcount */
    fd_copy(dst, src);
    /* POSIX/Linux dup2(2): the DUPLICATE fd does NOT inherit the close-on-exec
     * flag — it is always CLEARED on newfd.  Our `*dst = *src` copies cloexec
     * from the source, which is WRONG: Chromium dup2's a MFD_CLOEXEC memfd onto
     * a target fd to hand it to the content child across exec (fds_to_remap),
     * expecting it to SURVIVE exec.  Copying cloexec=1 made execve close it →
     * the jsInit/prefMap shared-memory fd vanished → child mmap'd a stale/reused
     * fd → EBADF → ImageBridgeChild::InitSameProcess NULL-deref crash.  Clear it. */
    dst->cloexec = 0;

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
    int lerr;
    vfs_node_t *n = vfs_lookup_at(path, 1, &lerr);
    if (!n) return lerr;
    if (!(n->flags & VFS_FLAG_DIR)) return -20;  /* ENOTDIR */
    char canonical[256];
    int cr = canonicalize_path_at_cwd(path, canonical, sizeof(canonical));
    if (cr < 0) return cr;
    __builtin_memcpy(current_proc->cwd, canonical, sizeof(canonical));
    return 0;
}

/* ── sys_chroot(path) — EAX=61 ───────────────────────────────────────────── */
/* The directory `path` names now (resolved inside the current root, symlinks
 * followed) becomes this process's root node, pinned with vfs_retain like an
 * open descriptor's node: later renames, or a symlink put where the path
 * was, do not move it.  /dev and /proc stay the global ones inside it (there
 * is no bind mount to put them there).  The cwd, a string relative to the
 * root, is reset to "/": Linux leaves the cwd outside the new root, but here
 * the old string would name a different directory inside it.  For the same
 * reason a descriptor opened before chroot() is resolved by its path string
 * inside the new root, and /proc/self/fd/N gives a chrooted process no
 * directory to walk through (fs/procfs.c), so neither reaches the old tree
 * (both are the classic escape on Linux). */
static int sys_chroot(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0)
        return -14;
    if (current_proc->euid != 0) return -1;            /* -EPERM */
    int lerr;
    vfs_node_t *n = vfs_lookup_at(path, 1, &lerr);
    if (!n) return lerr;
    if (!(n->flags & VFS_FLAG_DIR)) return -20;        /* -ENOTDIR */
    if (n == current_proc->root_node) return 0;        /* chroot("/"), "." at / */
    vfs_node_t *old = current_proc->root_node;
    if (n == vfs_root && !old) return 0;               /* the global root */
    vfs_retain(n);
    current_proc->root_node = n;
    if (old) vfs_close(old);
    current_proc->cwd[0] = '/';
    current_proc->cwd[1] = '\0';
    return 0;
}

/* ── sys_fchdir(fd) — EAX=133 ───────────────────────────────────────────── */
/* An FD_FILE remembers the canonical path it was opened by, which is what
 * the cwd holds. */
static int sys_fchdir(registers_t *regs) {
    int fd = (int)regs->ebx;
    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type != FD_FILE || !f->node || !f->path[0]) return -9;
    if (!(f->node->flags & VFS_FLAG_DIR)) return -20;  /* ENOTDIR */
    __builtin_memcpy(current_proc->cwd, f->path, sizeof(f->path));
    return 0;
}

/* ── sys_lseek(fd, offset, whence) — EAX=19 ─────────────────────────────── */
/* The offset lseek/_llseek would move `f` to, computed in 64 bits: Linux
 * vfs_setpos() refuses a negative result or one past the largest file (here
 * 4 GiB - 1) with -EINVAL, leaving the position unchanged. */
static int seek_target(proc_file_t *f, int64_t off, int whence, uint32_t *out) {
    int64_t base;
    switch (whence) {
    case 0: base = 0; break;                                   /* SEEK_SET */
    case 1: base = (int64_t)f->offset; break;                  /* SEEK_CUR */
    case 2: base = f->node ? (int64_t)f->node->size : 0; break; /* SEEK_END */
    default: return -22;
    }
    int64_t pos = base + off;
    if (pos < 0 || pos > 0xFFFFFFFFLL) return -22;             /* -EINVAL */
    *out = (uint32_t)pos;
    return 0;
}

static int sys_lseek(registers_t *regs) {
    int fd     = (int)regs->ebx;
    int off    = (int)regs->ecx;   /* signed */
    int whence = (int)regs->edx;

    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type == FD_NONE) return -9;
    if (f->type == FD_PIPE_R || f->type == FD_PIPE_W) return -29;  /* ESPIPE */

    uint32_t new_off;
    int r = seek_target(f, off, whence, &new_off);
    if (r < 0) return r;
    /* A 32-bit off_t cannot carry the result (Linux: -EOVERFLOW). */
    if (new_off > 0x7FFFFFFFU) return -75;
    f->offset = new_off;
    return (int)new_off;
}

/* ── credential syscalls ─────────────────────────────────────────────────── */
static int sys_getuid(registers_t *regs)  { (void)regs; return (int)current_proc->uid; }
static int sys_getgid(registers_t *regs)  { (void)regs; return (int)current_proc->gid; }
static int sys_geteuid(registers_t *regs) { (void)regs; return (int)current_proc->euid; }
static int sys_getegid(registers_t *regs) { (void)regs; return (int)current_proc->egid; }

/* Linux kernel/sys.c credential rules.  "Privileged" is euid 0 (CAP_SETUID /
 * CAP_SETGID); an unprivileged caller may only move each id among its current
 * real, effective and saved values.  An id of -1 (0xFFFFFFFF) means "leave
 * unchanged" where the call allows it.  The 16-bit legacy entry points widen
 * their 0xFFFF to -1 first (low2highuid). */
#define ID_KEEP 0xFFFFFFFFU

static uint32_t id16(uint32_t v) {
    v &= 0xFFFFU;
    return v == 0xFFFFU ? ID_KEEP : v;
}

static int uid_is_ours(uint32_t u) {
    struct proc *p = current_proc;
    return u == p->uid || u == p->euid || u == p->suid;
}

static int gid_is_ours(uint32_t g) {
    struct proc *p = current_proc;
    return g == p->gid || g == p->egid || g == p->sgid;
}

/* setuid(uid): privileged sets real, effective and saved; otherwise only the
 * effective id may change, and only to the real or saved uid. */
static int do_setuid(uint32_t u) {
    struct proc *p = current_proc;
    if (u == ID_KEEP) return -22;                  /* -EINVAL */
    if (p->euid == 0) {
        p->uid = p->euid = p->suid = u;
        return 0;
    }
    if (u == p->uid || u == p->suid) { p->euid = u; return 0; }
    return -1;                                      /* -EPERM */
}

static int do_setgid(uint32_t g) {
    struct proc *p = current_proc;
    if (g == ID_KEEP) return -22;
    if (p->euid == 0) {
        p->gid = p->egid = p->sgid = g;
        return 0;
    }
    if (g == p->gid || g == p->sgid) { p->egid = g; return 0; }
    return -1;
}

/* setreuid(ruid, euid): an unprivileged ruid must be the current real or
 * effective uid; euid any of real/effective/saved.  Setting the real id, or
 * the effective id to something other than the old real id, also saves the
 * new effective id (so the caller cannot switch back through it). */
static int do_setreuid(uint32_t r, uint32_t e) {
    struct proc *p = current_proc;
    int priv = (p->euid == 0);
    if (r != ID_KEEP && !priv && r != p->uid && r != p->euid) return -1;
    if (e != ID_KEEP && !priv && !uid_is_ours(e)) return -1;
    uint32_t old_ruid = p->uid;
    if (r != ID_KEEP) p->uid = r;
    if (e != ID_KEEP) p->euid = e;
    if (r != ID_KEEP || (e != ID_KEEP && e != old_ruid)) p->suid = p->euid;
    return 0;
}

static int do_setregid(uint32_t r, uint32_t e) {
    struct proc *p = current_proc;
    int priv = (p->euid == 0);
    if (r != ID_KEEP && !priv && r != p->gid && r != p->egid) return -1;
    if (e != ID_KEEP && !priv && !gid_is_ours(e)) return -1;
    uint32_t old_rgid = p->gid;
    if (r != ID_KEEP) p->gid = r;
    if (e != ID_KEEP) p->egid = e;
    if (r != ID_KEEP || (e != ID_KEEP && e != old_rgid)) p->sgid = p->egid;
    return 0;
}

/* setresuid(ruid, euid, suid): unprivileged, each new id must already be one
 * of the caller's real/effective/saved ids.  Checked in full before anything
 * changes, so a refused call leaves every id as it was. */
static int do_setresuid(uint32_t r, uint32_t e, uint32_t sv) {
    struct proc *p = current_proc;
    if (p->euid != 0) {
        if (r  != ID_KEEP && !uid_is_ours(r))  return -1;
        if (e  != ID_KEEP && !uid_is_ours(e))  return -1;
        if (sv != ID_KEEP && !uid_is_ours(sv)) return -1;
    }
    if (r  != ID_KEEP) p->uid  = r;
    if (e  != ID_KEEP) p->euid = e;
    if (sv != ID_KEEP) p->suid = sv;
    return 0;
}

static int do_setresgid(uint32_t r, uint32_t e, uint32_t sv) {
    struct proc *p = current_proc;
    if (p->euid != 0) {
        if (r  != ID_KEEP && !gid_is_ours(r))  return -1;
        if (e  != ID_KEEP && !gid_is_ours(e))  return -1;
        if (sv != ID_KEEP && !gid_is_ours(sv)) return -1;
    }
    if (r  != ID_KEEP) p->gid  = r;
    if (e  != ID_KEEP) p->egid = e;
    if (sv != ID_KEEP) p->sgid = sv;
    return 0;
}

static int sys_setuid(registers_t *regs)   { return do_setuid((uint32_t)regs->ebx); }
static int sys_setgid(registers_t *regs)   { return do_setgid((uint32_t)regs->ebx); }
static int sys_setuid16(registers_t *regs) { return do_setuid(id16(regs->ebx)); }
static int sys_setgid16(registers_t *regs) { return do_setgid(id16(regs->ebx)); }
static int sys_setreuid(registers_t *regs) {
    return do_setreuid((uint32_t)regs->ebx, (uint32_t)regs->ecx);
}
static int sys_setregid(registers_t *regs) {
    return do_setregid((uint32_t)regs->ebx, (uint32_t)regs->ecx);
}
static int sys_setreuid16(registers_t *regs) {
    return do_setreuid(id16(regs->ebx), id16(regs->ecx));
}
static int sys_setregid16(registers_t *regs) {
    return do_setregid(id16(regs->ebx), id16(regs->ecx));
}
static int sys_setresuid(registers_t *regs) {
    return do_setresuid((uint32_t)regs->ebx, (uint32_t)regs->ecx,
                        (uint32_t)regs->edx);
}
static int sys_setresgid(registers_t *regs) {
    return do_setresgid((uint32_t)regs->ebx, (uint32_t)regs->ecx,
                        (uint32_t)regs->edx);
}
static int sys_setresuid16(registers_t *regs) {
    return do_setresuid(id16(regs->ebx), id16(regs->ecx), id16(regs->edx));
}
static int sys_setresgid16(registers_t *regs) {
    return do_setresgid(id16(regs->ebx), id16(regs->ecx), id16(regs->edx));
}

/* getgroups(size, list) / setgroups(size, list) (Linux kernel/groups.c).
 * getgroups with size 0 only counts; a smaller non-zero size is -EINVAL.
 * setgroups needs CAP_SETGID — here euid 0 — and is -EPERM otherwise, before
 * the size is looked at.  The 16-bit legacy calls (80/81) move 16-bit gids:
 * 0xFFFF widens to -1, and a gid above 0xFFFF reads back as the overflow gid
 * 65534. */
static int do_getgroups(int size, void *ulist, int wide) {
    struct proc *p = current_proc;
    if (size < 0) return -22;                               /* -EINVAL */
    if (size == 0) return (int)p->ngroups;
    if ((uint32_t)size < p->ngroups) return -22;
    for (uint32_t i = 0; i < p->ngroups; i++) {
        int cr;
        if (wide) {
            cr = copy_to_user((uint32_t *)ulist + i, &p->groups[i], 4);
        } else {
            uint16_t g = p->groups[i] > 0xFFFFU ? 65534 : (uint16_t)p->groups[i];
            cr = copy_to_user((uint16_t *)ulist + i, &g, 2);
        }
        if (cr < 0) return cr;
    }
    return (int)p->ngroups;
}

static int do_setgroups(int size, const void *ulist, int wide) {
    struct proc *p = current_proc;
    uint32_t list[PROC_NGROUPS_MAX];
    if (p->euid != 0) return -1;                            /* -EPERM */
    if (size < 0 || size > PROC_NGROUPS_MAX) return -22;    /* -EINVAL */
    for (int i = 0; i < size; i++) {
        int cr;
        if (wide) {
            cr = copy_from_user(&list[i], (const uint32_t *)ulist + i, 4);
        } else {
            uint16_t g;
            cr = copy_from_user(&g, (const uint16_t *)ulist + i, 2);
            list[i] = id16(g);
        }
        if (cr < 0) return cr;
    }
    __builtin_memcpy(p->groups, list, (uint32_t)size * sizeof(uint32_t));
    p->ngroups = (uint32_t)size;
    return 0;
}

static int sys_getgroups(registers_t *regs) {
    return do_getgroups((int)regs->ebx, (void *)(uintptr_t)regs->ecx, 1);
}
static int sys_setgroups(registers_t *regs) {
    return do_setgroups((int)regs->ebx, (const void *)(uintptr_t)regs->ecx, 1);
}
static int sys_getgroups16(registers_t *regs) {
    return do_getgroups((int)regs->ebx, (void *)(uintptr_t)regs->ecx, 0);
}
static int sys_setgroups16(registers_t *regs) {
    return do_setgroups((int)regs->ebx, (const void *)(uintptr_t)regs->ecx, 0);
}

/* setfsuid/setfsgid: there is no separate filesystem id here (fsuid always
 * follows euid, as it does on Linux unless these are called), so report the
 * current one and change nothing — what Linux returns for a refused change. */
static int sys_setfsuid(registers_t *regs) { (void)regs; return (int)current_proc->euid; }
static int sys_setfsgid(registers_t *regs) { (void)regs; return (int)current_proc->egid; }

/* ── sys_mkdir(path, mode) — EAX=39 ─────────────────────────────────────── */
static int sys_mkdir(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0)
        return -14;

    char resolved[256];
    int r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    return sys_mkdir_kernel_path(resolved, regs->ecx);
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

/* How a device ioctl's third argument is passed: IOA_VAL is a plain value the
 * driver never dereferences; IOA_IN/IOA_OUT/IOA_INOUT point at `len` bytes the
 * driver reads, writes, or both. */
enum { IOA_VAL, IOA_IN, IOA_OUT, IOA_INOUT };

/* Every request a device ioctl_fn (tty, pty, console VT, fbdev) understands,
 * with the shape of its argument.  Anything not listed is ENOTTY without the
 * driver ever being called, so a driver can never be handed a user pointer
 * this table did not account for. */
static int ioctl_arg_shape(uint32_t req, uint32_t *len) {
    switch (req) {
    case FBIOGET_FSCREENINFO: *len = sizeof(fb_fix_screeninfo_t); return IOA_OUT;
    case FBIOGET_VSCREENINFO: *len = sizeof(fb_var_screeninfo_t); return IOA_OUT;
    case 0x4601:              *len = sizeof(fb_var_screeninfo_t); return IOA_IN;  /* FBIOPUT_VSCREENINFO */
    case 0x80045430U:         *len = sizeof(int);   return IOA_OUT;   /* TIOCGPTN */
    case 0x40045431U:         *len = sizeof(int);   return IOA_IN;    /* TIOCSPTLCK */
    case 0x5401:              *len = 36;            return IOA_OUT;   /* TCGETS: i386 struct termios */
    case 0x5402: case 0x5403: case 0x5404:
                              *len = 36;            return IOA_IN;    /* TCSETS/W/F */
    case 0x5413:              *len = 8;             return IOA_OUT;   /* TIOCGWINSZ */
    case 0x5414:              *len = 8;             return IOA_IN;    /* TIOCSWINSZ */
    case 0x540F:              *len = sizeof(int);   return IOA_OUT;   /* TIOCGPGRP */
    case 0x80044D00U:         *len = sizeof(int);   return IOA_OUT;   /* SOUND_MIXER_READ_VOLUME */
    case 0xC0044D00U:         *len = sizeof(int);   return IOA_INOUT; /* SOUND_MIXER_WRITE_VOLUME */
    case 0x5410:              *len = sizeof(int);   return IOA_IN;    /* TIOCSPGRP */
    case 0x5601:              *len = 8;             return IOA_OUT;   /* VT_GETMODE: struct vt_mode */
    case 0x5602:              *len = 8;             return IOA_IN;    /* VT_SETMODE */
    case 0x5603:              *len = 6;             return IOA_OUT;   /* VT_GETSTATE: struct vt_stat */
    case 0x4B3B: case 0x4B44: *len = sizeof(int);   return IOA_OUT;   /* KDGETMODE, KDGKBMODE */
    case 0x540E:                                      /* TIOCSCTTY (int steal flag) */
    case 0x5605: case 0x5606: case 0x5607:            /* VT_RELDISP/ACTIVATE/WAITACTIVE */
    case 0x4B3A: case 0x4B45: case 0x4B32: case 0x4B46: /* KDSETMODE/SKBMODE/SETLED/GKBMETA */
                              *len = 0;             return IOA_VAL;
    }
    return -1;
}

/* ── sys_ioctl(fd, request, arg) — EAX=54 ───────────────────────────────── */
static int sys_ioctl(registers_t *regs) {
    int fd  = (int)regs->ebx;
    int req = (int)regs->ecx;

    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    /* Interface and routing ioctls (SIOCGIFCONF, SIOCGIFADDR, SIOCADDRT...)
     * work on any socket, as on Linux (net/netlink.c). */
    if ((f->type == FD_SOCKET || f->type == FD_USOCKET) &&
        ((uint32_t)req & 0xFF00) == 0x8900) {
        int r = netdev_ioctl((uint32_t)req, (void *)(uintptr_t)regs->edx);
        if (r != 1) return r;
    }
    if (f->type == FD_FILE && f->node && f->node->ioctl_fn) {
        /* Drivers read and write their argument with plain loads and stores,
         * which fault (and panic, not being in __ex_table) on a read-only or
         * unmapped user page.  So a pointer argument never reaches them: it is
         * bounced through a kernel buffer, copied in before the call and back
         * out after it, and a bad user pointer is -EFAULT here instead. */
        void    *uarg = (void *)(uintptr_t)regs->edx;
        uint32_t len  = 0;
        int      dir  = ioctl_arg_shape((uint32_t)req, &len);
        if (dir < 0) return -25;                          /* -ENOTTY */
        if (dir == IOA_VAL)
            return f->node->ioctl_fn(f->node, (uint32_t)req, uarg);
        if (!uarg) return -14;
        uint8_t kbuf[sizeof(fb_var_screeninfo_t) > 64 ? sizeof(fb_var_screeninfo_t) : 64];
        _Static_assert(sizeof(fb_fix_screeninfo_t) <= sizeof(kbuf),
                       "ioctl bounce buffer too small for fb_fix_screeninfo");
        __builtin_memset(kbuf, 0, sizeof(kbuf));
        if (dir != IOA_OUT && copy_from_user(kbuf, uarg, len) < 0) return -14;
        int rc = f->node->ioctl_fn(f->node, (uint32_t)req, kbuf);
        if (rc >= 0 && dir != IOA_IN && copy_to_user(uarg, kbuf, len) < 0)
            return -14;
        return rc;
    }

    /* TCGETS = 0x5401, TCSETS = 0x5402, TIOCGWINSZ = 0x5413.  Only the
     * implicit serial console behind an unopened stdio fd is a terminal here;
     * on a pipe, socket or plain file these fail with -ENOTTY.  musl's
     * isatty() is TIOCGWINSZ, so answering them for every fd made every pipe
     * a tty (less refused piped input, git started a pager into a pipe). */
    if ((req == 0x5413 || req == 0x5401 || req == 0x5402 || req == 0x5403 ||
         req == 0x5404) && (f->type != FD_NONE || fd > 2))
        return -25;                                           /* -ENOTTY */
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
    if (req == 0x5429) {  /* TIOCGSID: return the session ID (use pgrp of shell) */
        int *out = (int *)(uintptr_t)regs->edx;
        int sid = current_proc ? current_proc->pgrp : 1;
        if (out) {
            int cr = copy_to_user(out, &sid, sizeof(sid));
            if (cr < 0) return cr;
        }
        return sid;
    }
    if (req == 0x540F) {  /* TIOCGPGRP: get foreground pgrp */
        int *out = (int *)(uintptr_t)regs->edx;
        int fg = tty_fg_pgrp ? tty_fg_pgrp : (current_proc ? current_proc->pgrp : 1);
        if (out) {
            int cr = copy_to_user(out, &fg, sizeof(fg));
            if (cr < 0) return cr;
        }
        return fg;
    }
    if (req == 0x540E) {  /* TIOCSCTTY on the implicit serial console */
        if (f->type != FD_NONE || fd > 2) return -25;
        return tty_console_setctty();
    }
    if (req == 0x5410) {  /* TIOCSPGRP: set foreground pgrp */
        /* Only the implicit serial console behind an unopened stdio fd is a
         * terminal here; a pipe, socket or plain file is not (-ENOTTY). */
        if (f->type != FD_NONE || fd > 2) return -25;
        int fg = 0;
        int cr = copy_from_user(&fg, (const void *)(uintptr_t)regs->edx, sizeof(fg));
        if (cr < 0) return cr;
        return tty_console_setpgrp(fg);
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
                fd_copy(&current_proc->ofile[i], f);
                /* F_DUPFD clears cloexec; F_DUPFD_CLOEXEC sets it */
                current_proc->ofile[i].cloexec = (cmd == 1030) ? 1 : 0;
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
        f->flags = (f->flags & O_ACCMODE) | (arg & (O_APPEND | O_NONBLOCK));
        return 0;
    case 5:  case 6:  case 7:   /* F_GETLK / F_SETLK / F_SETLKW */
    case 12: case 13: case 14:  /* their flock64 forms (musl, glibc LFS) */
    case 36: case 37: case 38:  /* F_OFD_GETLK / F_OFD_SETLK / F_OFD_SETLKW */
        /* Real advisory record locks (proc/flock.c).  F_GETLK answers
         * F_UNLCK when nothing conflicts, which is what Firefox's
         * nsProfileLock probes for. */
        return flock_fcntl(f, cmd, (void *)(uintptr_t)arg);
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

    int lerr;
    vfs_node_t *n = vfs_lookup_at(path, 1, &lerr);
    if (!n) return lerr;
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
    if (r == -4) {
        if (rem) {
            struct ktimespec krem = { (int32_t)rs, (int32_t)rn };
            cr = copy_to_user(rem, &krem, sizeof(krem));
            if (cr < 0) return cr;
        }
        /* -ERESTARTNOHAND, not -EINTR: the return-to-user path re-issues a -4
         * with the ORIGINAL arguments when the handler has SA_RESTART, which
         * for a sleep means sleeping the whole duration again and discarding
         * the remainder just written.  Linux never restarts these calls once a
         * handler has run (hrtimer_nanosleep returns ERESTART_RESTARTBLOCK,
         * and a restart resumes the REMAINING time, never the original). */
        return -ERESTARTNOHAND;
    }
    return r;
}

/*
 * getdents/getdents64 — records in the exact Linux i386 layouts (musl's
 * readdir walks them by d_reclen):
 *   linux_dirent:   ino(4) off(4) reclen(2) name NUL [pad] type, where the
 *                   type is the LAST byte: reclen = ALIGN(10 + namlen + 2, 4)
 *   linux_dirent64: ino(8) off(8) reclen(2) type(1) name NUL [pad]:
 *                   reclen = ALIGN(19 + namlen + 1, 8)
 * d_off is the directory position after the entry (what lseek on the
 * directory takes back, so telldir/seekdir work).  The position only advances
 * past entries actually copied out: an entry that does not fit is returned by
 * the next call, and if not even the first one fits the call is -EINVAL.
 */
static int getdents_common(registers_t *regs, int is64) {
    int       fd    = (int)regs->ebx;
    char     *buf   = (char *)(uintptr_t)regs->ecx;
    uint32_t  count = (uint32_t)regs->edx;

    if (!access_ok(buf, count)) return -14;
    if (fd < 0 || fd >= MAX_FD) return -9;

    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type != FD_FILE || !f->node) return -9;
    if (!(f->node->flags & VFS_FLAG_DIR)) return -20;

    uint32_t written = 0;
    vfs_dirent_t de;
    uint8_t rec[512];

    while (vfs_readdir(f->node, f->offset, &de) >= 0) {
        uint32_t nlen = 0;
        while (nlen < sizeof(de.name) - 1 && de.name[nlen]) nlen++;
        uint32_t reclen = is64 ? (19 + nlen + 1 + 7) & ~7U
                               : (10 + nlen + 2 + 3) & ~3U;
        if (reclen > sizeof(rec)) return written ? (int)written : -22;
        if (written + reclen > count) {
            if (!written) return -22;            /* -EINVAL: buffer too small */
            break;
        }
        __builtin_memset(rec, 0, reclen);
        if (is64) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)rec;
            d->d_ino    = de.ino;
            d->d_off    = (int64_t)(f->offset + 1);
            d->d_reclen = (uint16_t)reclen;
            d->d_type   = vfs_type_to_dt(de.type);
            __builtin_memcpy(d->d_name, de.name, nlen);   /* NUL: memset */
        } else {
            struct linux_dirent *d = (struct linux_dirent *)rec;
            d->d_ino    = de.ino;
            d->d_off    = f->offset + 1;
            d->d_reclen = (uint16_t)reclen;
            __builtin_memcpy(d->d_name, de.name, nlen);   /* NUL: memset */
            rec[reclen - 1] = vfs_type_to_dt(de.type);
        }
        int cr = copy_to_user(buf + written, rec, reclen);
        if (cr < 0) return written ? (int)written : cr;
        written += reclen;
        f->offset++;
    }
    return (int)written;
}

/* ── sys_getdents(fd, buf, count) — EAX=141 ─────────────────────────────── */
static int sys_getdents(registers_t *regs) {
    return getdents_common(regs, 0);
}

/* ── sys_getdents64(fd, buf, count) — EAX=220 ───────────────────────────── */
static int sys_getdents64(registers_t *regs) {
    return getdents_common(regs, 1);
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
/* A MAP_SHARED file mapping whose descriptor was not opened O_RDWR (Linux
 * !VM_MAYWRITE): it may never become writable, not even by mprotect later. */
#define VMA_F_NOWRITE  0x2U
/* A shm_map attachment (proc/shm.c).  Kept apart from ordinary shared anon
 * VMAs so the two never merge and shm_unmap only ever drops its own. */
#define VMA_F_SHM      0x4U

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
#define MMAP_TOP     SIGPAGE_VA      /* stack window minus the sigreturn page */

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
static inline int pte_mapped(pte_t pte) {
    return (pte & (PAGE_PRESENT | PAGE_PROTNONE)) != 0;
}

/* PTE flag bits for a freshly populated page of a mapping with `prot`
 * (already widened by proc_prot_adjust where the image asks for it). */
static pte_t pte_flags_for(uint32_t prot, uint32_t shared) {
    pte_t f = PAGE_USER | (shared ? PAGE_SHARED : 0);
    if (prot == 0) return f | PAGE_PROTNONE | PAGE_NX; /* inaccessible, frame kept */
    f |= PAGE_PRESENT | page_nx_unless(prot & PROT_EXEC_K);
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
static uint32_t first_mapped_page_inner(uint32_t start, uint32_t end);
static uint32_t first_mapped_page(uint32_t start, uint32_t end) {
    uint64_t kp = kprof_probe_begin();
    uint32_t r = first_mapped_page_inner(start, end);
    kprof_probe_end(KPP_FIRST_MAPPED, kp);
    return r;
}
static uint32_t first_mapped_page_inner(uint32_t start, uint32_t end) {
    for (uint32_t a = start; a < end; ) {
        if (!pde_present(a)) {
            uint32_t n = pt_next(a);   /* next page table */
            if (n <= a) break;                            /* wrapped */
            a = n;
            continue;
        }
        if (pte_mapped(pte_get(a))) return a ? a : 1;
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
static uint32_t vma_gap_find_inner(uint32_t length, uint32_t align);
static uint32_t vma_gap_find(uint32_t length, uint32_t align) {
    uint64_t kp = kprof_probe_begin();
    uint32_t r = vma_gap_find_inner(length, align);
    kprof_probe_end(KPP_GAP_FIND, kp);
    return r;
}
static uint32_t vma_gap_find_inner(uint32_t length, uint32_t align) {
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
    int         may_write; /* some mapping could store into the frames */
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

static int shmap_unmapped(struct shmap_entry *e) {
    for (uint32_t p = 0; p < e->npages; p++)
        if (e->frames && e->frames[p] && pmm_frame_refcount(e->frames[p]) > 1)
            return 0;   /* still mapped by a live process */
    return 1;
}

static void shmap_release(struct shmap_entry *e) {
    for (uint32_t p = 0; p < e->npages; p++)
        if (e->frames && e->frames[p]) pmm_frame_decref(e->frames[p]);
    if (e->frames) kfree(e->frames);
    e->frames = NULL; e->npages = 0;
    vfs_close(e->node);                 /* drop the registry's reference */
    e->node = NULL;
    e->may_write = 0;
}

static int shmap_try_reclaim(struct shmap_entry *e) {
    if (shmap_node_has_open_fd(e->node)) return 0;   /* live fd → data must persist */
    if (!shmap_unmapped(e)) return 0;                /* still mapped — keep it */
    shmap_release(e);
    return 1;
}

/* For an ordinary file (ext2, tmpfs) the entry is only a copy of the file taken
 * when it was first mapped: write() and truncate() never update it.  Once no
 * mapping uses it, it is stale, and serving it would give every later mapping
 * (shared or private) the old bytes while read() returns the new ones; ext2
 * hands every open of an inode the same node, so the stale key always matches.
 * Drop it instead, unless some mapping could have stored into it: nothing
 * writes those frames back, so they are then the only copy of the stores.
 * memfd (shmem) entries ARE the file and are never stale. */
static int node_is_shmem(vfs_node_t *n);
static struct shmap_entry *shmap_lookup_fresh(vfs_node_t *node) {
    struct shmap_entry *e = shmap_lookup(node);
    if (e && !node_is_shmem(node) && !e->may_write && shmap_unmapped(e)) {
        shmap_release(e);
        return NULL;
    }
    return e;
}

/* The registry keys on the node pointer and outlives every mapping of it, so it
 * must hold a reference of its own: otherwise the node could be freed, its
 * address reused by a different file, and the stale key would match the wrong
 * one.  shmap_try_reclaim() drops the reference when it retires an entry. */
static struct shmap_entry *shmap_get(vfs_node_t *node) {
    struct shmap_entry *free_e = NULL;
    struct shmap_entry *cur = shmap_lookup_fresh(node);
    if (cur) return cur;
    for (int i = 0; i < SHMAP_MAX; i++) {
        if (!shmaps[i].node && !free_e) free_e = &shmaps[i];
    }
    if (!free_e) {                       /* table full — reclaim dead entries */
        for (int i = 0; i < SHMAP_MAX; i++)
            if (shmaps[i].node && shmap_try_reclaim(&shmaps[i])) { free_e = &shmaps[i]; break; }
        if (!free_e) return NULL;
    }
    vfs_retain(node);                   /* the registry's own reference */
    free_e->node   = node;
    free_e->frames = NULL;
    free_e->npages = 0;
    free_e->may_write = 0;
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
    /* off + len must not wrap past 4 GiB onto page 0. */
    if (off == 0xFFFFFFFFU) return VFS_WRITE_EFBIG;
    if (len > 0xFFFFFFFFU - off) len = 0xFFFFFFFFU - off;
    struct shmap_entry *e = shmap_get(node);
    if (!e) return VFS_WRITE_ENOMEM;
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
    if (done == 0 && len) return VFS_WRITE_ENOMEM;  /* 0 would spin write loops */
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
    if (pde_present(addr) &&
        pte_mapped(pte_get(addr))) return 1;     /* already there */
    phys_t phys = pmm_alloc_user_frame();        /* private: may be high */
    if (!phys) return 0;
    pmm_frame_incref(phys);

    kprof_count(v->file ? KPE_PF_FILE : KPE_PF_ANON);
    /* Before the preempt-off fill: retiring a stale entry can drop a file
     * reference, and the last one of an unlinked ext2 file does disk I/O. */
    struct shmap_entry *se = v->file ? shmap_lookup_fresh(v->file) : NULL;
    preempt_disable();
    uint64_t kp_z = kprof_probe_begin();
    uint8_t *kva = (uint8_t *)paging_temp_map(phys);
    __builtin_memset(kva, 0, PAGE_SIZE);
    kprof_probe_end(KPP_FAULT_ZERO, kp_z);
    if (v->file) {
        uint32_t foff = v->file_off + (addr - v->start);
        uint32_t sf = se ? shmap_peek(se, foff / PAGE_SIZE) : 0;
        if (sf) {
            /* copy from the shared frame (temp slot 2), not the stale tmpfs buffer */
            const uint8_t *src = (const uint8_t *)paging_temp_map2(sf);
            __builtin_memcpy(kva, src, PAGE_SIZE);
            paging_temp_unmap2();
        } else if (foff < v->file_size) {
            uint32_t want = PAGE_SIZE;
            if (want > v->file_size - foff) want = v->file_size - foff;
            uint64_t kp_r = kprof_probe_begin();
            vfs_read(v->file, foff, want, kva);
            kprof_probe_end(KPP_FAULT_READ, kp_r);
        }
    }
    paging_temp_unmap();
    preempt_enable();

    if (paging_map(addr, phys, pte_flags_for(v->prot, 0)) != 0) {
        pmm_frame_decref(phys);        /* drop the ref taken above */
        return 0;                      /* caller turns this into SIGSEGV */
    }
    return 1;                          /* publish: complete */
}

/* Page-fault populate.  Returns 1 if handled, 0 if the address isn't a
 * populatable VMA page (PROT_NONE, shared mapping, no VMA, OOM → SIGSEGV). */
int vma_handle_fault(uint32_t addr) {
    addr &= ~0xFFFU;
    struct vma *v = vma_find(addr);
    if (!v) return 0;
    if (v->prot == 0) return 0;                 /* PROT_NONE guard → SIGSEGV */
    if (v->flags & VMA_F_SHARED) return 0;      /* shared frames are eager-only */
    /* Is this file fault the page immediately after the previous file fault of
     * the same thread?  That is the question fault-around turns on: a purely
     * sequential walk is worth reading ahead for, a scattered one is not. */
    if (v->file && current_proc) {
        if (addr == current_proc->last_file_fault + PAGE_SIZE)
            kprof_count(KPE_PF_FILE_SEQ);
        current_proc->last_file_fault = addr;
    }
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
    phys_t batch[256];
    int nb = 0;
    for (uint32_t va = start; va < end; ) {
        if (!pde_present(va)) {
            uint32_t n = pt_next(va);
            if (n <= va) break;
            va = n;
            continue;
        }
        pte_t pte = pte_get(va);
        if (pte_mapped(pte)) {
            batch[nb++] = pte_frame(pte);       /* remember frame; free AFTER flush */
            pte_set(va, 0);
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

/* ── shm attachments (proc/shm.c keeps the bookkeeping) ─────────────────────
 * An attachment is a shared anonymous VMA flagged VMA_F_SHM whose PTEs map the
 * object's frames, one reference per PTE.  The range comes from the owner's
 * free-range search, so it is right for every thread of the process and never
 * lands on a live mapping. */
uint32_t mm_shm_attach(const uint32_t *frames, uint32_t npages) {
    uint32_t len = npages * PAGE_SIZE;
    uint32_t base = vma_gap_find(len, PAGE_SIZE);
    if (!base) return 0;
    /* Reserve the page tables before taking any reference, so the attach
     * either happens whole or leaves the address space as it was (Linux
     * shmat() returns ENOMEM here too). */
    if (paging_reserve_range(base, base + len, 1) != 0) return 0;
    if (!vma_add(base, base + len, PROT_READ_K | PROT_WRITE_K,
                 VMA_F_SHARED | VMA_F_SHM, NULL, 0))
        return 0;
    for (uint32_t i = 0; i < npages; i++) {
        pmm_frame_incref(frames[i]);        /* this PTE's reference */
        if (paging_map(base + i * PAGE_SIZE, frames[i],
                       PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_SHARED |
                       page_nx_unless(current_proc->read_implies_exec)) != 0)
            panic("shmat: reserved page table vanished", NULL);
    }
    return base;
}

/* Is the page at va still mapping `frame` (present or PROT_NONE'd)? */
static int pte_maps_frame(uint32_t va, uint32_t frame) {
    if (!pde_present(va)) return 0;
    pte_t pte = pte_get(va);
    return pte_mapped(pte) && pte_frame(pte) == frame;
}

uint32_t mm_shm_mapped(uint32_t base, const uint32_t *frames, uint32_t npages) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < npages; i++)
        if (pte_maps_frame(base + i * PAGE_SIZE, frames[i])) n++;
    return n;
}

/* Only the pages that still map the object are the attachment's: a munmap or
 * a MAP_FIXED overlay already released the others (and whatever lives there
 * now is not ours to touch).  Flush before free, as unmap_pages does. */
void mm_shm_detach(uint32_t base, const uint32_t *frames, uint32_t npages) {
    uint32_t cleared[(SHM_MAX_PAGES + 31) / 32];
    uint32_t run = 0, run_len = 0;
    if (npages > SHM_MAX_PAGES) npages = SHM_MAX_PAGES;
    __builtin_memset(cleared, 0, sizeof(cleared));
    for (uint32_t i = 0; i < npages; i++) {
        uint32_t va = base + i * PAGE_SIZE;
        if (pte_maps_frame(va, frames[i])) {
            pte_set(va, 0);
            tlb_flush_single(va);
            cleared[i / 32] |= 1U << (i % 32);
            if (!run_len) run = va;
            run_len += PAGE_SIZE;
            continue;
        }
        if (run_len) vma_remove_range(run, run + run_len);
        run_len = 0;
    }
    if (run_len) vma_remove_range(run, run + run_len);
    tlb_shootdown();                    /* no CPU keeps a stale entry now */
    for (uint32_t i = 0; i < npages; i++)
        if (cleared[i / 32] & (1U << (i % 32)))
            pmm_frame_decref(frames[i]);
}

/* ── sys_mmap2(addr,len,prot,flags,fd,pgoffset) — EAX=192 ──────────────── */
static int sys_mmap2(registers_t *regs) {
    uint32_t addr   = regs->ebx;
    uint32_t length = regs->ecx;
    uint32_t prot   = proc_prot_adjust(current_proc, regs->edx & 0x7);
                                                 /* PROT_SEM/GROWS* ignored */
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
        /* Linux do_mmap(): every file mapping needs a descriptor open for
         * reading; a shared writable one needs it open read-write, or a
         * read-only descriptor would write the file through the mapping. */
        const proc_file_t *mf = &current_proc->ofile[fd];
        if (!fd_readable(mf)) return -13;                         /* -EACCES */
        if (shared && (prot & PROT_WRITE_K) &&
            (mf->flags & O_ACCMODE) != O_RDWR)
            return -13;                                           /* -EACCES */
    }
    /* Shared file mappings the descriptor cannot write through stay so. */
    uint32_t nowrite = (fnode && shared &&
                        (current_proc->ofile[fd].flags & O_ACCMODE) != O_RDWR)
                       ? VMA_F_NOWRITE : 0;

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
            uint64_t kp = kprof_probe_begin();
            unmap_range(va, va + length);                 /* replace */
            kprof_probe_end(KPP_MMAP_UNMAP, kp);
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
        if (!vma_add(va, end, prot, VMA_F_SHARED | nowrite, NULL, 0)) return -12;
        /* SHARED: no COW on fork; bit 4 is PCD.  A PROT_READ view of the
         * framebuffer is read-only like any other mapping. */
        pte_t fb_flags = PAGE_PRESENT | PAGE_USER | PAGE_SHARED | (1U << 4) | PAGE_NX |
                            ((prot & PROT_WRITE_K) ? PAGE_WRITABLE : 0);
        for (uint32_t i = 0; i < length; i += PAGE_SIZE) {
            if (paging_map(va + i, fb_phys + i, fb_flags) != 0) {
                /* Device frames are not refcounted, so unmap_range drops the
                 * PTEs and the VMA without touching the physical allocator —
                 * the same path munmap of an fb0 mapping already takes. */
                unmap_range(va, end);
                return -12;
            }
        }
        return (int)va;
    }

    /* ── MAP_SHARED on a real file: share physical frames ──
     * memfd/tmpfs shared memory — all mappers of the same node see the same
     * frames (Firefox IPC / SharedStringMap depend on this).  Mapped writable
     * iff PROT_WRITE; PAGE_SHARED so fork shares rather than COWs. */
    if (shared && fnode) {
        struct shmap_entry *e = shmap_get(fnode);
        if (!e) return -12;
        /* mprotect can add PROT_WRITE later unless the descriptor cannot
         * write through the mapping (VMA_F_NOWRITE). */
        if ((prot & PROT_WRITE_K) || !nowrite)
            e->may_write = 1;
        struct vma *v = vma_add(va, end, prot, VMA_F_SHARED | nowrite, NULL, 0);
        if (!v) return -12;
        for (uint32_t i = 0; i < length; i += PAGE_SIZE) {
            uint32_t phys = shmap_frame(e, fnode, pgoff + i / PAGE_SIZE);
            if (!phys) { unmap_range(va, end); return -12; }
            pmm_frame_incref(phys);          /* this mapping's ref */
            if (paging_map(va + i, phys, pte_flags_for(prot, 1)) != 0) {
                pmm_frame_decref(phys);      /* give this mapping's ref back;
                                              * the registry keeps its own */
                unmap_range(va, end);
                return -12;
            }
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
            if (paging_map(va + i, phys, pte_flags_for(prot, 1)) != 0) {
                pmm_frame_decref(phys);      /* the only ref: frees the frame */
                unmap_range(va, end);
                return -12;
            }
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
        if (length < VMA_DEMAND_MIN) {
            uint64_t kp = kprof_probe_begin();
            int okp = vma_populate_range(v, va, end);
            kprof_probe_end(KPP_MMAP_POP, kp);
            if (!okp) { unmap_range(va, end); return -12; }
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
        if (length < VMA_FILE_DEMAND_MIN) {
            uint64_t kp = kprof_probe_begin();
            int okp = vma_populate_range(v, va, end);
            kprof_probe_end(KPP_MMAP_POP, kp);
            if (!okp) { unmap_range(va, end); return -12; }
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
    if (!pde_present(va)) return;      /* not populated */
    pte_t old = pte_get(va);
    if (!pte_mapped(old) || !(old & PAGE_USER)) return;
    pte_t nw = old & ~(pte_t)(PAGE_PRESENT | PAGE_WRITABLE | PAGE_PROTNONE |
                              PAGE_WRPROT | PAGE_NX);
    if (prot == 0) {
        nw |= PAGE_PROTNONE | PAGE_NX;
    } else {
        nw |= PAGE_PRESENT | page_nx_unless(prot & PROT_EXEC_K);
        if (prot & PROT_WRITE_K) {
            if (!(old & PAGE_COW)) nw |= PAGE_WRITABLE;
        } else {
            /* Record the denial on the page itself.  The VMA registry already
             * carries the protection of everything it covers, but the ELF
             * image (ld.so's PT_GNU_RELRO), the brk heap and the main stack are
             * not in it, and for those the write fault has nothing else to
             * consult: without this a COW break would hand write permission
             * straight back and the two address spaces would diverge in
             * silence where Linux raises SIGSEGV. */
            nw |= PAGE_WRPROT;
        }
    }
    if (nw != old) {
        pte_set(va, nw);
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
    prot = proc_prot_adjust(current_proc, prot & 0x7U);
    uint32_t end = addr + ((len + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1));
    if (end < addr || end > 0xC0000000U) return -22;

    /* Linux mprotect_fixup(): write access cannot be added to a shared file
     * mapping whose descriptor was read-only (-EACCES), checked for the whole
     * range before anything changes. */
    if (prot & PROT_WRITE_K) {
        struct proc *o = mmap_owner();
        for (struct vma *v = o ? o->vmas : NULL; v && v->start < end; v = v->next)
            if (v->end > addr && (v->flags & VMA_F_NOWRITE)) return -13;
    }

    /* Record the new prot on the VMAs covering this range, so pages that fault
     * in LATER get the right permissions.  Gaps are tolerated (Linux returns
     * ENOMEM): ld.so's RELRO mprotect targets the executable image, which is
     * not a VMA here, and glibc treats that failure as fatal. */
    vma_protect_range(addr, end, prot);

    for (uint32_t va = addr; va < end; ) {
        if (!pde_present(va)) {
            uint32_t n = pt_next(va);
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

    /* A SIGSEGV handler that changes protections is making progress, not
     * looping: write barriers (GCs, JITs, this kernel's wxprobe) fault the
     * same store instruction on page after page and unprotect each one.
     * Forget the last fault so the page-fault loop breaker only fires on a
     * handler that returns without fixing anything. */
    if (current_proc) {
        current_proc->last_fault_eip = 0;
        current_proc->fault_repeat   = 0;
    }
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
    phys_t batch[256];
    int nb = 0;
    for (uint32_t va = addr; va < end; ) {
        if (!pde_present(va)) {
            uint32_t n = pt_next(va);
            if (n <= va) break;
            va = n;
            continue;
        }
        pte_t old = pte_get(va);
        if (pte_mapped(old) && (old & PAGE_USER) && !(old & PAGE_SHARED)) {
            phys_t frame = pte_frame(old);
            if (vma_find(va)) {
                batch[nb++] = frame;             /* free AFTER the shootdown */
                pte_set(va, 0);
                tlb_flush_single(va);
                if (nb == 256) {
                    tlb_shootdown();
                    for (int i = 0; i < nb; i++) pmm_frame_decref(batch[i]);
                    nb = 0;
                }
            } else if (old & PAGE_PRESENT) {
                if (pmm_frame_refcount(frame) > 1) {
                    /* COW-shared: give this side a private zero frame */
                    phys_t nf = pmm_alloc_user_frame();
                    if (nf) {
                        pmm_frame_incref(nf);
                        preempt_disable();
                        __builtin_memset(paging_temp_map(nf), 0, PAGE_SIZE);
                        paging_temp_unmap();
                        preempt_enable();
                        /* Keep the page's own protection.  PAGE_WRPROT means
                         * the process took write permission away with
                         * mprotect(), and handing it a fresh frame is no
                         * reason to give that permission back — the same rule
                         * the write-fault handler applies when it refuses to
                         * break COW on such a page.  Without this the entry
                         * would come back writable while still flagged
                         * write-protected, and the next store would succeed
                         * where Linux raises SIGSEGV. */
                        pte_t nflags = old & (0xFFFU | PAGE_NX) & ~(pte_t)PAGE_COW;
                        if (!(old & PAGE_WRPROT)) nflags |= PAGE_WRITABLE;
                        pte_set(va, nf | nflags);
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
            int mapped = pde_present(va) &&
                         pte_mapped(pte_get(va));
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
    /* Reserve every destination page table FIRST.  The move clears each source
     * PTE before re-publishing it at the destination, so a mapping failure
     * partway would lose the page outright — the frame would still be
     * referenced by nothing the process can reach.  Failing here instead costs
     * only the snapshot's file refs. */
    if (paging_reserve_range(new, new + len, 1) != 0) {
        while (np) { np--; if (pcs[np].file) vfs_close(pcs[np].file); }
        return -12;
    }

    vma_remove_range(old, old + len);
    for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
        uint32_t va = old + off;
        if (!pde_present(va)) continue;
        pte_t e = pte_get(va);
        if (!pte_mapped(e)) continue;
        pte_set(va, 0);
        tlb_flush_single(va);
        /* Cannot fail: the table was reserved above. */
        if (paging_map(new + off, pte_frame(e), e & (0xFFFU | PAGE_NX)) != 0)
            panic("mremap: reserved page table vanished", NULL);
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

    int lerr;
    vfs_node_t *n = vfs_lookup_at(path, 1, &lerr);
    if (!n) return lerr;
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

    struct kstat64 kst;
    int r = fd_kstat64(fd, &kst);
    if (r < 0) return r;
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

    struct kstat64 kst;
    /* empty path + AT_EMPTY_PATH(0x1000) → stat dirfd itself, whatever kind of
     * descriptor it is (musl 1.2 implements fstat() as exactly this call);
     * without the flag an empty path is -ENOENT (below). */
    if (path[0] == '\0' && dirfd >= 0 && ((int)regs->edx & 0x1000)) {
        int r = fd_kstat64(dirfd, &kst);
        if (r < 0) return r;
    } else {
        /* Relative to dirfd, and with AT_SYMLINK_NOFOLLOW (0x100) the link
         * itself: musl's lstat() and fstatat() on i386 are this call. */
        if (path[0] == '\0') return -2;                       /* -ENOENT */
        char resolved[256];
        int r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
        if (r < 0) return r;
        int lerr;
        vfs_node_t *n = vfs_lookup(resolved, !((int)regs->edx & 0x100), &lerr);
        if (!n) return lerr;
        fill_kstat64(&kst, n);
    }

    struct kstatx sx;
    __builtin_memset(&sx, 0, sizeof(sx));
    sx.stx_mask    = 0x000007ff;          /* STATX_BASIC_STATS */
    sx.stx_blksize = 4096;
    sx.stx_nlink   = (uint32_t)kst.st_nlink;
    sx.stx_uid     = kst.st_uid;
    sx.stx_gid     = kst.st_gid;
    sx.stx_mode    = (uint16_t)kst.st_mode;
    sx.stx_ino     = kst.st_ino;
    sx.stx_dev_major  = (uint32_t)(kst.st_dev >> 8) & 0xFFFu;
    sx.stx_dev_minor  = (uint32_t)kst.st_dev & 0xFFu;
    sx.stx_rdev_major = (uint32_t)(kst.st_rdev >> 8) & 0xFFFu;
    sx.stx_rdev_minor = (uint32_t)kst.st_rdev & 0xFFu;
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
    /* struct sigaction { sa_handler; sa_flags; sa_restorer; sa_mask[2] } —
     * Linux i386 kernel_sigaction, 20 bytes.  Copy exactly that much: musl's
     * and glibc's k_sigaction are that size, and writing more into oact
     * overwrites whatever the caller keeps after it on the stack. */
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
            uint32_t koact[5];
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
    uint32_t     old_mask    = sh->mask[sig];

    if (oact) {
        uint32_t koact[5];
        __builtin_memset(koact, 0, sizeof(koact));
        koact[0] = (uint32_t)(uintptr_t)old_handler;
        koact[1] = old_flags;
        koact[3] = sigset_to_user(old_mask);   /* sa_mask[0] */
        int cr = copy_to_user(oact, koact, sizeof(koact));
        if (cr < 0) return cr;
    }
    if (act) {
        uint32_t kact[5];
        int cr = copy_from_user(kact, act, sizeof(kact));
        if (cr < 0) return cr;
        sh->handlers[sig] = (sighandler_t)(uintptr_t)kact[0];
        sh->flags[sig]    = kact[1];  /* sa_flags (SA_RESTART etc.) */
        /* sa_mask: blocked for the duration of the handler.  SIGKILL and
         * SIGSTOP are dropped from it (Linux do_sigaction:
         * sigdelsetmask(&act->sa.sa_mask, sigmask(SIGKILL)|sigmask(SIGSTOP))). */
        sh->mask[sig] = sigset_from_user(kact[3]) &
                        ~((1u << SIGKILL) | (1u << SIGSTOP));
        /* Linux do_sigaction: setting SIG_IGN (or SIG_DFL for a default-ignored
         * signal) discards matching signals already pending on every thread. */
        sighandler_t nh = sh->handlers[sig];
        if (nh == SIG_IGN || (nh == SIG_DFL && (sig == SIGCHLD || sig == SIGCONT)))
            for (int i = 0; i < MAX_PROCS; i++)
                if (ptable[i].state != PROC_UNUSED && ptable[i].sighand == sh) {
                    ptable[i].pending_sigs &= ~(1u << sig);
                    if (ptable[i].sigshared)
                        ptable[i].sigshared->pending &= ~(1u << sig);
                }
    }
    return 0;
}

/* ── sys_rt_sigprocmask(how, set, oset, sigsetsize) — EAX=175 ────────────── */
/* sigset_from_user()/sigset_to_user() (proc/signal.h) convert between the user
 * sigset_t, which numbers bit (sig - 1), and the kernel's bit-sig masks.  Only
 * the first word (signals 1..32) is honoured. */

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
        uint32_t nb;
        switch (how) {
        case 0: /* SIG_BLOCK */   nb = current_proc->blocked_sigs |  set; break;
        case 1: /* SIG_UNBLOCK */ nb = current_proc->blocked_sigs & ~set; break;
        case 2: /* SIG_SETMASK */ nb = set;                               break;
        default: return -22;  /* EINVAL */
        }
        /* Process-wide pending signals this thread starts blocking are handed
         * to a sibling that can take them (Linux set_current_blocked ->
         * retarget_shared_pending); ones it unblocks it takes itself on the way
         * back to user mode. */
        signal_retarget_shared(current_proc, nb & ~current_proc->blocked_sigs);
        current_proc->blocked_sigs = nb;
    }
    return 0;
}

/* ── sys_rt_sigpending(set, sigsetsize) — EAX=176, sigpending(set) — EAX=73 ──
 * The signals raised while blocked and still waiting, the thread's own and the
 * process's (Linux do_sigpending: (pending | shared_pending) & blocked).  Both
 * write one word of user sigset_t. */
static int sys_rt_sigpending(registers_t *regs) {
    uint32_t *uset = (uint32_t *)(uintptr_t)regs->ebx;
    uint32_t set = sigset_to_user(signal_pending_set(current_proc) &
                                  current_proc->blocked_sigs);
    if (copy_to_user(uset, &set, sizeof(set)) < 0) return -14;
    return 0;
}

/* access(2) family (Linux do_faccessat).  The check is made with the REAL
 * uid and gid — a set-uid program asks "may the user who ran me do this?" —
 * unless AT_EACCESS asks for the effective ids.  Supplementary groups count
 * either way.  F_OK (mode 0) only tests that the path resolves; for root, X_OK
 * on a non-directory still needs an execute bit somewhere. */
#define AT_SYMLINK_NOFOLLOW_K 0x100
#define AT_EACCESS_K          0x200
#define AT_EMPTY_PATH_K       0x1000
static int do_faccessat(int dirfd, const char *upath, int mode, int flags) {
    if (mode & ~7) return -22;                              /* -EINVAL */
    if (flags & ~(AT_SYMLINK_NOFOLLOW_K | AT_EACCESS_K | AT_EMPTY_PATH_K))
        return -22;
    char path[256], resolved[256];
    int r = copy_user_str(upath, path, sizeof(path));
    if (r < 0) return r;
    vfs_node_t *n;
    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_K)) return -2;          /* -ENOENT */
        if (dirfd == AT_FDCWD) {
            n = vfs_open(current_proc->cwd);
        } else {
            if (dirfd < 0 || dirfd >= MAX_FD) return -9;    /* -EBADF */
            proc_file_t *f = &current_proc->ofile[dirfd];
            if (f->type == FD_NONE) return -9;
            if (f->type != FD_FILE || !f->node)
                return mode ? -13 : 0;       /* pipe/socket: anon inode 0600 */
            n = f->node;
        }
        if (!n) return -2;
    } else {
        r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
        if (r < 0) return r;
        int lerr;
        n = vfs_lookup(resolved, !(flags & AT_SYMLINK_NOFOLLOW_K), &lerr);
        if (!n) return lerr;          /* -ENOENT / -ELOOP / -ENAMETOOLONG */
    }
    if (mode == 0) return 0;                                /* F_OK */
    struct proc *p = current_proc;
    int eff = (flags & AT_EACCESS_K) != 0;
    return vfs_access_check_groups(n, eff ? p->euid : p->uid,
                                   eff ? p->egid : p->gid,
                                   p->groups, p->ngroups, mode);
}

/* ── sys_access(path, mode) — EAX=33 ────────────────────────────────────── */
static int sys_access(registers_t *regs) {
    return do_faccessat(AT_FDCWD, (const char *)(uintptr_t)regs->ebx,
                        (int)regs->ecx, 0);
}

/* ── sys_dup(int oldfd) — EAX=41 ────────────────────────────────────────── */
static int sys_dup(registers_t *regs) {
    int oldfd = (int)regs->ebx;
    if (oldfd < 0 || oldfd >= MAX_FD) return -9;
    proc_file_t *src = &current_proc->ofile[oldfd];
    if (src->type == FD_NONE) return -9;

    for (int i = 0; i < MAX_FD; i++) {
        if (current_proc->ofile[i].type == FD_NONE) {
            fd_copy(&current_proc->ofile[i], src);
            current_proc->ofile[i].cloexec = 0;
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
    /* Linux ksys_setsid(): -EPERM when a process group with the caller's id
     * already exists — in particular when the caller is a group leader — so a
     * new session can never capture another process's group. */
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q->state == PROC_UNUSED) continue;         /* zombies still count */
        if (q->pgrp == current_proc->tgid) return -1;   /* -EPERM */
    }
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
    if (vfs_path_rdonly(path, 1)) return -30;                 /* -EROFS */
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
    else if (fmt == 0xC000)  vfs_flag = VFS_FLAG_SOCK;    /* S_IFSOCK = 0140000 */
    else return -22;  /* -EINVAL: unsupported type */
    /* A device node needs CAP_MKNOD (Linux vfs_mknod). */
    if (vfs_flag == VFS_FLAG_CHARDEV && current_proc->euid != 0)
        return -1;    /* -EPERM */

    /* Like every other create: write+search on the parent directory, and the
     * new node belongs to the caller's effective ids with mode & ~umask. */
    if (proc_access_check(dir, VFS_WANT_W | VFS_WANT_X) < 0)
        return -13;
    int r = dir->create_fn(dir, base, vfs_flag);
    if (r < 0) return r;
    vfs_node_t *node = vfs_open_nofollow(path);
    if (node)
        init_new_node(dir, node, mode & ~current_proc->umask);
    return r;
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

/* ── sys_reboot(magic, magic2, cmd, arg) — EAX=88 ─────────────────────────
 * Linux kernel/reboot.c: magic must be LINUX_REBOOT_MAGIC1 and magic2 one of
 * the four MAGIC2 values (else -EINVAL); unknown commands are -EINVAL.  The
 * power paths live in drivers/acpi.c (S5 via \_S5, FADT reset register).
 * Only root may call it (Linux CAP_SYS_BOOT, checked before the magic): the
 * desktop, which runs as the session user, asks init through /tmp/powerctl. */
#define LINUX_REBOOT_MAGIC1         0xFEE1DEAD
#define LINUX_REBOOT_CMD_RESTART    0x01234567
#define LINUX_REBOOT_CMD_HALT       0xCDEF0123
#define LINUX_REBOOT_CMD_CAD_ON     0x89ABCDEF
#define LINUX_REBOOT_CMD_CAD_OFF    0x00000000
#define LINUX_REBOOT_CMD_POWER_OFF  0x4321FEDC
static int sys_reboot(registers_t *regs) {
    uint32_t magic = regs->ebx, magic2 = regs->ecx, cmd = regs->edx;
    if (!current_proc || current_proc->euid != 0)
        return -1;    /* -EPERM */
    if (magic != LINUX_REBOOT_MAGIC1 ||
        (magic2 != 672274793 && magic2 != 85072278 &&
         magic2 != 369367448 && magic2 != 537993216))
        return -22;   /* -EINVAL */

    /* NVMe asks for an orderly shutdown notification before power goes. */
    if (cmd == LINUX_REBOOT_CMD_POWER_OFF || cmd == LINUX_REBOOT_CMD_RESTART)
        nvme_shutdown();

    switch (cmd) {
    case LINUX_REBOOT_CMD_POWER_OFF:
        acpi_poweroff();
    case LINUX_REBOOT_CMD_RESTART:
        acpi_reboot();
    case LINUX_REBOOT_CMD_HALT:
        /* Linux kernel_halt(): stop the other CPUs, then this one.  Writes
         * are already on disk (fsync/sync have nothing to flush here). */
        __asm__ volatile("cli");
        smp_stop_others();
        printk("[REBOOT] System halted.\n");
        for (;;) __asm__ volatile("cli; hlt");
    case LINUX_REBOOT_CMD_CAD_ON:
    case LINUX_REBOOT_CMD_CAD_OFF:
        return 0;
    default:
        return -22;
    }
}

/* ── sys_lstat(path, stat*) — EAX=107 ───────────────────────────────────── */
static int sys_lstat(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0) return -14;
    struct kstat *st = (struct kstat *)(uintptr_t)regs->ecx;
    if (!access_ok(st, sizeof(*st))) return -14;
    /* lstat does NOT follow the final symlink */
    int err;
    vfs_node_t *n = vfs_lookup_at(path, 0, &err);
    if (!n) return err;
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
    int err;
    vfs_node_t *n = vfs_lookup_at(path, 0, &err);
    if (!n) return err;
    struct kstat64 kst;
    fill_kstat64(&kst, n);
    if (n->flags == VFS_FLAG_SYMLINK)
        kst.st_mode = (kst.st_mode & ~0xF000) | 0xA000;
    return copy_to_user(st, &kst, sizeof(kst));
}

/* ── sys_symlink(target, linkpath) — EAX=83 ─────────────────────────────── */
static int symlink_at(uint32_t utarget, int dirfd, uint32_t ulinkpath) {
    char target[512], linkpath[512];
    /* -EFAULT, or -ENAMETOOLONG for a string that does not fit. */
    int rc = copy_user_str((const char *)(uintptr_t)utarget, target, 512);
    if (rc < 0) return rc;
    rc = copy_user_str((const char *)(uintptr_t)ulinkpath, linkpath, 512);
    if (rc < 0) return rc;
    /* Resolve linkpath relative to dirfd or the cwd (bounded: cwd + a 511-byte
     * user path used to be joined into a 512-byte stack buffer unchecked). */
    char abspath[256];
    rc = resolve_path_at_fd(dirfd, linkpath, abspath, sizeof(abspath));
    if (rc < 0) return rc;
    /* Creating the link needs write+search on its directory, and the link is
     * the caller's (effective ids). */
    char dir_path[256], base[256];
    if (path_split(abspath, dir_path, base) < 0 || base[0] == '\0') return -22;
    if (vfs_path_rdonly(abspath, 1)) return -30;              /* -EROFS */
    vfs_node_t *dir = vfs_open_parent_at(abspath, dir_path);
    if (!dir) return -2;
    if (proc_access_check(dir, VFS_WANT_W | VFS_WANT_X) < 0)
        return -13;
    rc = vfs_symlink(target, abspath);
    if (rc == 0) {
        vfs_node_t *link = vfs_open_nofollow(abspath);
        if (link)
            vfs_setattr(link, 0777, current_proc->euid, current_proc->egid);
    }
    return rc;
}

static int sys_symlink(registers_t *regs) {
    return symlink_at(regs->ebx, AT_FDCWD, regs->ecx);
}

/* ── sys_symlinkat(target, newdirfd, linkpath) — EAX=304 ─────────────────── */
static int sys_symlinkat(registers_t *regs) {
    return symlink_at(regs->ebx, (int)regs->ecx, regs->edx);
}

/* readlink of /proc/self/fd/N or /proc/<pid>/fd/N: what the descriptor
 * stands for, as Linux's magic links answer it — the path it was opened by
 * (relative to the caller's root, as every fd path here is), or
 * "pipe:[id]", "socket:[id]", "anon_inode:[...]".  musl's ttyname() reads
 * it (sshd checks the pty it allocated that way).  Returns the length, -2
 * for a closed descriptor, or 1 when `abs` is not such a path. */
static int path_has_prefix(const char *s, const char *pre) {
    while (*pre)
        if (*s++ != *pre++) return 0;
    return 1;
}

static int proc_fd_readlink(const char *abs, char *out, uint32_t cap) {
    if (!path_has_prefix(abs, "/proc/")) return 1;
    const char *p = abs + 6;
    struct proc *who = NULL;
    if (path_has_prefix(p, "self/")) {
        who = current_proc;
        p += 5;
    } else if (path_has_prefix(p, "thread-self/")) {
        who = current_proc;
        p += 12;
    } else {
        int pid = 0;
        if (*p < '0' || *p > '9') return 1;
        while (*p >= '0' && *p <= '9') pid = pid * 10 + (*p++ - '0');
        if (*p++ != '/') return 1;
        for (int i = 0; i < MAX_PROCS; i++)
            if (ptable[i].state != PROC_UNUSED && ptable[i].pid == pid) {
                who = &ptable[i];
                break;
            }
        if (!who) return 1;
    }
    if (!path_has_prefix(p, "fd/")) return 1;
    /* Another process's descriptors are its business: root or the same
     * user only (Linux: PTRACE_MODE_READ on proc_fd_link). */
    if (who != current_proc && who->tgid != current_proc->tgid &&
        current_proc->euid != 0 && current_proc->euid != who->uid)
        return -13;                                         /* -EACCES */
    p += 3;
    int fd = 0;
    if (*p < '0' || *p > '9') return 1;
    while (*p >= '0' && *p <= '9') {
        fd = fd * 10 + (*p++ - '0');
        if (fd >= MAX_FD) return -2;
    }
    if (*p) return 1;
    if (!who->ofile) return -2;
    proc_file_t *f = &who->ofile[fd];
    uint32_t id = f->fid ? f->fid : (uint32_t)fd + 1;
    int n;
    switch (f->type) {
    case FD_NONE:    return -2;                             /* -ENOENT */
    case FD_FILE:    n = snprintf(out, cap, "%s", f->path[0] ? f->path : "/"); break;
    case FD_PIPE_R: case FD_PIPE_W:
                     n = f->path[0] ? snprintf(out, cap, "%s", f->path)
                                    : snprintf(out, cap, "pipe:[%u]", id);
                     break;
    case FD_SOCKET: case FD_USOCKET:
                     n = snprintf(out, cap, "socket:[%u]", id); break;
    case FD_EPOLL:   n = snprintf(out, cap, "anon_inode:[eventpoll]"); break;
    case FD_EVENTFD: n = snprintf(out, cap, "anon_inode:[eventfd]"); break;
    default:         n = snprintf(out, cap, "anon_inode:[%u]", id); break;
    }
    return n < (int)cap ? n : (int)cap - 1;
}

/* ── sys_readlink(path, buf, bufsiz) — EAX=85 ───────────────────────────── */
static int sys_readlink(registers_t *regs) {
    char path[512];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 512) < 0) return -14;
    char *ubuf = (char *)(uintptr_t)regs->ecx;
    int bufsiz = (int)regs->edx;
    if (bufsiz <= 0 || !access_ok(ubuf, (uint32_t)bufsiz)) return -14;

    {
        char abs[256], link[256];
        if (resolve_path_at_fd(AT_FDCWD, path, abs, sizeof(abs)) == 0) {
            int l = proc_fd_readlink(abs, link, sizeof(link));
            if (l != 1) {
                if (l < 0) return l;
                if (l > bufsiz) l = bufsiz;
                return copy_to_user(ubuf, link, (uint32_t)l) < 0 ? -14 : l;
            }
        }
    }
    /* Resolve without following final symlink */
    int err;
    vfs_node_t *n = vfs_lookup_at(path, 0, &err);
    if (!n) return err;
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
    if ((int32_t)len < 0 && regs->eax == 92) return -22;   /* -EINVAL */
    int lerr;
    vfs_node_t *n = vfs_lookup_at(path, 1, &lerr);
    if (!n) return lerr;
    /* Linux do_sys_truncate(): a directory is -EISDIR, anything else that is
     * not a regular file -EINVAL, and the caller needs write permission on
     * the file itself. */
    if (n->flags == VFS_FLAG_DIR) return -21;              /* -EISDIR */
    if (n->flags != VFS_FLAG_FILE) return -22;             /* -EINVAL */
    if (proc_access_check(n, VFS_WANT_W) < 0)
        return -13;                                        /* -EACCES */
    {
        char abs[256];
        if (canonicalize_path_at_cwd(path, abs, sizeof(abs)) == 0 &&
            vfs_path_rdonly(abs, 0))
            return -30;                                    /* -EROFS */
    }
    return vfs_truncate(n, len);
}

/* ── sys_ftruncate(fd, length) — EAX=93 ─────────────────────────────────── */
static int sys_ftruncate(registers_t *regs) {
    int fd = (int)regs->ebx;
    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type != FD_FILE || !f->node) return -9;
    /* Linux do_sys_ftruncate(): -EINVAL unless the descriptor was opened for
     * writing (the permission was checked when it was opened). */
    if (!fd_writable(f)) return -22;
    if (fd_rofs(f)) return -30;                    /* -EROFS */
    /* A negative length is -EINVAL (93 takes a signed 32-bit off_t; 194
     * reaches here with the non-negative low word of a 64-bit one). */
    if ((int32_t)regs->ecx < 0 && regs->eax == 93) return -22;
    return vfs_truncate(f->node, (uint32_t)regs->ecx);
}

/* ── sys_truncate64(path, length_lo, length_hi) — EAX=193 ───────────────────
 * ── sys_ftruncate64(fd,  length_lo, length_hi) — EAX=194 ───────────────────
 * Linux i386 carries a 64-bit-offset (f)truncate pair alongside the legacy
 * 92/93; musl always issues these (src/unistd/{f,}truncate.c pass the length
 * as the __SYSCALL_LL_O register pair ECX:EDX).  Our files are below 4 GiB, so
 * a nonzero high word is -EFBIG, exactly what Linux reports past s_maxbytes. */
static int sys_truncate64(registers_t *regs) {
    if ((int32_t)regs->edx < 0) return -22;        /* -EINVAL: negative */
    if (regs->edx) return -27;                     /* -EFBIG */
    return sys_truncate(regs);                     /* ECX already holds the low word */
}

static int sys_ftruncate64(registers_t *regs) {
    if ((int32_t)regs->edx < 0) return -22;        /* -EINVAL: negative */
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
    if (!fd_writable(f)) return -9;                /* Linux vfs_fallocate: -EBADF */
    if (fd_rofs(f)) return -30;                    /* -EROFS */
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
    case FD_PIPE_W: return 0;   /* Linux pipe_poll: a write end never reads */
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
    case FD_PIPE_R: return 0;   /* ... nor is a read end ever writable */
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


static int sys_poll(registers_t *regs) {
    uint32_t *fds  = (uint32_t *)(uintptr_t)regs->ebx;
    uint32_t  nfds = regs->ecx;
    int       toms = (int)regs->edx;

    if (!nfds) return 0;
    if (nfds > 1024) return -22;   /* -EINVAL */
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
            } else {
                proc_file_t *f = &current_proc->ofile[fd];
                if ((events & POLLIN)  && fd_read_ready(fd))  rev |= POLLIN;
                if ((events & POLLOUT) && fd_write_ready(fd)) rev |= POLLOUT;
                if (f->type == FD_PIPE_R && f->pipe->nwriters == 0) rev |= POLLHUP;
                if (f->type == FD_PIPE_W && f->pipe->nreaders == 0) rev |= POLLERR;
                /* A closed AF_UNIX peer is a hangup, reported whether or not
                 * the caller asked for it (net/unix/af_unix.c unix_poll:
                 * EPOLLHUP once sk_shutdown is SHUTDOWN_MASK). */
                if (f->type == FD_USOCKET && usocket_hup(f->usock)) rev |= POLLHUP;
                if (f->type == FD_SOCKET && net_socket_poll_err(f->socket)) rev |= POLLERR;
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
        {
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
/* An item names a descriptor NUMBER plus the identity (proc_file_t.fid) of
 * the open file it held when it was added.  Linux ties a registration to the
 * struct file, so it ends when that file's last descriptor closes: here an
 * item whose number now holds another file (closed, or closed and reused) is
 * dead to the caller's table — never reported, and replaced by an ADD of the
 * number, whose MOD/DEL see ENOENT.  It is not freed eagerly because a forked
 * sharer of the epoll can still hold that file under the same number; its
 * slot is reclaimed when the set is full. */
struct epoll {
    struct {
        int      fd;          /* -1 = empty slot */
        uint32_t fid;         /* open-file identity at ADD time */
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

/* Does item i still watch the open file it was added for, in the caller's
 * table? */
static int epoll_item_live(struct epoll *ep, int i) {
    int fd = ep->items[i].fd;
    if (fd < 0 || fd >= MAX_FD) return 0;
    proc_file_t *f = &current_proc->ofile[fd];
    return f->type != FD_NONE && f->fid && f->fid == ep->items[i].fid;
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

    proc_file_t *wf = &current_proc->ofile[fd];
    switch (op) {
    case 1: { /* EPOLL_CTL_ADD */
        if (wf->type == FD_EPOLL && wf->epoll == ep) return -22;  /* itself */
        int slot = -1;
        for (int i = 0; i < EPOLL_MAX_ITEMS; i++) {
            if (ep->items[i].fd != fd) continue;
            if (epoll_item_live(ep, i)) return -17;  /* -EEXIST */
            slot = i;              /* the number's previous file is gone */
        }
        for (int i = 0; i < EPOLL_MAX_ITEMS && slot < 0; i++)
            if (ep->items[i].fd == -1) slot = i;
        for (int i = 0; i < EPOLL_MAX_ITEMS && slot < 0; i++)
            if (!epoll_item_live(ep, i)) slot = i;
        if (slot < 0) return -28;  /* -ENOSPC */
        if (!wf->fid) wf->fid = fd_new_fid();   /* never copied yet */
        ep->items[slot].fd  = fd;
        ep->items[slot].fid = wf->fid;
        ep->items[slot].events = ev.events;
        __builtin_memcpy(ep->items[slot].data, ev.data, 8);
        return 0;
    }
    case 3: /* EPOLL_CTL_MOD */
        for (int i = 0; i < EPOLL_MAX_ITEMS; i++)
            if (ep->items[i].fd == fd && epoll_item_live(ep, i)) {
                ep->items[i].events = ev.events;
                __builtin_memcpy(ep->items[i].data, ev.data, 8);
                return 0;
            }
        return -2;   /* -ENOENT */
    case 2: /* EPOLL_CTL_DEL */
        for (int i = 0; i < EPOLL_MAX_ITEMS; i++)
            if (ep->items[i].fd == fd && epoll_item_live(ep, i)) {
                ep->items[i].fd = -1;
                return 0;
            }
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
            if (!epoll_item_live(ep, i)) continue;
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

/* ── sys_waitid(idtype, id, infop, options, rusage) — EAX=284 ─────────────
 * Linux kernel/exit.c: the same wait set as waitpid (see sys_waitpid), with
 * the result reported as a SIGCHLD siginfo instead of a status word, and
 * WNOWAIT, which reports a child without reaping it.  Firefox's IPC process
 * watcher (base::IsProcessDead) asks exactly that - waitid(P_PID, pid,
 * WEXITED | WNOWAIT | WNOHANG) - to learn whether a content process has
 * exited while leaving the reaping to its waitpid; with -ENOSYS every such
 * check came back as an error ("[SYSCALL] unimplemented 284" once per boot,
 * on every page load). */
#define WSTOPPED_W   2
#define WEXITED_W    4
#define WCONTINUED_W 8
#define WNOWAIT_W    0x01000000
#define P_ALL_W  0
#define P_PID_W  1
#define P_PGID_W 2
#define CLD_EXITED_W  1
#define CLD_KILLED_W  2
#define CLD_DUMPED_W  3
#define CLD_STOPPED_W 5
#define CLD_CONTINUED_W 6

/* Fill the SIGCHLD siginfo for child `p` (i386 layout: signo, errno, code,
 * then si_pid, si_uid, si_status at 12/16/20; 128 bytes in all). */
static int waitid_report(void *infop, struct proc *p, int code, int status) {
    if (!infop) return 0;
    int32_t si[32];
    __builtin_memset(si, 0, sizeof si);
    si[0] = SIGCHLD;
    si[2] = code;
    si[3] = p->pid;
    si[4] = (int32_t)p->uid;
    si[5] = status;
    return copy_to_user(infop, si, sizeof si);
}

static int sys_waitid(registers_t *regs) {
    int      idtype  = (int)regs->ebx;
    int      id      = (int)regs->ecx;
    void    *infop   = (void *)(uintptr_t)regs->edx;
    int      options = (int)regs->esi;
    void    *ru      = (void *)(uintptr_t)regs->edi;

    /* __WNOTHREAD/__WALL/__WCLONE are accepted and change nothing here. */
    if (options & ~(WNOHANG | WSTOPPED_W | WEXITED_W | WCONTINUED_W | WNOWAIT_W |
                    0x20000000 | 0x40000000 | (int)0x80000000))
        return -22;                                          /* -EINVAL */
    if (!(options & (WSTOPPED_W | WEXITED_W | WCONTINUED_W)))
        return -22;
    if (idtype == P_PID_W) {
        if (id <= 0) return -22;
    } else if (idtype == P_PGID_W) {
        if (id < 0) return -22;
        if (id == 0) id = current_proc->pgrp;
    } else if (idtype != P_ALL_W) {
        return -22;                                          /* incl. P_PIDFD */
    }
    if (infop && !access_ok(infop, 128)) return -14;
    if (ru) {
        uint8_t z[72];
        __builtin_memset(z, 0, sizeof z);
        int cr = copy_to_user(ru, z, sizeof z);
        if (cr < 0) return cr;
    }

    struct proc *me = proc_group_leader(current_proc);
    int my_tgid = current_proc->tgid;

    for (;;) {
        int found_child = 0;
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            if (p->state == PROC_UNUSED) continue;
            if (!p->parent || p->parent->tgid != my_tgid) continue;
            if (p->pid != p->tgid) continue;               /* threads: never */
            if (idtype == P_PID_W && p->pid != id) continue;
            if (idtype == P_PGID_W && p->pgrp != id) continue;
            found_child = 1;

            /* A zombie leader whose siblings still run is not reapable yet,
             * but the process can still be stopped or continued (below), as
             * in sys_waitpid. */
            if ((options & WEXITED_W) && p->state == PROC_ZOMBIE &&
                proc_group_empty(p)) {
                int st = p->exit_status;
                int code = (st & 0x7f) == 0 ? CLD_EXITED_W
                         : (st & 0x80) ? CLD_DUMPED_W : CLD_KILLED_W;
                int val = code == CLD_EXITED_W ? (st >> 8) & 0xff : st & 0x7f;
                int cr = waitid_report(infop, p, code, val);
                if (cr < 0) return cr;
                if (!(options & WNOWAIT_W)) proc_release(p);
                return 0;
            }
            /* Group stop / continue: the same once-per-event reports as
             * sys_waitpid's WUNTRACED / WCONTINUED (the process is stopped
             * once every live thread is), except that WNOWAIT leaves the
             * event to be reported again. */
            if ((options & WSTOPPED_W) && proc_group_stopped(p) &&
                !p->stop_reported) {
                int cr = waitid_report(infop, p, CLD_STOPPED_W,
                                       p->stop_sig ? p->stop_sig : SIGSTOP);
                if (cr < 0) return cr;
                if (!(options & WNOWAIT_W)) p->stop_reported = 1;
                return 0;
            }
            if ((options & WCONTINUED_W) && p->group_continued) {
                int cr = waitid_report(infop, p, CLD_CONTINUED_W, SIGCONT);
                if (cr < 0) return cr;
                if (!(options & WNOWAIT_W)) p->group_continued = 0;
                return 0;
            }
        }

        if (!found_child)
            return -10;                                      /* -ECHILD */

        if (options & WNOHANG) {
            /* Nothing to report: Linux clears signo/errno/code/pid/uid/
             * status, and si_pid == 0 is how the caller tells "still
             * running" from a report. */
            if (infop) {
                int32_t z[6] = { 0, 0, 0, 0, 0, 0 };
                int cr = copy_to_user(infop, z, sizeof z);
                if (cr < 0) return cr;
            }
            return 0;
        }

        if (signal_interrupt_pending(current_proc))
            return -4;                                       /* -EINTR */
        sleep_on(me);
    }
}

/* ── sys_getrusage(who, usage) — EAX=77 ────────────────────────────────────
 * struct rusage on i386 is two timevals and fourteen longs (72 bytes).  Only
 * user CPU time is accounted (utime_ticks), so that is what is reported for
 * RUSAGE_SELF (the thread group) and RUSAGE_THREAD; the rest reads zero.
 * Firefox calls it for its CPU-time telemetry. */
static int sys_getrusage(registers_t *regs) {
    int   who = (int)regs->ebx;
    void *ru  = (void *)(uintptr_t)regs->ecx;
    uint32_t t;
    if (who == 0)       t = cputime_ticks(current_proc->tgid, 0);   /* SELF */
    else if (who == 1)  t = cputime_ticks(0, current_proc->pid);    /* THREAD */
    else if (who == -1) t = 0;                                      /* CHILDREN */
    else return -22;
    int32_t r[18];
    __builtin_memset(r, 0, sizeof r);
    r[0] = (int32_t)(t / TICK_HZ);
    r[1] = (int32_t)((t % TICK_HZ) * (1000000U / TICK_HZ));
    return copy_to_user(ru, r, sizeof r);
}

/* ── sched_get_priority_max/min(policy) — EAX=159/160 ─────────────────────
 * The static priority range per policy (kernel/sched/syscalls.c): 1..99 for
 * SCHED_FIFO(1)/SCHED_RR(2), 0 for SCHED_OTHER(0)/BATCH(3)/IDLE(5)/
 * DEADLINE(6), EINVAL otherwise.  159 used to be wired to sched_yield (and
 * the in-tree libc's sched_yield called 159), so a caller asking for a
 * priority range yielded and got 0 for every policy. */
static int sys_sched_get_priority(registers_t *regs, int max) {
    switch ((int)regs->ebx) {
    case 1: case 2:                 return max ? 99 : 1;
    case 0: case 3: case 5: case 6: return 0;
    default:                        return -22;
    }
}

/* ── sys_getrlimit / sys_setrlimit — EAX=76/75 (stubs) ──────────────────── */
static int sys_getrlimit(registers_t *regs) {
    /* struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; } */
    int resource = (int)regs->ebx;
    uint32_t *rl = (uint32_t *)(uintptr_t)regs->ecx;
    if (rl) {
        uint32_t krl[2] = { 0xFFFFFFFFU, 0xFFFFFFFFU }; /* RLIM_INFINITY */
        if (resource == 3 /*RLIMIT_STACK*/) krl[0] = 8U * 1024 * 1024;
        if (resource == 7 /*RLIMIT_NOFILE*/) krl[0] = krl[1] = MAX_FD;
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
#define RLIMIT_NOFILE 7      /* the fd table really is MAX_FD entries */
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
        } else if (resource == RLIMIT_NOFILE) {
            rl.cur = rl.max = MAX_FD;
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
        } else if (resource == RLIMIT_NOFILE) {
            krl[0] = krl[1] = MAX_FD;
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
    (void)regs;
    return 0;
}

/* ── sys_sigaltstack(ss, oss) — EAX=186 ─────────────────────────────────────
 * stack_t on i386 is { void *ss_sp; int ss_flags; size_t ss_size; }.  A handler
 * installed with SA_ONSTACK runs on this stack instead of the interrupted one,
 * which is how a program survives a SIGSEGV caused by its own stack overflow
 * (and how glibc/SpiderMonkey handle theirs).  Linux do_sigaltstack:
 * SS_DISABLE removes it, any other unknown flag is EINVAL, a stack smaller than
 * MINSIGSTKSZ is ENOMEM, and it may not be changed while running on it. */
#define MINSIGSTKSZ_K 2048
static int sys_sigaltstack(registers_t *regs) {
    uint32_t uss  = regs->ebx;
    uint32_t uoss = regs->ecx;
    uint32_t sp   = regs->useresp;
    int on = current_proc->sas_size &&
             sp >= current_proc->sas_sp &&
             sp <  current_proc->sas_sp + current_proc->sas_size;

    if (uoss) {
        uint32_t old[3];
        old[0] = current_proc->sas_sp;
        old[1] = (uint32_t)(on ? SS_ONSTACK
                               : (current_proc->sas_size ? 0 : SS_DISABLE));
        old[2] = current_proc->sas_size;
        if (copy_to_user((void *)(uintptr_t)uoss, old, sizeof(old)) < 0) return -14;
    }
    if (uss) {
        uint32_t ns[3];
        if (copy_from_user(ns, (void *)(uintptr_t)uss, sizeof(ns)) < 0) return -14;
        if (on) return -1;                       /* -EPERM while running on it */
        if (ns[1] & SS_DISABLE) {
            current_proc->sas_sp = current_proc->sas_size = 0;
            return 0;
        }
        if (ns[1] & ~(uint32_t)SS_ONSTACK) return -22;     /* -EINVAL */
        if (ns[2] < MINSIGSTKSZ_K) return -12;             /* -ENOMEM */
        if (!access_ok((void *)(uintptr_t)ns[0], ns[2])) return -14;
        current_proc->sas_sp   = ns[0];
        current_proc->sas_size = ns[2];
    }
    return 0;
}

/* ── sys_pread64(fd, buf, count, offset_lo, offset_hi) — EAX=180 ─────────── */
static int sys_pread64(registers_t *regs) {
    int      fd  = (int)regs->ebx;
    char    *buf = (char *)(uintptr_t)regs->ecx;
    int      len = (int)(uint32_t)regs->edx;
    uint32_t off = regs->esi;  /* offset low 32 bits (high 32 bits in edi) */

    if (len < 0 || !access_ok(buf, (size_t)len)) return -14;
    if (fd < 0 || fd >= MAX_FD) return -9;

    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type != FD_FILE || !f->node) return -9;
    if (!fd_readable(f)) return -9;
    if ((int32_t)regs->edi < 0) return -22;                /* -EINVAL */
    if (regs->edi) return 0;                /* past the largest file: EOF */

    return vfs_read_user(f->node, off, buf, (uint32_t)len);
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
    if (!fd_writable(f) || !f->node->write_fn) return -9;
    /* The offset is 64-bit (esi:edi); files here are at most 4 GiB. */
    if ((int32_t)regs->edi < 0) return -22;                /* -EINVAL */
    if (regs->edi) return len ? -27 : 0;                   /* -EFBIG */

    return vfs_write_user(f->node, off, buf, (uint32_t)len);
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
    int rc = sys_open_kernel_path(resolved, flags, regs->esi);
    return rc;
}

static int sys_mkdir_kernel_path(const char *path, uint32_t mode) {
    char dir_path[256], base[256];
    /* mkdir("/") is -EEXIST on Linux; `mkdir -p /x` starts with it. */
    if (path[0] == '/' && path[1] == '\0') return -17;
    /* An existing name is -EEXIST whatever lies behind it, including the
     * boot mountpoints, whose parent has no create_fn. */
    if (vfs_open_nofollow(path)) return -17;
    if (vfs_path_rdonly(path, 1)) return -30;                 /* -EROFS */
    if (path_split(path, dir_path, base) < 0)
        return -2;
    if (base[0] == '\0') return -22;

    vfs_node_t *dir = vfs_open_parent_at(path, dir_path);
    if (!dir || !dir->create_fn) return -2;
    /* Need write+search on the parent to create a directory in it. */
    if (proc_access_check(dir, VFS_WANT_W | VFS_WANT_X) < 0)
        return -13;
    int r = dir->create_fn(dir, base, VFS_FLAG_DIR);
    if (r < 0) return r;
    /* mode & ~umask, permission and sticky bits only (Linux vfs_mkdir:
     * S_IRWXUGO|S_ISVTX); set-group-ID comes from the parent, if at all. */
    vfs_node_t *node = vfs_open_at(path);
    if (node)
        init_new_node(dir, node, mode & ~current_proc->umask & 01777);
    return r;
}

/* Linux may_delete(): removing (or renaming away, or replacing) the entry
 * `victim` of `dir` needs write and search permission on the directory
 * (-EACCES).  In a sticky directory (/tmp) the caller must in addition own the
 * entry or the directory, or be root (-EPERM). */
static int may_delete(vfs_node_t *dir, vfs_node_t *victim) {
    struct proc *p = current_proc;
    if (proc_access_check(dir, VFS_WANT_W | VFS_WANT_X) < 0)
        return -13;                                      /* -EACCES */
    if ((dir->mask & 01000) && p->euid != 0 &&
        p->euid != victim->uid && p->euid != dir->uid)
        return -1;                                       /* -EPERM */
    return 0;
}

/* unlink() and rmdir() of an already-resolved absolute path.  The entry is
 * looked up without following a final symlink: unlink removes the link, and
 * rmdir of a link to a directory is -ENOTDIR, as on Linux. */
static int remove_kernel_path(const char *path, int want_dir) {
    if (vfs_path_rdonly(path, 1)) return -30;                 /* -EROFS */
    char dir_path[256], base[256];
    if (path_split(path, dir_path, base) < 0)
        return -2;
    if (base[0] == '\0') return -22;

    vfs_node_t *dir = vfs_open_parent_at(path, dir_path);
    if (!dir) return -2;
    vfs_node_t *victim = vfs_finddir(dir, base);
    if (!victim) return -2;                              /* -ENOENT */
    if (vfs_is_mountpoint(victim)) return -16;           /* -EBUSY */
    if (want_dir && victim->flags != VFS_FLAG_DIR) return -20;   /* -ENOTDIR */
    /* unlink() never removes a directory; Linux reports -EISDIR (POSIX
     * allows -EPERM), and rmdir() is the call for that. */
    if (!want_dir && victim->flags == VFS_FLAG_DIR) return -21;  /* -EISDIR */
    int r = may_delete(dir, victim);
    if (r < 0) return r;
    if (want_dir) {
        /* Remove an empty directory (Linux fs/namei.c do_rmdir): -ENOTEMPTY
         * while it still has entries. */
        vfs_dirent_t de;
        for (uint32_t i = 0; i < 65536 && vfs_readdir(victim, i, &de) == 0; i++) {
            if (de.name[0] == '.' &&
                (de.name[1] == '\0' || (de.name[1] == '.' && de.name[2] == '\0')))
                continue;                                /* "." and ".." */
            return -39;                                  /* -ENOTEMPTY */
        }
    }
    return vfs_unlink(dir, base);
}

static int sys_unlink_kernel_path(const char *path) {
    return remove_kernel_path(path, 0);
}

static int sys_rmdir_kernel_path(const char *path) {
    return remove_kernel_path(path, 1);
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
    return sys_mkdir_kernel_path(resolved, regs->edx);
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
    int flags = (int)regs->esi;
    if (!access_ok(st, sizeof(*st))) return -14;
    char path[256], resolved[256];
    int r = copy_user_str(upath, path, sizeof(path));
    if (r < 0) return r;
    struct kstat64 kst;
    if (path[0] == '\0' && dirfd >= 0) {  /* AT_EMPTY_PATH: stat dirfd itself */
        r = fd_kstat64(dirfd, &kst);
        if (r < 0) return r;
        return copy_to_user(st, &kst, sizeof(kst));
    }
    r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    int lerr;
    /* AT_SYMLINK_NOFOLLOW: stat the link itself, as lstat() does. */
    vfs_node_t *n = vfs_lookup(resolved, !(flags & AT_SYMLINK_NOFOLLOW_K), &lerr);
    if (!n) return lerr;
    fill_kstat64(&kst, n);
    return copy_to_user(st, &kst, sizeof(kst));
}

/* faccessat(dirfd, path, mode) — EAX=307 (no flags argument, like Linux) */
static int sys_faccessat(registers_t *regs) {
    return do_faccessat((int)regs->ebx, (const char *)(uintptr_t)regs->ecx,
                        (int)regs->edx, 0);
}

/* faccessat2(dirfd, path, mode, flags) — EAX=439: AT_EACCESS,
 * AT_SYMLINK_NOFOLLOW and AT_EMPTY_PATH (musl and glibc use it for flags). */
static int sys_faccessat2(registers_t *regs) {
    return do_faccessat((int)regs->ebx, (const char *)(uintptr_t)regs->ecx,
                        (int)regs->edx, (int)regs->esi);
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
    {
        char link[256];
        int l = proc_fd_readlink(resolved, link, sizeof(link));
        if (l != 1) {
            if (l < 0) return l;
            if (l > bufsiz) l = bufsiz;
            return copy_to_user(ubuf, link, (uint32_t)l) < 0 ? -14 : l;
        }
    }
    int lerr;
    vfs_node_t *n = vfs_lookup(resolved, 0, &lerr);
    if (!n) return lerr;
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

/* ── sys_renameat2(olddirfd, old, newdirfd, new, flags) — EAX=353 ─────────
 * RENAME_NOREPLACE fails with EEXIST when `new` exists (checked and done in
 * one syscall under the BKL); RENAME_EXCHANGE and RENAME_WHITEOUT are
 * EINVAL, what Linux answers for a filesystem that lacks them. */
static int sys_renameat2(registers_t *regs) {
    uint32_t flags = regs->edi;
    if (flags & ~1u) return -22;
    if (flags & 1u) {                                  /* RENAME_NOREPLACE */
        char newpath[256], newres[256];
        int r = copy_user_str((const char *)(uintptr_t)regs->esi, newpath,
                              sizeof(newpath));
        if (r < 0) return r;
        r = resolve_path_at_fd((int)regs->edx, newpath, newres, sizeof(newres));
        if (r < 0) return r;
        int lerr;
        if (vfs_lookup(newres, 0, &lerr)) return -17;  /* -EEXIST */
    }
    return sys_renameat(regs);
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
    if (r == -4) {
        if (!(flags & TIMER_ABSTIME_K)) *write_rem = 1;
        /* See sys_nanosleep: -EINTR here would be restarted with the original
         * request under an SA_RESTART handler. */
        return -ERESTARTNOHAND;
    }
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

/* ── getresuid / getresgid — EAX=165/171 (16-bit), 209/211 (32-bit) ───────
 * Report real, effective and saved ids.  `wide` is the id size in bytes. */
static int put_res_ids(registers_t *regs, uint32_t r, uint32_t e, uint32_t sv,
                       uint32_t wide) {
    uint32_t ids[3] = { r, e, sv };
    uint32_t ptrs[3] = { regs->ebx, regs->ecx, regs->edx };
    for (int i = 0; i < 3; i++) {
        void *up = (void *)(uintptr_t)ptrs[i];
        int cr;
        if (wide == 4) {
            cr = copy_to_user(up, &ids[i], 4);
        } else {
            uint16_t v = ids[i] > 0xFFFFU ? 65534U : (uint16_t)ids[i];  /* overflowuid */
            cr = copy_to_user(up, &v, 2);
        }
        if (cr < 0) return -14;
    }
    return 0;
}
static int sys_getresuid32(registers_t *regs) {
    struct proc *p = current_proc;
    return put_res_ids(regs, p->uid, p->euid, p->suid, 4);
}
static int sys_getresgid32(registers_t *regs) {
    struct proc *p = current_proc;
    return put_res_ids(regs, p->gid, p->egid, p->sgid, 4);
}
static int sys_getresuid16(registers_t *regs) {
    struct proc *p = current_proc;
    return put_res_ids(regs, p->uid, p->euid, p->suid, 2);
}
static int sys_getresgid16(registers_t *regs) {
    struct proc *p = current_proc;
    return put_res_ids(regs, p->gid, p->egid, p->sgid, 2);
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
 * Firefox checks free space / fs type for its profile + cache.  Report the real
 * numbers when an ext2 filesystem is mounted, so df agrees with dumpe2fs and a
 * program can actually observe space being consumed and released; fall back to
 * the old roomy constants when nothing is mounted (initrd-only boots, where
 * there is no block accounting to report). */
/* f_type for a canonical path: /proc and /dev (and /tmp outside a chroot,
 * where it is the tmpfs) are not the disk.  apk statfs()es <root>/proc and
 * mounts procfs there itself unless it reads PROC_SUPER_MAGIC. */
static uint32_t statfs_magic(const char *path) {
    char first[256];
    uint32_t i = 0;
    const char *p = path;
    while (*p == '/') p++;
    while (p[i] && p[i] != '/' && i < sizeof(first) - 1) { first[i] = p[i]; i++; }
    first[i] = '\0';
    if (__builtin_strcmp(first, "proc") == 0) return 0x9FA0;   /* PROC_SUPER_MAGIC */
    if (__builtin_strcmp(first, "dev") == 0 ||
        (__builtin_strcmp(first, "tmp") == 0 && !current_proc->root_node))
        return 0x01021994;                                       /* TMPFS_MAGIC */
    return 0xEF53;                                               /* EXT2_SUPER_MAGIC */
}

static int sys_statfs64_fill(void *ubuf, uint32_t bufsz, uint32_t magic) {
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
    s.f_type    = magic;
    uint32_t bs, blocks, bfree, inodes, ifree;
    if (ext2_statfs(&bs, &blocks, &bfree, &inodes, &ifree) == 0) {
        s.f_bsize  = bs;
        s.f_frsize = bs;
        s.f_blocks = blocks;
        s.f_bfree  = bfree;
        s.f_bavail = bfree;
        s.f_files  = inodes;
        s.f_ffree  = ifree;
    } else {
        s.f_bsize  = 4096;
        s.f_frsize = 4096;
        s.f_blocks = 256 * 1024;      /* ~1 GiB */
        s.f_bfree  = 192 * 1024;
        s.f_bavail = 192 * 1024;
        s.f_files  = 65536;
        s.f_ffree  = 60000;
    }
    s.f_namelen = 255;
    if (!access_ok(ubuf, bufsz)) return -14;
    uint32_t n = bufsz < sizeof(s) ? bufsz : sizeof(s);
    if (copy_to_user(ubuf, &s, n) < 0) return -14;
    return 0;
}
static int sys_statfs64(registers_t *regs) {
    char path[256];
    if (copy_user_str((const char *)(uintptr_t)regs->ebx, path, 256) < 0) return -14;
    char resolved[256];
    int r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    int err;
    if (!vfs_lookup(resolved, 1, &err)) return err;
    return sys_statfs64_fill((void *)(uintptr_t)regs->edx, regs->ecx,
                             statfs_magic(resolved));
}
static int sys_fstatfs64(registers_t *regs) {
    int fd = (int)regs->ebx;
    if (fd < 0 || fd >= MAX_FD || current_proc->ofile[fd].type == FD_NONE)
        return -9;                                             /* -EBADF */
    const char *path = current_proc->ofile[fd].path;
    return sys_statfs64_fill((void *)(uintptr_t)regs->edx, regs->ecx,
                             path[0] == '/' ? statfs_magic(path) : 0xEF53);
}

/* ── sys_memfd_create(name, flags) — EAX=356 ────────────────────────────────
 * Firefox/Chromium IPC use memfd_create for shared memory.  We back it with an
 * anonymous file in the /tmp tmpfs: a real fd that supports ftruncate + mmap.
 * (Cross-process sharing via SCM_RIGHTS fd-passing is a separate step; this
 * makes the single-process path work so the parent stops aborting on shm.) */
static int sys_open_kernel_path(const char *path, int flags, uint32_t mode);
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
    int fd = sys_open_kernel_path(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
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
/* Linux kernel/sys.c setpgid(): the target is the caller or one of its
 * children (else -ESRCH); a child must still be in the caller's session
 * (-EPERM) and must not have exec'd yet (-EACCES); a session leader cannot
 * move (-EPERM); and joining a group other than its own pid requires that
 * group to exist in the caller's session (-EPERM). */
static int sys_setpgid(registers_t *regs) {
    int pid  = (int)regs->ebx;
    int pgid = (int)regs->ecx;
    struct proc *me = current_proc;
    if (pgid < 0) return -22;   /* -EINVAL */
    struct proc *p = (pid == 0) ? me : proc_find_by_pid(pid);
    if (p) p = proc_group_leader(p);
    if (!p) return -3;           /* -ESRCH */
    if (p->tgid != me->tgid) {
        if (!p->parent || p->parent->tgid != me->tgid) return -3;  /* -ESRCH */
        if (p->sid != me->sid) return -1;                          /* -EPERM */
        if (p->did_exec) return -13;                               /* -EACCES */
    }
    if (p->sid == p->tgid) return -1;          /* session leader: -EPERM */
    if (pgid == 0) pgid = p->tgid;
    if (pgid != p->tgid) {
        int found = 0;
        /* A zombie is still a member until it is reaped (Linux keeps its
         * pid attached): a pipeline's later stages join the group of a first
         * stage that may already have exited. */
        for (int i = 0; i < MAX_PROCS && !found; i++) {
            struct proc *q = &ptable[i];
            if (q->state == PROC_UNUSED) continue;
            if (q->pgrp == pgid && q->sid == me->sid) found = 1;
        }
        if (!found) return -1;                 /* -EPERM */
    }
    /* Every thread of the process carries the process's group. */
    for (int i = 0; i < MAX_PROCS; i++)
        if (ptable[i].state != PROC_UNUSED && ptable[i].tgid == p->tgid)
            ptable[i].pgrp = pgid;
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

/* ── sys_getsid(pid) — EAX=147 ──────────────────────────────────────────── */
static int sys_getsid(registers_t *regs) {
    int pid = (int)regs->ebx;
    struct proc *p = (pid == 0) ? current_proc : proc_find_by_pid(pid);
    if (!p) return -3;   /* -ESRCH */
    return p->sid;
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
        if (!kill_permitted(p, sig)) return -1;       /* -EPERM */
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
/* readv/writev on a SEQPACKET/DGRAM AF_UNIX socket: one record over the whole
 * iovec array (defined with the AF_UNIX glue below); -1 when fd is not one. */
static int usock_rw_record(int fd, uint32_t uiov, int iovcnt, int write);

/* One iovec segment of a readv/writev after earlier segments already moved
 * data.  Linux runs the whole iovec through one read_iter/write_iter pass, so
 * a pipe or socket read returns as soon as it has something (it never sleeps
 * for a later segment) and a socket write raises SIGPIPE only when the call
 * sent nothing.  -EAGAIN here just ends the call.  Returns -1000 for a
 * descriptor that needs no special handling (plain sys_read/sys_write). */
#define IOV_SEG_PLAIN (-1000)
static int iov_seg_more(int fd, void *base, int len, int write) {
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type == FD_PIPE_R && !write)
        return pipe_read(f->pipe, base, len, 1);
    if (f->type == FD_SOCKET)
        return write ? sock_send_user(f->socket, base, (uint32_t)len, NULL,
                                      SOCK_MSG_NOSIGNAL |
                                      ((f->flags & O_NONBLOCK) ? NET_MSG_DONTWAIT : 0))
                     : sock_recv_user(f->socket, base, (uint32_t)len, NULL,
                                      NET_MSG_DONTWAIT);
    if (f->type == FD_USOCKET) {
        usocket_t *us = f->usock;
        int fl = write ? (USOCK_NOSIGNAL | ((f->flags & O_NONBLOCK) ? USOCK_NONBLOCK : 0))
                       : USOCK_NONBLOCK;
        usocket_pin(us);
        int r = write ? usocket_write(us, base, len, fl) : usocket_read(us, base, len, fl);
        usocket_unpin(us);
        return r;
    }
    /* Anything else that can block a read (a tty, a device, an eventfd):
     * stop at the data already read unless more is ready now. */
    if (!write && f->type != FD_FILE) return fd_read_ready(fd) ? IOV_SEG_PLAIN : -11;
    if (!write && f->node && !node_is_regular(f->node) && !fd_read_ready(fd))
        return -11;
    return IOV_SEG_PLAIN;
}

static int sys_readv(registers_t *regs) {
    int fd     = (int)regs->ebx;
    const uint32_t *iov = (const uint32_t *)(uintptr_t)regs->ecx;
    int iovcnt = (int)regs->edx;

    if (iovcnt < 0 || iovcnt > 1024) return -22;
    if (!access_ok(iov, (size_t)iovcnt * 8)) return -14;
    if (fd < 0 || fd >= MAX_FD) return -9;
    int rr = usock_rw_record(fd, regs->ecx, iovcnt, 0);
    if (rr != -1) return rr;

    int total = 0;
    for (int i = 0; i < iovcnt; i++) {
        uint32_t kiov[2];
        int cr = copy_from_user(kiov, &iov[i * 2], sizeof(kiov));
        if (cr < 0) return cr;
        char *base = (char *)(uintptr_t)kiov[0];
        int   len  = (int)kiov[1];
        if (len <= 0) continue;
        if (!access_ok(base, (size_t)len)) return -14;
        int n = total ? iov_seg_more(fd, base, len, 0) : IOV_SEG_PLAIN;
        if (n == IOV_SEG_PLAIN) {
            registers_t fake = *regs;
            fake.ebx = (uint32_t)fd;
            fake.ecx = (uint32_t)(uintptr_t)base;
            fake.edx = (uint32_t)len;
            n = sys_read(&fake);
        }
        /* Bytes already read are consumed: report them, not a later iovec's
         * EAGAIN/EFAULT, or they are lost to the caller. */
        if (n < 0) return total ? total : n;
        total += n;
        if (n < len) break;  /* short read — don't continue */
    }
    return total;
}

/* ── sys_flock(fd, operation) — EAX=143 ─────────────────────────────────── */
static int sys_flock(registers_t *regs) {
    int fd = (int)regs->ebx;
    int op = (int)regs->ecx;
    if (fd < 0 || fd >= MAX_FD) return -9;
    if (current_proc->ofile[fd].type == FD_NONE) return -9;
    return flock_bsd(&current_proc->ofile[fd], op);
}

/* ── sys__llseek(fd, off_high, off_low, loff_t *result, whence) — EAX=140 ─ */
static int sys_llseek(registers_t *regs) {
    int      fd       = (int)regs->ebx;
    int64_t  off      = (int64_t)(((uint64_t)regs->ecx << 32) | regs->edx);
    uint64_t *result  = (uint64_t *)(uintptr_t)regs->esi;
    int       whence  = (int)regs->edi;

    if (fd < 0 || fd >= MAX_FD) return -9;
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type == FD_NONE) return -9;
    if (f->type == FD_PIPE_R || f->type == FD_PIPE_W) return -29;

    uint32_t new_off;
    int r = seek_target(f, off, whence, &new_off);
    if (r < 0) return r;
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
    /* i386 struct sysinfo is 64 bytes: the same uptime and memory figures as
     * /proc/uptime and /proc/meminfo, in 4 KiB units (mem_unit), no swap. */
    uint32_t *si = (uint32_t *)(uintptr_t)regs->ebx;
    if (!si || !access_ok(si, 64)) return -14;
    uint32_t ksi[16];
    __builtin_memset(ksi, 0, sizeof(ksi));
    ksi[0] = pit_ticks() / 100;     /* uptime seconds */
    ksi[4] = pmm_ram_frames();      /* totalram */
    ksi[5] = pmm_free_frames();     /* freeram */
    uint32_t procs = 0;
    for (int i = 0; i < MAX_PROCS; i++)
        if (ptable[i].state != PROC_UNUSED) procs++;
    ksi[10] = procs & 0xFFFF;       /* u16 procs, then 2 bytes pad */
    ksi[11] = pmm_high_frames();    /* totalhigh: above 4 GiB, user pages only */
    ksi[12] = pmm_high_free_frames();   /* freehigh */
    ksi[13] = 4096;                 /* mem_unit (after totalhigh, freehigh) */
    return copy_to_user(si, ksi, sizeof(ksi));
}

/* Resolve the physical address of a user futex word (page + offset), or 0 if the
 * page is not present.  Used to key process-SHARED futexes by physical page so a
 * cross-process (shm) primitive matches across address spaces, exactly like Linux
 * get_futex_key for the shared case. */
static uint32_t futex_resolve_phys(uint32_t uaddr) {
    uint32_t page = uaddr & ~0xFFFU;
    if (page >= 0xC0000000U) return 0;              /* kernel space — not a futex */
    if (!pde_present(page)) return 0;
    pte_t pte = pte_get(page);
    if (!(pte & PAGE_PRESENT)) return 0;
    /* The key is the physical address shifted right by 2: futex words are
     * 4-byte aligned, so nothing is lost, and a frame above 4 GiB (PAE, up to
     * 16 GiB) still gets a key of its own in 32 bits.  Never 0: frame 0 is
     * not RAM a process can map. */
    return (uint32_t)((pte_frame(pte) | (uaddr & 0xFFFU)) >> 2);
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

    /* Linux: GRND_NONBLOCK(1)|GRND_RANDOM(2)|GRND_INSECURE(4), but INSECURE
     * and RANDOM together are -EINVAL.  The pool is always initialised here,
     * so none of them changes what is returned. */
    if (flags & ~7U) return -22;
    if ((flags & 6U) == 6U) return -22;
    /* Linux caps one call at MAX_RW_COUNT, so the count fits the return. */
    if (buflen > 0x7FFFF000U) buflen = 0x7FFFF000U;
    if (!access_ok(buf, buflen)) return -14;

    uint32_t done = 0;
    while (done < buflen) {
        uint32_t chunk = buflen - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        random_get_bytes(tmp, chunk);
        int cr = copy_to_user(buf + done, tmp, chunk);
        __builtin_memset(tmp, 0, sizeof(tmp));
        if (cr < 0) return done ? (int)done : cr;     /* partial: bytes so far */
        done += chunk;
    }
    return (int)done;
}

/* ── sys_rename(oldpath, newpath) — EAX=38 ────────────────────────────────
 * Linux vfs_rename(): the source must be removable from its directory and
 * the target name creatable (or, when it exists, removable) in the new one,
 * both under the sticky rule; a directory moving to a new parent also needs
 * write permission on itself (its ".." changes).  The move itself is the
 * filesystem's rename_fn, which replaces an existing target atomically and
 * keeps the source's inode — its mode, owner and contents — so passwd's
 * write-temp-then-rename of /etc/shadow can never leave the file missing,
 * half-written or owned by someone else.  Across filesystems it is -EXDEV
 * (mv copies in that case). */
static int sys_rename_kernel_path(const char *oldpath, const char *newpath) {
    char old_dir[256], old_base[256];
    if (vfs_path_rdonly(oldpath, 1) || vfs_path_rdonly(newpath, 1))
        return -30;                                           /* -EROFS */
    char new_dir[256], new_base[256];
    if (path_split(oldpath, old_dir, old_base) < 0) return -2;
    if (path_split(newpath, new_dir, new_base) < 0) return -2;
    if (old_base[0] == '\0' || new_base[0] == '\0') return -22;

    vfs_node_t *src_dir = vfs_open_parent_at(oldpath, old_dir);
    vfs_node_t *dst_dir = vfs_open_parent_at(newpath, new_dir);
    if (!src_dir || !dst_dir) return -2;
    if (src_dir->flags != VFS_FLAG_DIR || dst_dir->flags != VFS_FLAG_DIR)
        return -20;                                      /* -ENOTDIR */

    vfs_node_t *src = vfs_finddir(src_dir, old_base);
    if (!src) return -2;                                 /* -ENOENT */
    int src_is_dir = (src->flags == VFS_FLAG_DIR);

    /* A directory cannot become its own descendant. */
    uint32_t olen = (uint32_t)__builtin_strlen(oldpath);
    if (src_is_dir && __builtin_strncmp(newpath, oldpath, olen) == 0 &&
        newpath[olen] == '/')
        return -22;                                      /* -EINVAL */

    int r = may_delete(src_dir, src);
    if (r < 0) return r;
    vfs_node_t *dst = vfs_finddir(dst_dir, new_base);
    if (dst) {
        r = may_delete(dst_dir, dst);
        if (r < 0) return r;
    } else if (proc_access_check(dst_dir,
                                VFS_WANT_W | VFS_WANT_X) < 0) {
        return -13;                                      /* -EACCES */
    }
    if (src_is_dir && __builtin_strcmp(old_dir, new_dir) != 0 &&
        proc_access_check(src, VFS_WANT_W) < 0)
        return -13;

    return vfs_rename(src_dir, old_base, dst_dir, new_base);
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

/* ── link(oldpath, newpath) — EAX=9, linkat(...) — EAX=303 ─────────────────
 * Linux vfs_link(): the new name must not exist and its directory must be
 * writable; the old path names the object itself (a symlink, unless
 * AT_SYMLINK_FOLLOW), never a directory; both on one filesystem (-EXDEV).
 * ext2 has hard links; tmpfs/initrd answer -EPERM, as a filesystem without
 * them does on Linux. */
static int sys_link_paths(int olddirfd, const char *uold, int newdirfd,
                          const char *unew, int flags) {
    if (flags & ~(0x400 | 0x1000)) return -22;  /* AT_SYMLINK_FOLLOW, AT_EMPTY_PATH */
    char oldpath[256], newpath[256], oldres[256], newres[256];
    int r = copy_user_str(uold, oldpath, sizeof(oldpath));
    if (r < 0) return r;
    r = copy_user_str(unew, newpath, sizeof(newpath));
    if (r < 0) return r;
    vfs_node_t *target;
    if (!oldpath[0]) {
        if (!(flags & 0x1000)) return -2;
        if (olddirfd < 0 || olddirfd >= MAX_FD ||
            current_proc->ofile[olddirfd].type != FD_FILE ||
            !current_proc->ofile[olddirfd].node)
            return -9;
        target = current_proc->ofile[olddirfd].node;
    } else {
        r = resolve_path_at_fd(olddirfd, oldpath, oldres, sizeof(oldres));
        if (r < 0) return r;
        int err;
        target = vfs_lookup(oldres, (flags & 0x400) != 0, &err);
        if (!target) return err;
    }
    if (target->flags == VFS_FLAG_DIR) return -1;              /* -EPERM */
    r = resolve_path_at_fd(newdirfd, newpath, newres, sizeof(newres));
    if (r < 0) return r;
    /* The new name goes into the directory that would hold it. */
    if (vfs_path_rdonly(newres, 1)) return -30;                /* -EROFS */
    char new_dir[256], new_base[256];
    if (path_split(newres, new_dir, new_base) < 0 || !new_base[0]) return -2;
    vfs_node_t *dir = vfs_open_parent_at(newres, new_dir);
    if (!dir) return -2;
    if (dir->flags != VFS_FLAG_DIR) return -20;                /* -ENOTDIR */
    if (vfs_finddir(dir, new_base)) return -17;                /* -EEXIST */
    if (proc_access_check(dir, VFS_WANT_W | VFS_WANT_X) < 0) return -13;
    return vfs_link(dir, new_base, target);
}

/* ── chmod / fchmod / chown / fchown / lchown ───────────────────────────── */
/* chmod: only the file owner or root may change the mode.  An owner who is
 * not in the file's group cannot set its set-group-ID bit: it is silently
 * dropped (Linux setattr_prepare / setattr_should_drop_sgid). */
static int do_chmod_node(vfs_node_t *n, uint32_t mode) {
    if (!n) return -2;
    if (current_proc->euid != 0 && current_proc->euid != n->uid)
        return -1;  /* -EPERM */
    mode &= 07777;
    if ((mode & 02000) && current_proc->euid != 0 && !proc_in_group(n->gid))
        mode &= ~02000u;
    return vfs_setattr(n, mode, n->uid, n->gid);
}

/* chown: changing the owning uid is root-only; the owner may chgrp only to
 * a group it belongs to (Linux chown_ok/chgrp_ok).  uid/gid of -1
 * (0xFFFFFFFF) means "unchanged". */
static int do_chown_node(vfs_node_t *n, uint32_t uid, uint32_t gid) {
    if (!n) return -2;
    uint32_t new_uid = n->uid, new_gid = n->gid;
    if (uid != 0xFFFFFFFFU && uid != n->uid) {
        if (current_proc->euid != 0) return -1; /* -EPERM */
        new_uid = uid;
    }
    if (gid != 0xFFFFFFFFU && gid != n->gid) {
        if (current_proc->euid != 0 &&
            (current_proc->euid != n->uid || !proc_in_group(gid)))
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
    if (vfs_path_rdonly(resolved, 0)) return -30;          /* -EROFS */
    return do_chmod_node(vfs_open(resolved), (uint32_t)regs->ecx);
}

static int sys_fchmod(registers_t *regs) {
    int fd = (int)regs->ebx;
    if (fd < 0 || fd >= MAX_FD) return -9;
    if (current_proc->ofile[fd].type == FD_NONE) return -9;
    if (fd_rofs(&current_proc->ofile[fd])) return -30;     /* -EROFS */
    return do_chmod_node(current_proc->ofile[fd].node, (uint32_t)regs->ecx);
}

static int sys_chown(registers_t *regs) {
    char path[256], resolved[256];
    int r = copy_user_str((const char *)(uintptr_t)regs->ebx, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    if (vfs_path_rdonly(resolved, 0)) return -30;          /* -EROFS */
    return do_chown_node(vfs_open(resolved), (uint32_t)regs->ecx,
                         (uint32_t)regs->edx);
}

static int sys_fchown(registers_t *regs) {
    int fd = (int)regs->ebx;
    if (fd < 0 || fd >= MAX_FD) return -9;
    if (current_proc->ofile[fd].type == FD_NONE) return -9;
    if (fd_rofs(&current_proc->ofile[fd])) return -30;     /* -EROFS */
    return do_chown_node(current_proc->ofile[fd].node, (uint32_t)regs->ecx,
                         (uint32_t)regs->edx);
}

static int sys_lchown(registers_t *regs) {
    char path[256], resolved[256];
    int r = copy_user_str((const char *)(uintptr_t)regs->ebx, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    if (vfs_path_rdonly(resolved, 1)) return -30;          /* -EROFS */
    return do_chown_node(vfs_open_nofollow(resolved), (uint32_t)regs->ecx,
                         (uint32_t)regs->edx);
}

/* ── sys_fchownat(dirfd, path, owner, group, flags) — EAX=298 ────────────── */
static int sys_fchownat(registers_t *regs) {
    char path[256], resolved[256];
    int flags = (int)regs->edi;
    if (flags & ~(0x100 | 0x1000)) return -22;   /* AT_SYMLINK_NOFOLLOW, AT_EMPTY_PATH */
    int r = copy_user_str((const char *)(uintptr_t)regs->ecx, path, sizeof(path));
    if (r < 0) return r;
    if (!path[0]) {
        int fd = (int)regs->ebx;
        if (!(flags & 0x1000)) return -2;
        if (fd < 0 || fd >= MAX_FD || current_proc->ofile[fd].type == FD_NONE)
            return -9;
        if (fd_rofs(&current_proc->ofile[fd])) return -30;  /* -EROFS */
        return do_chown_node(current_proc->ofile[fd].node, (uint32_t)regs->edx,
                             (uint32_t)regs->esi);
    }
    r = resolve_path_at_fd((int)regs->ebx, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    if (vfs_path_rdonly(resolved, (flags & 0x100) != 0)) return -30;
    vfs_node_t *n = (flags & 0x100) ? vfs_open_nofollow(resolved) : vfs_open(resolved);
    return do_chown_node(n, (uint32_t)regs->edx, (uint32_t)regs->esi);
}

static int sys_fchmodat(registers_t *regs) {
    int dirfd = (int)regs->ebx;
    const char *upath = (const char *)(uintptr_t)regs->ecx;
    char path[256], resolved[256];
    int r = copy_user_str(upath, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    if (vfs_path_rdonly(resolved, 0)) return -30;          /* -EROFS */
    return do_chmod_node(vfs_open(resolved), (uint32_t)regs->edx);
}

/* ── utime(30), utimes(271), futimesat(299), utimensat(320/412) ─────────── */
/* All five come down to do_utimens(): `have` clear means "both now" (a NULL
 * times pointer); otherwise ns[i] may be UTIME_NOW or UTIME_OMIT.  Setting
 * explicit times needs ownership (or root); setting both to now also accepts
 * write access, as on Linux. */
#define UTIME_NOW_K  ((1u << 30) - 1)
#define UTIME_OMIT_K ((1u << 30) - 2)

static int do_utimens(vfs_node_t *n, int have, const int64_t sec[2],
                      const uint32_t ns[2]) {
    if (!n) return -2;
    if (have) {
        for (int i = 0; i < 2; i++)
            if (ns[i] >= 1000000000u && ns[i] != UTIME_NOW_K &&
                ns[i] != UTIME_OMIT_K)
                return -22;                                    /* -EINVAL */
        if (ns[0] == UTIME_OMIT_K && ns[1] == UTIME_OMIT_K) return 0;
    }
    int only_now = !have || (ns[0] == UTIME_NOW_K && ns[1] == UTIME_NOW_K);
    struct proc *p = current_proc;
    if (p->euid != 0 && p->euid != n->uid) {
        if (!only_now) return -1;                              /* -EPERM */
        if (proc_access_check(n, VFS_WANT_W) < 0) return -13;  /* -EACCES */
    }
    int64_t now; uint32_t now_ns;
    kclock_get(CLK_REALTIME_K, &now, &now_ns);
    uint32_t t[2] = { n->atime, n->mtime };
    for (int i = 0; i < 2; i++) {
        if (!have || ns[i] == UTIME_NOW_K) t[i] = (uint32_t)now;
        else if (ns[i] != UTIME_OMIT_K)    t[i] = (uint32_t)sec[i];
    }
    return vfs_settimes(n, t[0], t[1]) < 0 ? -5 : 0;           /* -EIO */
}

/* The node utimensat()-style calls act on: `upath` under `dirfd`, or with a
 * NULL or (AT_EMPTY_PATH) empty path the descriptor itself.  -EROFS when it
 * is on a read-only mount, like chmod and chown. */
static int utimens_target(int dirfd, const char *upath, int flags,
                          vfs_node_t **out) {
    if (flags & ~(0x100 | 0x1000)) return -22;  /* AT_SYMLINK_NOFOLLOW, AT_EMPTY_PATH */
    char path[256], resolved[256];
    if (upath) {
        int r = copy_user_str(upath, path, sizeof(path));
        if (r < 0) return r;
    }
    if (!upath || (!path[0] && (flags & 0x1000))) {
        if (dirfd < 0 || dirfd >= MAX_FD ||
            current_proc->ofile[dirfd].type == FD_NONE || !current_proc->ofile[dirfd].node)
            return -9;                                         /* -EBADF */
        if (fd_rofs(&current_proc->ofile[dirfd])) return -30;  /* -EROFS */
        *out = current_proc->ofile[dirfd].node;
        return 0;
    }
    if (!path[0]) return -2;
    int r = resolve_path_at_fd(dirfd, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    int err;
    *out = vfs_lookup(resolved, !(flags & 0x100), &err);
    if (!*out) return err;
    if (vfs_path_rdonly(resolved, (flags & 0x100) != 0)) return -30;  /* -EROFS */
    return 0;
}

static int sys_utimensat_common(registers_t *regs, int time64) {
    vfs_node_t *n;
    int r = utimens_target((int)regs->ebx, (const char *)(uintptr_t)regs->ecx,
                           (int)regs->esi, &n);
    if (r < 0) return r;
    int64_t sec[2] = { 0, 0 };
    uint32_t ns[2] = { 0, 0 };
    const void *ut = (const void *)(uintptr_t)regs->edx;
    if (ut) {
        if (time64) {
            int64_t k[4];
            if (copy_from_user(k, ut, sizeof(k)) < 0) return -14;
            sec[0] = k[0]; ns[0] = (uint32_t)k[1];
            sec[1] = k[2]; ns[1] = (uint32_t)k[3];
        } else {
            int32_t k[4];
            if (copy_from_user(k, ut, sizeof(k)) < 0) return -14;
            sec[0] = k[0]; ns[0] = (uint32_t)k[1];
            sec[1] = k[2]; ns[1] = (uint32_t)k[3];
        }
    }
    return do_utimens(n, ut != NULL, sec, ns);
}

/* futimesat(dirfd, path, timeval[2]) and utimes(path, timeval[2]). */
static int do_futimesat(int dirfd, const char *upath, const void *utv) {
    vfs_node_t *n;
    int r = utimens_target(dirfd, upath, 0, &n);
    if (r < 0) return r;
    int64_t sec[2] = { 0, 0 };
    uint32_t ns[2] = { 0, 0 };
    if (utv) {
        int32_t k[4];
        if (copy_from_user(k, utv, sizeof(k)) < 0) return -14;
        for (int i = 0; i < 2; i++) {
            if (k[2 * i + 1] < 0 || k[2 * i + 1] >= 1000000) return -22;
            sec[i] = k[2 * i];
            ns[i] = (uint32_t)k[2 * i + 1] * 1000u;
        }
    }
    return do_utimens(n, utv != NULL, sec, ns);
}

static int sys_utime(registers_t *regs) {
    const char *upath = (const char *)(uintptr_t)regs->ebx;
    const void *ub = (const void *)(uintptr_t)regs->ecx;
    if (!upath) return -14;
    vfs_node_t *n;
    int r = utimens_target(AT_FDCWD, upath, 0, &n);
    if (r < 0) return r;
    int64_t sec[2] = { 0, 0 };
    uint32_t ns[2] = { 0, 0 };
    if (ub) {
        int32_t k[2];                       /* struct utimbuf { actime, modtime } */
        if (copy_from_user(k, ub, sizeof(k)) < 0) return -14;
        sec[0] = k[0]; sec[1] = k[1];
    }
    return do_utimens(n, ub != NULL, sec, ns);
}

/* ── xattr family, EAX=226..237 ─────────────────────────────────────────── */
/* No filesystem here stores extended attributes: after the usual path or fd
 * checks every call fails with -EOPNOTSUPP, which is what Linux returns on a
 * filesystem without xattr support, and what coreutils (ls, cp) and apk
 * treat as "no ACLs / labels" instead of an error. */
static int sys_xattr(registers_t *regs, int nr) {
    int kind = (nr - 226) % 3;                 /* 0 path, 1 lpath, 2 fd */
    if (kind == 2) {
        int fd = (int)regs->ebx;
        if (fd < 0 || fd >= MAX_FD || current_proc->ofile[fd].type == FD_NONE)
            return -9;                                         /* -EBADF */
        return -95;                                            /* -EOPNOTSUPP */
    }
    char path[256], resolved[256];
    int r = copy_user_str((const char *)(uintptr_t)regs->ebx, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    int err;
    if (!vfs_lookup(resolved, kind == 0, &err)) return err;
    return -95;                                                /* -EOPNOTSUPP */
}

/* ── mount(2) / umount(2) / umount2(2) — EAX=21 / 22 / 52 ──────────────────
 * mount(source, target, fstype, flags, data).  Privileged (euid 0), as on
 * Linux without user namespaces.  The work is in fs/mount.c. */
static int copy_user_str_opt(uint32_t uptr, char *kbuf, int max, const char **out) {
    *out = NULL;
    if (!uptr) return 0;
    int r = copy_user_str((const char *)(uintptr_t)uptr, kbuf, max);
    if (r < 0) return r;
    *out = kbuf;
    return 0;
}

static int sys_mount(registers_t *regs) {
    if (current_proc->euid != 0) return -1;                  /* -EPERM */
    char src[256], tgt[256], typ[32], data[256], target[256];
    const char *psrc, *ptgt, *ptyp, *pdata;
    int r;
    if ((r = copy_user_str_opt(regs->ebx, src, sizeof(src), &psrc)) < 0) return r;
    if ((r = copy_user_str_opt(regs->ecx, tgt, sizeof(tgt), &ptgt)) < 0) return r;
    if ((r = copy_user_str_opt(regs->edx, typ, sizeof(typ), &ptyp)) < 0) return r;
    if ((r = copy_user_str_opt(regs->edi, data, sizeof(data), &pdata)) < 0) return r;
    if (!ptgt) return -14;                                   /* -EFAULT */
    r = resolve_path_at_fd(AT_FDCWD, ptgt, target, sizeof(target));
    if (r < 0) return r;
    /* A source naming a path (block device, bind source) is resolved like
     * any other; a pseudo-filesystem's source ("none", "proc") is a label. */
    char srcabs[256];
    if (psrc && (psrc[0] == '/' || psrc[0] == '.') &&
        resolve_path_at_fd(AT_FDCWD, psrc, srcabs, sizeof(srcabs)) == 0)
        psrc = srcabs;
    return mount_do(psrc, target, ptyp, (uint32_t)regs->esi, pdata);
}

static int sys_umount2(registers_t *regs, uint32_t flags) {
    if (current_proc->euid != 0) return -1;                  /* -EPERM */
    char path[256], target[256];
    int r = copy_user_str((const char *)(uintptr_t)regs->ebx, path, sizeof(path));
    if (r < 0) return r;
    r = resolve_path_at_fd(AT_FDCWD, path, target, sizeof(target));
    if (r < 0) return r;
    return umount_do(target, flags);
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
    int rr = usock_rw_record(fd, regs->ecx, iovcnt, 1);
    if (rr != -1) return rr;

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
        int n = total ? iov_seg_more(fd, (void *)base, len, 1) : IOV_SEG_PLAIN;
        if (n == IOV_SEG_PLAIN) {
            registers_t fake = *regs;
            fake.ebx = (uint32_t)fd;
            fake.ecx = (uint32_t)(uintptr_t)base;
            fake.edx = (uint32_t)len;
            n = sys_write(&fake);
        }
        /* A short write ends the call, and bytes already written are what it
         * returns (fs/read_write.c do_loop_readv_writev).  Carrying on to the
         * next iovec after a short one dropped the rest of that one from the
         * stream; returning a later iovec's EAGAIN after a partial write made
         * the caller send those bytes again.  libxcb writes [header][image]
         * non-blocking, so a PutImage bigger than the 64 KiB AF_UNIX ring
         * corrupted the X stream this way whenever maeroX read it promptly. */
        if (n < 0) return total ? total : n;
        total += n;
        if (n < len) break;
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
        signal_retarget_shared(current_proc, m & ~current_proc->blocked_sigs);
        current_proc->blocked_sigs = m;
    }

    /* Linux set_restore_sigmask(): stash the caller's mask instead of putting
     * it back here, and let the signal-return path reinstate it once it has
     * decided what to deliver (kernel/signal.c sigsuspend + signal_delivered).
     * Restoring it here would re-block the very signal this call is waiting
     * for, so signal_return_to_user() would see nothing deliverable, take the
     * `restartable` path for the -ERESTARTNOHAND below, and re-enter this
     * syscall — which finds the signal pending under the temporary mask, does
     * not sleep, and returns -ERESTARTNOHAND again: an unbreakable spin in
     * which the handler never runs. */
    current_proc->saved_sigmask   = old_mask;
    current_proc->restore_sigmask = 1;

    /* Sleep until a DELIVERABLE signal wakes us (signal_send only wakes for
     * those); if one is already pending under the new mask, do not sleep.
     * Linux sigsuspend returns -ERESTARTNOHAND: EINTR once a handler has run,
     * a transparent restart if the signal turned out to do nothing. */
    if (!signal_interrupt_pending(current_proc))
        sleep_on((void *)&sys_rt_sigsuspend);

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
    proc_copy_image_ids(child, parent);
    child->pending_sigs  = 0;
    child->blocked_sigs  = parent->blocked_sigs;
    child->sigframe_addr = 0;
    /* The alternate signal stack is per TASK and must not be shared with one
     * that shares the address space: both would build their SA_ONSTACK frames
     * at the same address, and the second to arrive would overwrite the saved
     * registers, ucontext and trampoline of a handler already running there.
     * Linux clears it on exactly this condition (kernel/fork.c copy_process:
     * sas_ss_reset(p) when (clone_flags & (CLONE_VM|CLONE_VFORK)) == CLONE_VM).
     * A vfork child keeps it: the parent is suspended until the child execs —
     * which clears it — or exits, so there is never a second user.  (A real
     * fork inherits it; that is do_fork's copy, not this one.) */
    if (flags & CLONE_VFORK) {
        child->sas_sp   = parent->sas_sp;
        child->sas_size = parent->sas_size;
    } else {
        child->sas_sp   = 0;
        child->sas_size = 0;
    }
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
        __builtin_memcpy(child->sighand->mask, parent->sighand->mask,
                         sizeof(child->sighand->mask));
    }
    __builtin_memcpy(child->cwd, parent->cwd, sizeof(parent->cwd));
    child->root_node = parent->root_node;
    if (child->root_node) vfs_retain(child->root_node);
    child->heap_end  = parent->heap_end;
    child->umask     = parent->umask;
    child->uid = parent->uid; child->gid = parent->gid;
    child->euid = parent->euid; child->egid = parent->egid;
    child->suid = parent->suid; child->sgid = parent->sgid;
    child->ngroups = parent->ngroups;
    __builtin_memcpy(child->groups, parent->groups, sizeof(child->groups));
    child->mmap_next = parent->mmap_next;
    child->pgrp      = parent->pgrp;
    child->sid       = parent->sid;
    child->ctty      = parent->ctty;
    if (child->ctty)
        vfs_retain(child->ctty);

    child->read_implies_exec = parent->read_implies_exec;
    child->exec_stack        = parent->exec_stack;
    child->pgdir_phys = parent->pgdir_phys;
    pgdir_retain(child->pgdir_phys);

    if (flags & CLONE_THREAD) {
        /* Same group as the creator; the group's parent is the parent of every
         * thread (copy_process: p->real_parent = current->real_parent), so a
         * thread's exit is never a "child exit" for the creating thread. */
        child->tgid   = parent->tgid;
        child->parent = parent->parent;
        /* One process-wide signal state for the group (copy_signal is skipped
         * under CLONE_THREAD). */
        if (parent->sigshared) {
            sigshared_put(child->sigshared);
            child->sigshared = parent->sigshared;
            child->sigshared->refcount++;
        }
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
     * both the parent's and child's view.  The words are stored with
     * copy_to_user, best effort like Linux's put_user() here: a pointer into a
     * read-only page is ignored, not a ring-0 fault. */
    child->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? uctid : 0;
    {
        uint32_t tid = (uint32_t)child->pid;
        if ((flags & CLONE_PARENT_SETTID) && uptid)
            (void)copy_to_user((void *)(uintptr_t)uptid, &tid, sizeof(tid));
        if ((flags & CLONE_CHILD_SETTID) && uctid)
            (void)copy_to_user((void *)(uintptr_t)uctid, &tid, sizeof(tid));
    }

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
            fd_copy(&child->ofile[i], &parent->ofile[i]);
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

/* ── sys_vfork() — EAX=190 ──────────────────────────────────────────────────
 * clone(CLONE_VM | CLONE_VFORK | SIGCHLD) with the caller's own stack, as
 * Linux implements it: the parent sleeps until the child execs or exits.
 * busybox (udhcpc running its script, ash) calls it. */
static int sys_vfork(registers_t *regs) {
    registers_t fake = *regs;
    fake.ebx = CLONE_VM | CLONE_VFORK | 17;   /* SIGCHLD */
    fake.ecx = regs->useresp;                 /* the caller's stack */
    fake.edx = fake.esi = fake.edi = 0;
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

/* recv/send MSG_* bits this layer honours (include/linux/socket.h). */
#define MSG_PEEK_K          0x0002
#define MSG_DONTWAIT_K      0x0040
#define MSG_NOSIGNAL_K      0x4000
#define MSG_CTRUNC_K        0x0008
#define MSG_CMSG_CLOEXEC_K  0x40000000

/* Per-call MSG_* flags + the descriptor's O_NONBLOCK, as usocket_read()/
 * usocket_write() want them. */
static int usock_flags(uint32_t msgflags, int nb) {
    int f = (nb || (msgflags & MSG_DONTWAIT_K)) ? USOCK_NONBLOCK : 0;
    if (msgflags & MSG_PEEK_K)     f |= USOCK_PEEK;
    if (msgflags & MSG_NOSIGNAL_K) f |= USOCK_NOSIGNAL;
    if (msgflags & 0x0020)         f |= USOCK_TRUNC;     /* MSG_TRUNC */
    return f;
}

/* Undo a queued SCM_RIGHTS batch whose sendmsg is not going to carry it after
 * all (no data written, or the call is failing).  The batch may already have
 * been delivered — a concurrent reader can have taken it — in which case the
 * refs are no longer ours to close. */
static void scm_abort(usocket_t *us, uint32_t id, proc_file_t *pass, int npass) {
    if (npass <= 0 || !id) return;
    if (usocket_cancel_fds(us, id))
        for (int k = 0; k < npass; k++) fd_release(&pass[k]);
}

/* ── AF_UNIX glue ────────────────────────────────────────────────────────── */

#define MSG_TRUNC_K         0x0020

/* socket()/socketpair() type and protocol for AF_UNIX (net/unix/af_unix.c
 * unix_create): SOCK_RAW is taken as SOCK_DGRAM, anything else unknown is
 * -ESOCKTNOSUPPORT, and the protocol must be 0 or PF_UNIX. */
static int usock_type_check(uint32_t rawtype, int protocol) {
    if (rawtype & ~(0xFU | SOCK_NONBLOCK_K | SOCK_CLOEXEC_K)) return -22;
    if (protocol != 0 && protocol != AF_UNIX_K) return -93;  /* -EPROTONOSUPPORT */
    int t = (int)(rawtype & 0xF);
    if (t == 3) t = USOCK_DGRAM;                             /* SOCK_RAW */
    if (t != USOCK_STREAM && t != USOCK_DGRAM && t != USOCK_SEQPACKET)
        return -94;                                          /* -ESOCKTNOSUPPORT */
    return t;
}

/* struct sockaddr_un from user memory.  Returns 1 for an address that names
 * nothing (just the family: autobind / unnamed), 0 for a name, -errno. */
static int usock_name_from_user(uint32_t ua, uint32_t alen, usock_name_t *n) {
    uint8_t kbuf[2 + USOCK_PATH_MAX];
    /* Linux unix_validate_addr: family present, at most sizeof(sockaddr_un). */
    if (alen < 2 || alen > 2 + USOCK_PATH_MAX) return -22;
    if (!ua || !access_ok((void *)(uintptr_t)ua, alen)) return -14;
    if (copy_from_user(kbuf, (void *)(uintptr_t)ua, alen) < 0) return -14;
    uint16_t fam = (uint16_t)(kbuf[0] | (kbuf[1] << 8));
    if (fam != AF_UNIX_K) return -22;
    uint32_t plen = alen - 2;
    if (plen == 0) return 1;
    __builtin_memcpy(n->path, kbuf + 2, plen);
    if (n->path[0] == '\0') {
        /* Abstract namespace (Linux): sun_path[0]==0, the name is the raw
         * bytes that follow, embedded NULs and all.  libxcb tries
         * "@/tmp/.X11-unix/X0" before the filesystem path. */
        n->len = plen;
    } else {
        /* Filesystem namespace: the path runs to the first NUL, or fills
         * the whole of sun_path (unix_mkname_bsd). */
        uint32_t l = 0;
        while (l < plen && n->path[l]) l++;
        n->len = l;
    }
    n->path[n->len] = '\0';
    return 0;
}

/* Write a name out as struct sockaddr_un to uaddr, clipped to the caller's
 * *ulen, then store the full length in *ulen.  An unnamed socket reads as
 * the bare family (getsockname/getpeername/accept) or, for the sender of a
 * received message, as no address at all (`zero_unnamed`). */
static int usock_name_to_user(const usock_name_t *n, uint32_t uaddr,
                              uint32_t *ulen, int zero_unnamed) {
    uint8_t kbuf[2 + USOCK_PATH_MAX + 1];
    uint32_t total, klen;
    if (!ulen) return 0;
    if (copy_from_user(&klen, ulen, sizeof(klen)) < 0) return -14;
    if ((int32_t)klen < 0) return -22;
    kbuf[0] = AF_UNIX_K; kbuf[1] = 0;
    if (!n->len) {
        total = zero_unnamed ? 0 : 2;
    } else if (n->path[0]) {                  /* path: reported with its NUL */
        __builtin_memcpy(kbuf + 2, n->path, n->len + 1);
        total = 2 + n->len + 1;
    } else {                                  /* abstract: exactly the bytes */
        __builtin_memcpy(kbuf + 2, n->path, n->len);
        total = 2 + n->len;
    }
    uint32_t copy = klen < total ? klen : total;
    if (copy && (!uaddr || copy_to_user((void *)(uintptr_t)uaddr, kbuf, copy) < 0))
        return -14;
    if (copy_to_user(ulen, &total, sizeof(total)) < 0) return -14;
    return 0;
}

/* bind() to a filesystem name: create the socket inode (S_IFSOCK) like
 * mknod, with the directory's write+search permission checked, owned by the
 * caller with 0777 & ~umask (Linux unix_bind_bsd).  A name that exists in any
 * form is -EADDRINUSE. */
static int usock_fs_create(const char *path, vfs_node_t **out) {
    char resolved[256], dir_path[256], base[256];
    int r = resolve_path_at_fd(AT_FDCWD, path, resolved, sizeof(resolved));
    if (r < 0) return r;
    if (path_split(resolved, dir_path, base) < 0 || base[0] == '\0') return -22;
    if (vfs_open_nofollow(resolved)) return -98;            /* -EADDRINUSE */
    vfs_node_t *dir = vfs_open_parent_at(resolved, dir_path);
    if (!dir) return -2;                                    /* -ENOENT */
    if (!dir->create_fn) return -30;                        /* -EROFS */
    if (proc_access_check(dir, VFS_WANT_W | VFS_WANT_X) < 0) return -13;
    r = dir->create_fn(dir, base, VFS_FLAG_SOCK);
    if (r == -17) return -98;
    if (r < 0) return r;
    vfs_node_t *node = vfs_open_nofollow(resolved);
    if (!node) return -2;
    init_new_node(dir, node, 0777 & ~current_proc->umask);
    *out = node;
    return 0;
}

/* connect()/sendto() to a filesystem name: the inode must be a socket
 * (-ECONNREFUSED otherwise, as Linux) and writable by the caller. */
static int usock_fs_lookup(const char *path, vfs_node_t **out) {
    int err = -2;
    vfs_node_t *node = vfs_lookup_at(path, 1, &err);
    if (!node) return err;
    if (node->flags != VFS_FLAG_SOCK) return -111;
    if (proc_access_check(node, VFS_WANT_W) < 0) return -13;
    *out = node;
    return 0;
}

/* Resolve a destination name to (name, node) for connect/sendto. */
static int usock_resolve(usock_name_t *n, vfs_node_t **node) {
    *node = NULL;
    if (n->path[0] == '\0') return 0;                       /* abstract */
    return usock_fs_lookup(n->path, node);
}

/* recvmsg: hand over the ready SCM_RIGHTS fds.  An fd is installed only once
 * there is room for it BOTH in the caller's control buffer and in the fd
 * table; any that does not fit is closed and MSG_CTRUNC is reported, so
 * nothing can end up installed but unnameable — an unclosable leak in the
 * receiving process (Linux scm_detach_fds + __scm_destroy).  Writes
 * msg_controllen and msg_flags. */
static void usock_deliver_fds(usocket_t *us, uint32_t umsg, uint32_t uctrl,
                              uint32_t uctrllen, int wantfds, uint32_t msgflags,
                              uint32_t out_flags) {
    uint32_t ctrl_used = 0;
    proc_file_t got[SCM_MAX_FDS];
    int ngot = usocket_recv_fds(us, got, SCM_MAX_FDS);
    int room = wantfds ? (int)((uctrllen - 12) / 4) : 0;
    if (room > SCM_MAX_FDS) room = SCM_MAX_FDS;
    if (ngot > 0) {
        int cloex = (msgflags & MSG_CMSG_CLOEXEC_K) ? 1 : 0;
        int newfds[SCM_MAX_FDS]; int ninst = 0;
        for (int k = 0; k < ngot; k++) {
            int slot = -1;
            if (ninst < room) {         /* the cmsg can name it */
                for (int j = 0; j < MAX_FD; j++)
                    if (current_proc->ofile[j].type == FD_NONE) { slot = j; break; }
                if (slot < 0)
                    printk("[scm] fd table FULL on recv (pid %d)\n",
                           current_proc ? current_proc->pid : -1);
            }
            if (slot < 0) {             /* no room: close, truncate */
                fd_release(&got[k]);
                out_flags |= MSG_CTRUNC_K;
                continue;
            }
            current_proc->ofile[slot] = got[k];
            current_proc->ofile[slot].cloexec = (uint8_t)cloex;
            newfds[ninst++] = slot;
        }
        if (ninst > 0) {
            uint8_t cbuf[256];
            uint32_t clen = 12 + (uint32_t)ninst * 4;
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
    copy_to_user((void *)(uintptr_t)(umsg + 24), &out_flags, 4);
}

/* sendmsg: parse the control buffer straight from user memory and retain the
 * files its SCM_RIGHTS cmsg(s) name.  Anything wrong fails the whole call
 * before a byte is queued, as Linux __scm_send/scm_fp_copy do: a malformed
 * cmsg or too many fds is -EINVAL, a bad fd -EBADF, an unreadable buffer
 * -EFAULT.  Dropping them silently instead would hand the receiver a message
 * without the fds its protocol expects.  Returns the count (the refs in
 * pass[] are then the caller's) or a negative errno (nothing retained). */
#define SCM_CTL_MAX 20480U                 /* net.core.optmem_max */
static int usock_collect_fds(uint32_t uctrl, uint32_t uctrllen,
                             proc_file_t *pass) {
    int npass = 0, err = 0;
    if (uctrllen < 12) return 0;           /* CMSG_FIRSTHDR: none */
    if (uctrllen > SCM_CTL_MAX) return -105;                 /* -ENOBUFS */
    if (!uctrl) return -14;
    uint32_t off = 0;
    while (off + 12 <= uctrllen) {
        uint32_t h[3];                     /* cmsg_len, cmsg_level, cmsg_type */
        if (copy_from_user(h, (void *)(uintptr_t)(uctrl + off), 12) < 0) {
            err = -14; break;
        }
        if (h[0] < 12 || h[0] > uctrllen - off) { err = -22; break; }  /* !CMSG_OK */
        if (h[1] == 1 /*SOL_SOCKET*/) {
            if (h[2] == 1 /*SCM_RIGHTS*/) {
                uint32_t cnt = (h[0] - 12) / 4;
                if (cnt > (uint32_t)(SCM_MAX_FDS - npass)) { err = -22; break; }
                for (uint32_t k = 0; k < cnt; k++) {
                    int sfd;
                    if (copy_from_user(&sfd, (void *)(uintptr_t)(uctrl + off + 12 + k * 4),
                                       4) < 0) { err = -14; break; }
                    if (sfd < 0 || sfd >= MAX_FD ||
                        current_proc->ofile[sfd].type == FD_NONE) { err = -9; break; }
                    fd_copy(&pass[npass], &current_proc->ofile[sfd]);
                    npass++;
                }
                if (err) break;
            } else if (h[2] != 2 /*SCM_CREDENTIALS: accepted, not checked*/) {
                err = -22; break;
            }
        }
        off += (h[0] + 3u) & ~3u;
        if (off < h[0]) break;             /* wrapped */
    }
    if (err) {
        for (int k = 0; k < npass; k++) fd_release(&pass[k]);
        return err;
    }
    return npass;
}

/* The user iovec array of a msghdr, validated, in kernel memory (kfree it). */
static int usock_iov_from_user(uint32_t uiov, uint32_t iovlen, usock_iov_t **out) {
    *out = NULL;
    if (iovlen > 1024) return -22;                           /* UIO_MAXIOV */
    if (!iovlen) return 0;
    usock_iov_t *iv = (usock_iov_t *)kmalloc(iovlen * sizeof(usock_iov_t));
    if (!iv) return -12;
    uint32_t total = 0;
    for (uint32_t i = 0; i < iovlen; i++) {
        uint32_t e[2];
        if (copy_from_user(e, (void *)(uintptr_t)(uiov + i * 8), sizeof(e)) < 0 ||
            (e[1] && !access_ok((void *)(uintptr_t)e[0], e[1]))) {
            kfree(iv);
            return -14;
        }
        if ((int32_t)e[1] < 0 || total + e[1] < total) { kfree(iv); return -22; }
        total += e[1];
        iv[i].base = (void *)(uintptr_t)e[0];
        iv[i].len  = e[1];
    }
    *out = iv;
    return 0;
}

/* sendmsg/recvmsg on a SEQPACKET or DGRAM socket: one record per call,
 * gathered from / scattered into the whole iovec array. */
static int usock_record_msg(int call, uint32_t *kargs, usocket_t *us, int nb) {
    uint32_t umsg = kargs[1];
    uint32_t mh[7];
    if (copy_from_user(mh, (void *)(uintptr_t)umsg, sizeof(mh)) < 0) return -14;
    uint32_t uname = mh[0], unamelen = mh[1];
    uint32_t uctrl = mh[4], uctrllen = mh[5];
    int mflags = usock_flags(kargs[2], nb);
    usock_iov_t *iv = NULL;
    int r = usock_iov_from_user(mh[2], mh[3], &iv);
    if (r < 0) return r;
    int niov = (int)mh[3];

    if (call == 16) {
        usock_name_t to;
        vfs_node_t *tonode = NULL;
        int has_to = 0;
        if (uname && unamelen) {
            r = usock_name_from_user(uname, unamelen, &to);
            if (r == 0) r = usock_resolve(&to, &tonode);
            else if (r == 1) r = -22;
            if (r < 0) { kfree(iv); return r; }
            has_to = 1;
        }
        proc_file_t pass[SCM_MAX_FDS];
        int npass = usock_collect_fds(uctrl, uctrllen, pass);
        if (npass < 0) { kfree(iv); return npass; }
        r = usocket_send_record(us, iv, niov, pass, npass,
                                has_to ? &to : NULL, tonode, mflags);
        if (r < 0)                            /* the fds never left */
            for (int k = 0; k < npass; k++) fd_release(&pass[k]);
        kfree(iv);
        return r;
    }

    int wantfds = (uctrl && uctrllen >= 12);
    usock_name_t from;
    int oflags = 0;
    r = usocket_recv_record(us, iv, niov, mflags | USOCK_WANTFDS, &oflags, &from);
    kfree(iv);
    if (r < 0) return r;
    if (uname) {
        /* msg_namelen is the msghdr field right after msg_name. */
        if (usock_name_to_user(&from, uname, (uint32_t *)(uintptr_t)(umsg + 4), 1) < 0) {
            /* The record is consumed; so are its fds, or they would ride
             * with the NEXT record. */
            proc_file_t got[SCM_MAX_FDS];
            int ngot = usocket_recv_fds(us, got, SCM_MAX_FDS);
            for (int k = 0; k < ngot; k++) fd_release(&got[k]);
            return -14;
        }
    } else {
        uint32_t zero = 0;
        copy_to_user((void *)(uintptr_t)(umsg + 4), &zero, 4);
    }
    usock_deliver_fds(us, umsg, uctrl, uctrllen, wantfds, kargs[2],
                      (oflags & USOCK_MSG_TRUNC) ? MSG_TRUNC_K : 0);
    return r;
}

/* getsockopt(SOL_SOCKET) on an AF_UNIX socket (net/core/sock.c
 * sk_getsockopt): each option's value clipped to the caller's optlen, whose
 * new value is the length written.  Unknown options are -ENOPROTOOPT; other
 * levels have no handler on AF_UNIX (-EOPNOTSUPP). */
static int usock_getsockopt(usocket_t *us, uint32_t *kargs) {
    int       level   = (int)kargs[1];
    int       optname = (int)kargs[2];
    void     *optval  = (void *)(uintptr_t)kargs[3];
    uint32_t *optlen  = (uint32_t *)(uintptr_t)kargs[4];
    uint32_t  len = 0;
    uint32_t  v[3] = { 0, 0, 0 };
    uint32_t  size = 4;

    if (!optlen) return -14;
    if (copy_from_user(&len, optlen, sizeof(len)) < 0) return -14;
    if ((int32_t)len < 0) return -22;
    if (level != 1) return -95;                     /* not SOL_SOCKET */

    switch (optname) {
    case 3:  v[0] = (uint32_t)usocket_type(us); break;        /* SO_TYPE */
    case 4:  v[0] = 0; break;                                 /* SO_ERROR */
    /* SO_SNDBUF(7)/SO_RCVBUF(8) must report a POSITIVE buffer size — Firefox's
     * IPC Channel::SetPipe (ipc_channel_posix.cc:181) does CHECK(buf_len > 0)
     * on getsockopt(SO_SNDBUF).  Our per-direction ring size. */
    case 7: case 8: v[0] = 65536; break;
    case 2: case 5: case 6: case 9: case 10: case 12: case 16: case 34:
        v[0] = 0; break;           /* REUSEADDR DONTROUTE BROADCAST KEEPALIVE
                                    * OOBINLINE PRIORITY PASSCRED PASSSEC */
    case 13: size = 8; break;                                 /* SO_LINGER */
    case 17: {                                                /* SO_PEERCRED */
        usock_cred_t c;
        usocket_peercred(us, &c);
        v[0] = (uint32_t)c.pid; v[1] = c.uid; v[2] = c.gid;
        size = 12;
        break;
    }
    case 18: case 19: v[0] = 1; break;                        /* RCV/SNDLOWAT */
    case 20: case 21: size = 8; break;                        /* RCV/SNDTIMEO */
    case 30: v[0] = usocket_listening(us) ? 1 : 0; break;     /* SO_ACCEPTCONN */
    case 38: v[0] = 0; break;                                 /* SO_PROTOCOL */
    case 39: v[0] = AF_UNIX_K; break;                         /* SO_DOMAIN */
    default: return -92;                                      /* -ENOPROTOOPT */
    }
    if (len > size) len = size;
    if (len && (!optval || copy_to_user(optval, v, len) < 0)) return -14;
    if (copy_to_user(optlen, &len, sizeof(len)) < 0) return -14;
    return 0;
}

static int usock_rw_record(int fd, uint32_t uiov, int iovcnt, int write) {
    proc_file_t *f = &current_proc->ofile[fd];
    if (f->type != FD_USOCKET || !f->usock || !usocket_is_record(f->usock))
        return -1;
    usocket_t *us = f->usock;
    int flags = (f->flags & O_NONBLOCK) ? USOCK_NONBLOCK : 0;
    usock_iov_t *iv = NULL;
    int r = usock_iov_from_user(uiov, (uint32_t)iovcnt, &iv);
    if (r < 0) return r;
    usocket_pin(us);
    r = write ? usocket_send_record(us, iv, iovcnt, NULL, 0, NULL, NULL, flags)
              : usocket_recv_record(us, iv, iovcnt, flags, NULL, NULL);
    usocket_unpin(us);
    kfree(iv);
    return r;
}

/* One socketcall on an AF_UNIX socket.  The caller holds a pin on `us` for
 * the whole call, and nothing here may look at the descriptor again once the
 * call may have slept: another thread can have closed it meanwhile. */
static int usock_call(int call, uint32_t *kargs, usocket_t *us, int nb,
                      int accept4_flags) {
    if (call == 2) {                     /* bind(fd, sockaddr_un, len) */
        usock_name_t n;
        int r = usock_name_from_user(kargs[1], kargs[2], &n);
        if (r < 0) return r;
        if (r == 1) return usocket_autobind(us);
        if (n.path[0] == '\0') return usocket_bind(us, &n, NULL);
        /* As unix_bind_bsd: the inode first (an existing name is
         * -EADDRINUSE even for a bound socket), then the socket's state. */
        vfs_node_t *node = NULL;
        r = usock_fs_create(n.path, &node);
        if (r < 0) return r;
        r = usocket_bind(us, &n, node);
        if (r < 0) {
            /* Linux unix_bind_bsd: the inode it made goes again. */
            char dir_path[256], base[256], resolved[256];
            if (resolve_path_at_fd(AT_FDCWD, n.path, resolved, sizeof(resolved)) == 0 &&
                path_split(resolved, dir_path, base) == 0) {
                vfs_node_t *dir = vfs_open_parent_at(resolved, dir_path);
                if (dir) vfs_unlink(dir, base);
            }
        }
        return r;
    }
    if (call == 3) {                     /* connect(fd, sockaddr_un, len) */
        usock_name_t n;
        vfs_node_t *node = NULL;
        /* AF_UNSPEC dissolves a datagram socket's association. */
        if (kargs[2] >= 2 && usocket_type(us) == USOCK_DGRAM) {
            uint16_t fam = 0;
            if (copy_from_user(&fam, (void *)(uintptr_t)kargs[1], 2) < 0) return -14;
            if (fam == 0) return usocket_connect(us, NULL, NULL, nb);
        }
        int r = usock_name_from_user(kargs[1], kargs[2], &n);
        if (r == 1) r = -22;
        if (r < 0) return r;
        r = usock_resolve(&n, &node);
        if (r < 0) return r;
        return usocket_connect(us, &n, node, nb);
    }
    if (call == 4)                       /* listen(fd, backlog) */
        return usocket_listen(us, (int)kargs[1]);
    if (call == 5) {                     /* accept(fd, addr, addrlen) */
        int err = 0;
        usocket_t *ns = usocket_accept(us, nb, &err);
        if (!ns) return err;
        for (int nfd = 0; nfd < MAX_FD; nfd++)
            if (current_proc->ofile[nfd].type == FD_NONE) {
                if (kargs[1]) {
                    usock_name_t pn;
                    usocket_getname(ns, 1, &pn);
                    if (usock_name_to_user(&pn, kargs[1],
                                           (uint32_t *)(uintptr_t)kargs[2], 0) < 0) {
                        usocket_release(ns);
                        return -14;
                    }
                }
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
        usock_name_t n;
        int r = usocket_getname(us, call == 7, &n);
        if (r < 0) return r;
        return usock_name_to_user(&n, kargs[1], (uint32_t *)(uintptr_t)kargs[2], 0);
    }
    if (call == 9 || call == 11) {       /* send / sendto */
        const void *buf = (const void *)(uintptr_t)kargs[1];
        uint32_t len = kargs[2];
        if (!access_ok(buf, len)) return -14;
        if (call == 11 && kargs[4] && kargs[5]) {
            usock_name_t to;
            vfs_node_t *tonode = NULL;
            int r = usock_name_from_user(kargs[4], kargs[5], &to);
            if (r == 1) r = -22;
            if (r < 0) return r;
            if (!usocket_is_record(us))             /* unix_stream_sendmsg */
                return usocket_getname(us, 1, &to) == 0 ? -106 : -95;
            r = usock_resolve(&to, &tonode);
            if (r < 0) return r;
            usock_iov_t iov = { (void *)buf, len };
            return usocket_send_record(us, &iov, 1, NULL, 0, &to, tonode,
                                       usock_flags(kargs[3], nb));
        }
        return usocket_write(us, buf, (int)len, usock_flags(kargs[3], nb));
    }
    if (call == 10 || call == 12) {      /* recv / recvfrom */
        void *buf = (void *)(uintptr_t)kargs[1];
        uint32_t len = kargs[2];
        int mflags = usock_flags(kargs[3], nb);
        if (!access_ok(buf, len)) return -14;
        if (usocket_is_record(us)) {
            usock_iov_t iov = { buf, len };
            usock_name_t from;
            int r = usocket_recv_record(us, &iov, 1, mflags, NULL, &from);
            if (r >= 0 && call == 12 && kargs[4] &&
                usock_name_to_user(&from, kargs[4], (uint32_t *)(uintptr_t)kargs[5], 1) < 0)
                return -14;
            return r;
        }
        int r = usocket_read(us, buf, (int)len, mflags);
        if (r >= 0 && call == 12 && kargs[4]) {
            usock_name_t from;
            if (usocket_getname(us, 1, &from) < 0) from.len = 0;
            if (usock_name_to_user(&from, kargs[4], (uint32_t *)(uintptr_t)kargs[5], 1) < 0)
                return -14;
        }
        return r;
    }
    if (call == 16 || call == 17) {      /* sendmsg / recvmsg */
        if (usocket_is_record(us))
            return usock_record_msg(call, kargs, us, nb);
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
        if (call == 16 && mh[1]) {                   /* unix_stream_sendmsg */
            usock_name_t peer;
            return usocket_getname(us, 1, &peer) == 0 ? -106 : -95;
        }
        /* Only a control buffer big enough for a cmsg header can name
         * received fds; a recvmsg without one still collects them here
         * (rather than letting the read drop them silently) so that it can
         * close them and report MSG_CTRUNC, as scm_recv does. */
        int wantfds = (call == 17 && uctrl && uctrllen >= 12);
        int mflags  = usock_flags(kargs[2], nb);
        if (call == 17) mflags |= USOCK_WANTFDS;

        /* sendmsg: parse SCM_RIGHTS cmsg(s), retain the named fds. */
        proc_file_t pass[SCM_MAX_FDS];
        int npass = (call == 16) ? usock_collect_fds(uctrl, uctrllen, pass) : 0;
        if (npass < 0) return npass;          /* nothing queued, nothing sent */

        /* Linux attaches the fds to the FIRST skb of the message
         * (unix_stream_sendmsg), so the batch goes in BEFORE the bytes it
         * rides with: a reader draining concurrently can then never pass
         * the batch's position while the batch is still invisible to it —
         * which would hand the fds to the next message's bytes, or lose
         * them to a plain read().  If the data cannot be written at all,
         * scm_abort() takes the batch back out. */
        uint32_t scm_id = 0;
        if (call == 16 && npass > 0) {
            int sr = usocket_send_fds(us, pass, npass, &scm_id);
            if (sr < 0) {
                for (int k = 0; k < npass; k++) fd_release(&pass[k]);
                npass = 0;
                /* A reader that is gone makes the write below fail with
                 * EPIPE (and SIGPIPE) as usual; any other failure fails the
                 * call rather than send the data without its fds. */
                if (sr != -32) return sr;
            }
        }

        int total = 0, eagain = 0;
        for (uint32_t i = 0; i < iovlen; i++) {
            uint32_t iv[2];
            if (copy_from_user(iv, (void *)(uintptr_t)(iov + i * 8),
                               sizeof(iv)) < 0) {
                if (total) break;
                scm_abort(us, scm_id, pass, npass);
                return -14;
            }
            int len = (int)iv[1];
            if (len <= 0) continue;
            if (!access_ok((void *)(uintptr_t)iv[0], (size_t)len)) {
                if (total) break;
                scm_abort(us, scm_id, pass, npass);
                return -14;
            }
            int n = (call == 16)
                ? usocket_write(us, (void *)(uintptr_t)iv[0], len, mflags)
                : usocket_read(us, (void *)(uintptr_t)iv[0], len, mflags);
            if (n < 0) {
                if (total) break;
                if (call == 16) {
                    scm_abort(us, scm_id, pass, npass);
                    return n;
                }
                if (n == -11) { eagain = 1; break; }
                return n;
            }
            total += n;
            if (n < len) break;          /* short read/write — stop */
            if (mflags & USOCK_PEEK) break;   /* peek does not advance */
            /* Once some bytes have moved, the rest of the call must not
             * block (a recvmsg returns what is there, as Linux's single
             * read_iter pass does) nor raise SIGPIPE (Linux: only when
             * nothing was sent). */
            mflags |= (call == 17) ? USOCK_NONBLOCK : USOCK_NOSIGNAL;
            /* One message's ancillary data per recvmsg: stop rather than
             * read into a second batch whose fds this call cannot also
             * deliver (Linux breaks its loop on !unix_skb_scm_eq). */
            if (call == 17 && usocket_fds_ready(us)) break;
        }

        /* A sendmsg that put no bytes on the stream sends no fds either:
         * unix_stream_sendmsg never enters its loop for a zero-length
         * message, so no skb carries them and scm_destroy closes them. */
        if (call == 16 && total == 0)
            scm_abort(us, scm_id, pass, npass);

        if (call == 17) {
            if (mh[0]) {
                usock_name_t from;
                if (usocket_getname(us, 1, &from) < 0) from.len = 0;
                usock_name_to_user(&from, mh[0], (uint32_t *)(uintptr_t)(umsg + 4), 1);
            } else {
                uint32_t zero = 0;
                copy_to_user((void *)(uintptr_t)(umsg + 4), &zero, 4);
            }
            usock_deliver_fds(us, umsg, uctrl, uctrllen, wantfds, kargs[2], 0);
            if (total == 0 && eagain) return -11;
        }
        return total;
    }
    if (call == 13)                      /* shutdown(fd, how) */
        return usocket_shutdown(us, (int)kargs[1]);
    if (call == 14) return 0;            /* setsockopt: accepted, ignored */
    if (call == 15)                      /* getsockopt */
        return usock_getsockopt(us, kargs);
    return -22;
}

/* sendmsg/recvmsg on an AF_INET socket: the iovecs gathered/scattered through
 * the same send/recv paths as send()/recv(), msg_name as sendto()/recvfrom().
 * A UDP datagram is one net_socket_sendto/recvfrom over the whole iovec array
 * (so it is never split or glued); a TCP stream goes segment by segment, and
 * once some bytes have moved the rest neither blocks (recv) nor raises
 * SIGPIPE (send), as Linux's single tcp_sendmsg/tcp_recvmsg pass.  Control
 * data (IP_PKTINFO and friends) is not supported: on send it is ignored, on
 * receive msg_controllen comes back 0. */
static int inet_msg(int call, uint32_t *kargs, proc_file_t *f) {
    uint32_t umsg = kargs[1];
    uint32_t mh[7];                 /* name,namelen,iov,iovlen,control,controllen,flags */
    if (copy_from_user(mh, (void *)(uintptr_t)umsg, sizeof(mh)) < 0) return -14;
    net_socket_t *so = f->socket;
    int stream = net_socket_is_stream(so);
    int mflags = (int)(kargs[2] & (NET_MSG_PEEK | NET_MSG_DONTWAIT |
                                   NET_MSG_WAITALL | SOCK_MSG_NOSIGNAL));
    if (f->flags & O_NONBLOCK) mflags |= NET_MSG_DONTWAIT;
    usock_iov_t *iv = NULL;
    int r = usock_iov_from_user(mh[2], mh[3], &iv);
    if (r < 0) return r;
    int niov = (int)mh[3];
    uint32_t total = 0;
    for (int i = 0; i < niov; i++) total += iv[i].len;

    net_sockaddr_in_t kaddr, *pa = NULL;
    if (call == 16 && mh[0] && mh[1]) {
        if (mh[1] < sizeof(kaddr)) { kfree(iv); return -22; }
        if (copy_from_user(&kaddr, (void *)(uintptr_t)mh[0], sizeof(kaddr)) < 0) {
            kfree(iv); return -14;
        }
        pa = &kaddr;
    }

    net_socket_retain(so);          /* the descriptor may close while we sleep */
    int done = 0;
    if (!stream) {
        /* One datagram over the whole array, through a kernel buffer. */
        uint32_t cap = total ? total : 1;
        if (cap > 65536) cap = 65536;
        uint8_t *kb = (uint8_t *)kmalloc(cap);
        if (!kb) { r = -12; goto out; }
        if (call == 16) {
            uint32_t off = 0;
            if (total > cap) { kfree(kb); r = -90; goto out; }       /* -EMSGSIZE */
            for (int i = 0; i < niov && r >= 0; i++) {
                if (iv[i].len && copy_from_user(kb + off, iv[i].base, iv[i].len) < 0)
                    r = -14;
                off += iv[i].len;
            }
            if (r >= 0) r = net_socket_sendto(so, kb, total, pa, mflags);
        } else {
            r = net_socket_recvfrom(so, kb, total < cap ? total : cap,
                                    mh[0] ? &kaddr : NULL, mflags);
            uint32_t left = r > 0 ? (uint32_t)r : 0, off = 0;
            for (int i = 0; i < niov && left; i++) {
                uint32_t c = iv[i].len < left ? iv[i].len : left;
                if (copy_to_user(iv[i].base, kb + off, c) < 0) { r = -14; break; }
                off += c; left -= c;
            }
            if (r >= 0) pa = mh[0] ? &kaddr : NULL;
        }
        kfree(kb);
        done = r;
    } else {
        for (int i = 0; i < niov; i++) {
            if (!iv[i].len) continue;
            int n = (call == 16)
                ? sock_send_user(so, iv[i].base, iv[i].len, NULL, mflags)
                : sock_recv_user(so, iv[i].base, iv[i].len, NULL, mflags);
            if (n < 0) { if (!done) r = n; break; }
            done += n;
            if ((uint32_t)n < iv[i].len || (mflags & NET_MSG_PEEK)) break;
            if (call == 16) mflags |= SOCK_MSG_NOSIGNAL;
            else if (!(mflags & NET_MSG_WAITALL)) mflags |= NET_MSG_DONTWAIT;
        }
        if (done) r = done;
        else if (r >= 0) r = 0;
    }
    if (r >= 0 && call == 17) {
        /* msg_name: the datagram's source; a stream reports none (Linux
         * tcp_recvmsg leaves msg_namelen 0). */
        uint32_t nl = 0, zero = 0;
        if (pa) {
            nl = sizeof(kaddr);
            uint32_t want = mh[1] < nl ? mh[1] : nl;
            if (want && copy_to_user((void *)(uintptr_t)mh[0], &kaddr, want) < 0) r = -14;
        }
        if (copy_to_user((void *)(uintptr_t)(umsg + 4), &nl, 4) < 0 ||
            copy_to_user((void *)(uintptr_t)(umsg + 20), &zero, 4) < 0 ||
            copy_to_user((void *)(uintptr_t)(umsg + 24), &zero, 4) < 0)
            r = -14;
    }
out:
    net_socket_release(so);
    kfree(iv);
    return r;
}

static int free_fd_slot(void) {
    for (int nfd = 0; nfd < MAX_FD; nfd++)
        if (current_proc->ofile[nfd].type == FD_NONE)
            return nfd;
    return -1;
}

/* accept/accept4 on an AF_INET listener: the new connection gets the lowest
 * free descriptor, SOCK_NONBLOCK/SOCK_CLOEXEC from accept4, and the peer's
 * address in addr/addrlen when asked (Linux copies min(len, 16) and stores
 * the full size).  EMFILE is reported before waiting, as Linux allocates the
 * descriptor first. */
static int inet_accept(proc_file_t *f, uint32_t *kargs, int flags) {
    if (flags & ~(SOCK_NONBLOCK_K | SOCK_CLOEXEC_K))
        return -22;
    uint32_t *ulen = (uint32_t *)(uintptr_t)kargs[2];
    uint32_t klen = 0;
    if (kargs[1]) {
        if (!ulen || copy_from_user(&klen, ulen, sizeof(klen)) < 0) return -14;
        if ((int32_t)klen < 0) return -22;
    }
    if (free_fd_slot() < 0)
        return -24;
    net_socket_t *ns = NULL;
    int r = net_socket_accept(f->socket, &ns, (f->flags & O_NONBLOCK) != 0);
    if (r < 0)
        return r;
    if (kargs[1]) {
        net_sockaddr_in_t pa;
        if (net_socket_getname(ns, 1, &pa) < 0) {
            __builtin_memset(&pa, 0, sizeof(pa));
            pa.family = 2;
        }
        uint32_t c = klen < sizeof(pa) ? klen : sizeof(pa);
        uint32_t full = sizeof(pa);
        if ((c && copy_to_user((void *)(uintptr_t)kargs[1], &pa, c) < 0) ||
            copy_to_user(ulen, &full, sizeof(full)) < 0) {
            net_socket_release(ns);
            return -14;
        }
    }
    int nfd = free_fd_slot();       /* another thread may have taken it */
    if (nfd < 0) {
        net_socket_release(ns);
        return -24;
    }
    current_proc->ofile[nfd].type = FD_SOCKET;
    current_proc->ofile[nfd].socket = ns;
    current_proc->ofile[nfd].flags = O_RDWR | ((flags & SOCK_NONBLOCK_K) ? O_NONBLOCK : 0);
    current_proc->ofile[nfd].cloexec = (flags & SOCK_CLOEXEC_K) ? 1 : 0;
    return nfd;
}

/* ── AF_NETLINK / AF_PACKET / AF_INET SOCK_RAW (net/xsock.h) ─────────────
 * These carry their own sockaddr (sockaddr_nl, sockaddr_ll, sockaddr_in), so
 * the address bytes go through as they are, up to sockaddr_storage's 128.
 * Datagram sockets: one send or receive is one message, MSG_TRUNC on a
 * receive reports the full length.  setsockopt/getsockopt other than
 * SO_ATTACH_FILTER take the generic path (XSOCK_GENERIC). */
#define XSOCK_GENERIC (-1000)
#define XADDR_MAX     128

static int xaddr_in(uint32_t uaddr, uint32_t alen, uint8_t *k, uint32_t *klen) {
    *klen = 0;
    if (!uaddr) return 0;
    if ((int32_t)alen < 0 || alen > XADDR_MAX) return -22;
    if (alen && copy_from_user(k, (void *)(uintptr_t)uaddr, alen) < 0) return -14;
    *klen = alen;
    return 0;
}

/* Linux move_addr_to_user: min(user len, size) bytes, then the full size. */
static int xaddr_out(uint32_t uaddr, uint32_t ulenp, const uint8_t *k,
                     uint32_t klen) {
    if (!uaddr || !ulenp) return 0;
    uint32_t ulen;
    if (copy_from_user(&ulen, (void *)(uintptr_t)ulenp, 4) < 0) return -14;
    if ((int32_t)ulen < 0) return -22;
    uint32_t n = ulen < klen ? ulen : klen;
    if (n && copy_to_user((void *)(uintptr_t)uaddr, k, n) < 0) return -14;
    return copy_to_user((void *)(uintptr_t)ulenp, &klen, 4) < 0 ? -14 : 0;
}

static int xsock_call(int call, uint32_t *kargs, proc_file_t *f) {
    net_socket_t *so = f->socket;
    uint8_t ka[XADDR_MAX];
    uint32_t kl;
    int r;
    switch (call) {
    case 2: case 3:                                   /* bind / connect */
        if ((r = xaddr_in(kargs[1], kargs[2], ka, &kl)) < 0) return r;
        if (!kargs[1]) return -14;
        return call == 2 ? net_socket_xbind(so, ka, kl)
                         : net_socket_xconnect(so, ka, kl);
    case 4: case 5: case 13:                          /* listen/accept/shutdown */
        return -95;
    case 6: case 7:                                   /* getsockname/getpeername */
        __builtin_memset(ka, 0, sizeof(ka));
        kl = 0;
        if ((r = net_socket_xgetname(so, call == 7, ka, &kl)) < 0) return r;
        if (!kargs[2]) return -14;
        return xaddr_out(kargs[1], kargs[2], ka, kl);
    case 14:
        /* SO_ATTACH_FILTER: struct sock_fprog { u16 len; filter * } names
         * the program in user memory; the family gets a kernel copy. */
        if (kargs[1] == 1 && kargs[2] == 26) {
            uint32_t fprog[2];
            if (kargs[4] < 8 ||
                copy_from_user(fprog, (void *)(uintptr_t)kargs[3], 8) < 0)
                return -14;
            uint32_t n = fprog[0] & 0xFFFF;
            if (n == 0 || n > 4096) return -22;
            void *prog = kmalloc(n * 8);
            if (!prog) return -12;
            r = copy_from_user(prog, (void *)(uintptr_t)fprog[1], n * 8) < 0
                ? -14 : net_socket_setopt(so, 1, 26, prog, n * 8);
            kfree(prog);
            return r;
        }
        return XSOCK_GENERIC;
    case 15:
        return XSOCK_GENERIC;
    }

    if (call == 9 || call == 10 || call == 11 || call == 12) {
        /* send(fd,buf,len,flags) recv(...) sendto(...,addr,alen)
         * recvfrom(...,addr,*alen) */
        uint32_t len = kargs[2] > 65536 ? 65536 : kargs[2];
        int flags = (int)kargs[3];
        int mf = (flags & (NET_MSG_PEEK | NET_MSG_DONTWAIT)) |
                 ((f->flags & O_NONBLOCK) ? NET_MSG_DONTWAIT : 0);
        if (!access_ok((void *)(uintptr_t)kargs[1], kargs[2])) return -14;
        uint8_t *kb = (uint8_t *)kmalloc(len ? len : 1);
        if (!kb) return -12;
        if (call == 9 || call == 11) {
            kl = 0;
            r = call == 11 ? xaddr_in(kargs[4], kargs[5], ka, &kl) : 0;
            if (r >= 0 && copy_from_user(kb, (void *)(uintptr_t)kargs[1], len) < 0)
                r = -14;
            if (r >= 0)
                r = net_socket_xsendto(so, kb, len, kl ? ka : NULL, kl, mf);
        } else {
            kl = 0;
            r = net_socket_xrecvfrom(so, kb, len, ka, &kl, mf);
            if (r >= 0) {
                uint32_t got = (uint32_t)r < len ? (uint32_t)r : len;
                if (got && copy_to_user((void *)(uintptr_t)kargs[1], kb, got) < 0)
                    r = -14;
                else if (call == 12) {
                    int ar = xaddr_out(kargs[4], kargs[5], ka, kl);
                    if (ar < 0) r = ar;
                }
                if (r >= 0 && !(flags & MSG_TRUNC_K)) r = (int)got;
            }
        }
        kfree(kb);
        return r;
    }

    if (call == 16 || call == 17) {                   /* sendmsg / recvmsg */
        uint32_t umsg = kargs[1];
        uint32_t mh[7];      /* name,namelen,iov,iovlen,control,controllen,flags */
        if (copy_from_user(mh, (void *)(uintptr_t)umsg, sizeof(mh)) < 0) return -14;
        int flags = (int)kargs[2];
        int mf = (flags & (NET_MSG_PEEK | NET_MSG_DONTWAIT)) |
                 ((f->flags & O_NONBLOCK) ? NET_MSG_DONTWAIT : 0);
        usock_iov_t *iv = NULL;
        r = usock_iov_from_user(mh[2], mh[3], &iv);
        if (r < 0) return r;
        uint32_t total = 0;
        for (uint32_t i = 0; i < mh[3]; i++) total += iv[i].len;
        uint32_t cap = total > 65536 ? 65536 : total;
        uint8_t *kb = (uint8_t *)kmalloc(cap ? cap : 1);
        if (!kb) { kfree(iv); return -12; }
        if (call == 16) {
            kl = 0;
            r = xaddr_in(mh[0], mh[0] ? mh[1] : 0, ka, &kl);
            if (r >= 0 && total > cap) r = -90;          /* -EMSGSIZE */
            uint32_t off = 0;
            for (uint32_t i = 0; r >= 0 && i < mh[3]; i++) {
                if (iv[i].len && copy_from_user(kb + off, iv[i].base, iv[i].len) < 0)
                    r = -14;
                off += iv[i].len;
            }
            if (r >= 0)
                r = net_socket_xsendto(so, kb, total, kl ? ka : NULL, kl, mf);
        } else {
            kl = 0;
            r = net_socket_xrecvfrom(so, kb, cap, ka, &kl, mf);
            if (r >= 0) {
                uint32_t got = (uint32_t)r < cap ? (uint32_t)r : cap;
                uint32_t left = got, off = 0;
                for (uint32_t i = 0; i < mh[3] && left; i++) {
                    uint32_t c = iv[i].len < left ? iv[i].len : left;
                    if (copy_to_user(iv[i].base, kb + off, c) < 0) { r = -14; break; }
                    off += c; left -= c;
                }
                if (r >= 0) {
                    uint32_t mflags = (uint32_t)r > got ? MSG_TRUNC_K : 0;
                    uint32_t nl = mh[0] ? kl : 0, zero = 0;
                    uint32_t n = mh[1] < nl ? mh[1] : nl;
                    if (n && copy_to_user((void *)(uintptr_t)mh[0], ka, n) < 0) r = -14;
                    if (copy_to_user((void *)(uintptr_t)(umsg + 4), &nl, 4) < 0 ||
                        copy_to_user((void *)(uintptr_t)(umsg + 20), &zero, 4) < 0 ||
                        copy_to_user((void *)(uintptr_t)(umsg + 24), &mflags, 4) < 0)
                        r = -14;
                    if (r >= 0 && !(flags & MSG_TRUNC_K)) r = (int)got;
                }
            }
        }
        kfree(kb);
        kfree(iv);
        return r;
    }
    return -22;
}

static int socketcall_core_inner(int call, uint32_t *kargs) {
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
            int ut = usock_type_check(kargs[1], (int)kargs[2]);
            if (ut < 0) return ut;
            usocket_t *us = usocket_create(ut);
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
        int ut = usock_type_check(kargs[1], (int)kargs[2]);
        if (ut < 0) return ut;
        uint32_t *usv = (uint32_t *)(uintptr_t)kargs[3];
        if (!access_ok(usv, 2 * sizeof(uint32_t))) return -14;
        usocket_t *a = NULL, *b = NULL;
        if (usocket_socketpair(ut, &a, &b) < 0) return -12;
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
        /* Hold the socket for the whole call (Linux fdget): another thread
         * sharing the fd table can close the descriptor while this one
         * sleeps, and the socket must outlive the sleep. */
        usocket_t *us = f->usock;
        int nb = (f->flags & O_NONBLOCK) != 0;
        usocket_pin(us);
        int r = usock_call(call, kargs, us, nb, accept4_flags);
        usocket_unpin(us);
        return r;
    }

    if (f->type != FD_SOCKET || !f->socket)
        return -88;

    if (net_socket_is_x(f->socket)) {
        net_socket_t *so = f->socket;
        net_socket_retain(so);      /* the descriptor may close meanwhile */
        int r = xsock_call(call, kargs, f);
        net_socket_release(so);
        if (r != XSOCK_GENERIC)
            return r;
    }

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
                           : net_socket_connect(f->socket, &kaddr,
                                                (f->flags & O_NONBLOCK) != 0);
    }

    /* send/recv flags: the MSG_* bits net_socket_* understand, the
     * descriptor's O_NONBLOCK folded in as MSG_DONTWAIT, and MSG_NOSIGNAL for
     * sock_send_user's SIGPIPE decision. */
    int mflags = 0;
    if (call >= 9 && call <= 12) {
        mflags = (int)(kargs[3] & (NET_MSG_PEEK | NET_MSG_DONTWAIT |
                                   NET_MSG_WAITALL | SOCK_MSG_NOSIGNAL));
        if (f->flags & O_NONBLOCK) mflags |= NET_MSG_DONTWAIT;
    }

    if (call == 9) { /* send(fd, buf, len, flags) */
        const void *buf = (const void *)(uintptr_t)kargs[1];
        uint32_t len = kargs[2];
        if (!access_ok(buf, len))
            return -14;
        return sock_send_user(f->socket, buf, len, NULL, mflags);
    }

    if (call == 10) { /* recv(fd, buf, len, flags) */
        void *buf = (void *)(uintptr_t)kargs[1];
        uint32_t len = kargs[2];
        if (!access_ok(buf, len))
            return -14;
        return sock_recv_user(f->socket, buf, len, NULL, mflags);
    }

    if (call == 11) { /* sendto */
        const void *buf = (const void *)(uintptr_t)kargs[1];
        uint32_t len = kargs[2];
        net_sockaddr_in_t *uaddr = (net_sockaddr_in_t *)(uintptr_t)kargs[4];
        uint32_t addrlen = kargs[5];
        if (!access_ok(buf, len))
            return -14;
        if (!uaddr)
            return sock_send_user(f->socket, buf, len, NULL, mflags);
        if (addrlen < sizeof(net_sockaddr_in_t) ||
            !access_ok(uaddr, sizeof(net_sockaddr_in_t)))
            return -14;
        net_sockaddr_in_t kaddr;
        if (copy_from_user(&kaddr, uaddr, sizeof(kaddr)) < 0)
            return -14;
        return sock_send_user(f->socket, buf, len, &kaddr, mflags);
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
        int ret = sock_recv_user(f->socket, buf, len, uaddr ? &kaddr : NULL,
                                 mflags);
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

    if (call == 4)                  /* listen(fd, backlog) */
        return net_socket_listen(f->socket, (int)kargs[1]);

    if (call == 5)                  /* accept / accept4 */
        return inet_accept(f, kargs, accept4_flags);

    if (call == 14) {               /* setsockopt */
        /* The options net/socket.c knows (SO_REUSEADDR, SO_KEEPALIVE,
         * SO_RCVTIMEO/SO_SNDTIMEO, TCP_NODELAY, TCP_KEEP*); the rest are
         * accepted and ignored. */
        uint8_t kv[16] = {0};
        uint32_t len = kargs[4];
        if (len > sizeof(kv)) len = sizeof(kv);
        if (len && (!kargs[3] ||
                    copy_from_user(kv, (void *)(uintptr_t)kargs[3], len) < 0))
            return -14;
        return net_socket_setopt(f->socket, (int)kargs[1], (int)kargs[2], kv, len);
    }

    if (call == 15) {               /* getsockopt */
        int       level   = (int)kargs[1];
        int       optname = (int)kargs[2];
        void     *optval = (void *)(uintptr_t)kargs[3];
        uint32_t *optlen = (uint32_t *)(uintptr_t)kargs[4];
        uint32_t  len = 0;
        if (optlen) {
            if (copy_from_user(&len, optlen, sizeof(len)) < 0) return -14;
        }
        if ((int32_t)len < 0) return -22;
        uint8_t kv[16];
        uint32_t klen = len < sizeof(kv) ? len : sizeof(kv);
        /* SO_ERROR (4) is the pending error as a positive errno, cleared by
         * the read — how a non-blocking connect's outcome is learnt. */
        int r = net_socket_getopt(f->socket, level, optname, kv, &klen);
        if (r < 0) return r;
        if (r == 1) {               /* unknown here: reads as 0, as before */
            if (len < 4) return 0;
            __builtin_memset(kv, 0, 4);
            klen = 4;
        }
        if (!optval || !optlen) return klen ? -14 : 0;
        if (klen && copy_to_user(optval, kv, klen) < 0) return -14;
        if (copy_to_user(optlen, &klen, sizeof(klen)) < 0) return -14;
        return 0;
    }

    if (call == 6 || call == 7) {   /* getsockname / getpeername */
        void     *uaddr = (void *)(uintptr_t)kargs[1];
        uint32_t *ulen  = (uint32_t *)(uintptr_t)kargs[2];
        uint32_t  klen;
        net_sockaddr_in_t kaddr;
        if (!ulen || copy_from_user(&klen, ulen, sizeof(klen)) < 0) return -14;
        if ((int32_t)klen < 0) return -22;
        int r = net_socket_getname(f->socket, call == 7, &kaddr);
        if (r < 0) return r;
        /* Linux copies min(len, sizeof addr) and reports the full size. */
        if (klen > sizeof(kaddr)) klen = sizeof(kaddr);
        if (klen && copy_to_user(uaddr, &kaddr, klen) < 0) return -14;
        klen = sizeof(kaddr);
        if (copy_to_user(ulen, &klen, sizeof(klen)) < 0) return -14;
        return 0;
    }

    if (call == 16 || call == 17)   /* sendmsg / recvmsg */
        return inet_msg(call, kargs, f);

    if (call == 8)
        return -95;

    return -22;
}

/* NETTRACE (include/kernel/config.h) logs every AF_INET socket call with its
 * result: the trace that showed Firefox's socket thread asleep in a recv it
 * had asked not to block. */
static int socketcall_core(int call, uint32_t *kargs) {
    int r = socketcall_core_inner(call, kargs);
    if (NETTRACE) {
        int inet = 0;
        if (call == 1) inet = ((int)kargs[0] != AF_UNIX_K);
        else if ((int)kargs[0] >= 0 && (int)kargs[0] < MAX_FD)
            inet = current_proc->ofile[kargs[0]].type == FD_SOCKET;
        if (inet)
            printk("[NET] pid=%d socketcall %d (%x,%x,%x,%x) -> %d\n",
                   current_proc->pid, call, (unsigned)kargs[0], (unsigned)kargs[1],
                   (unsigned)kargs[2], (unsigned)kargs[3], r);
    }
    return r;
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

    uint64_t kp_pro = kprof_probe_begin();
    if (current_proc) current_proc->last_syscall = (int)num;
    /* kprof's periodic dump.  Here rather than in the timer tick so the printk
     * runs with interrupts enabled and does not cost the tick counter time. */
    kprof_tick();
    kprof_probe_end(KPP_SYS_PRO, kp_pro);

    uint64_t kp_body = kprof_probe_begin();
    switch (num) {
    case 1:   sys_exit(regs);                  break;  /* noreturn */
    case 2:   ret = sys_fork(regs);            break;
    case 3:   ret = sys_read(regs);            break;
    case 4:   ret = sys_write(regs);           break;
    case 5:   ret = sys_open(regs);            break;
    case 6:   ret = sys_close(regs);           break;
    case 7:   ret = sys_waitpid(regs);         break;
    case 10:  ret = sys_unlink(regs);          break;
    case 21:  ret = sys_mount(regs);           break;  /* mount */
    case 22:  ret = sys_umount2(regs, 0);      break;  /* umount (oldumount) */
    case 52:  ret = sys_umount2(regs, regs->ecx); break;  /* umount2 */
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
    case 40:  ret = sys_rmdir(regs);           break;
    case 41:  ret = sys_dup(regs);             break;
    case 42:  ret = sys_pipe(regs);            break;
    case 43:  ret = sys_times(regs);           break;
    case 45:  ret = sys_brk(regs);             break;
    case 47:  ret = sys_getgid(regs);          break;
    case 48:  ret = sys_signal(regs);          break;
    case 49:  ret = sys_geteuid(regs);         break;  /* geteuid */
    case 50:  ret = sys_getegid(regs);         break;  /* getegid */
    case 23:  ret = sys_setuid16(regs);        break;  /* setuid16 */
    case 46:  ret = sys_setgid16(regs);        break;  /* setgid16 */
    case 70:  ret = sys_setreuid16(regs);      break;  /* setreuid16 */
    case 71:  ret = sys_setregid16(regs);      break;  /* setregid16 */
    case 164: ret = sys_setresuid16(regs);     break;  /* setresuid16 */
    case 165: ret = sys_getresuid16(regs);     break;  /* getresuid16 */
    case 170: ret = sys_setresgid16(regs);     break;  /* setresgid16 */
    case 171: ret = sys_getresgid16(regs);     break;  /* getresgid16 */
    case 203: ret = sys_setreuid(regs);        break;  /* setreuid32 */
    case 204: ret = sys_setregid(regs);        break;  /* setregid32 */
    case 210: ret = sys_setresgid(regs);       break;  /* setresgid32 */
    case 139: case 216: ret = sys_setfsgid(regs); break;  /* setfsgid{16,32} */
    case 213: ret = sys_setuid(regs);          break;  /* setuid32 (musl) */
    case 214: ret = sys_setgid(regs);          break;  /* setgid32 (musl) */
    case 138: case 215: ret = sys_setfsuid(regs); break;  /* setfsuid{16,32} */
    case 54:  ret = sys_ioctl(regs);           break;
    case 55:  ret = sys_fcntl(regs);           break;
    case 57:  ret = sys_setpgid(regs);         break;
    case 60:  ret = sys_umask(regs);           break;
    case 61:  ret = sys_chroot(regs);          break;
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
    case 193: ret = sys_truncate64(regs);      break;
    case 194: ret = sys_ftruncate64(regs);     break;
    case 324: ret = sys_fallocate(regs);       break;  /* fallocate */
    case 106: ret = sys_stat(regs);            break;
    case 108: ret = sys_fstat(regs);           break;
    case 114: ret = sys_wait4(regs);           break;
    case 284: ret = sys_waitid(regs);          break;
    case 77:  ret = sys_getrusage(regs);       break;
    case 119: sys_sigreturn(regs);             return; /* regs fully restored —
        the dispatcher must NOT stomp the restored eax with its ret value */
    case 120: ret = sys_clone(regs);           break;
    case 190: ret = sys_vfork(regs);           break;
    case 435: ret = sys_clone3(regs);          break;  /* clone3 */
    case 122: ret = sys_uname(regs);           break;
    case 125: ret = sys_mprotect(regs);        break;
    case 132: ret = sys_getpgid(regs);         break;
    case 133: ret = sys_fchdir(regs);          break;
    case 147: ret = sys_getsid(regs);          break;
    case 141: ret = sys_getdents(regs);        break;
    case 146: ret = sys_writev(regs);          break;
    case 159: ret = sys_sched_get_priority(regs, 1); break;
    case 160: ret = sys_sched_get_priority(regs, 0); break;
    case 162: ret = sys_nanosleep(regs);       break;
    case 168: ret = sys_poll(regs);            break;
    case 309: ret = sys_ppoll(regs, 0);        break;  /* ppoll */
    case 414: ret = sys_ppoll(regs, 1);        break;  /* ppoll_time64 */
    case 172: ret = sys_prctl(regs);           break;
    case 174: ret = sys_rt_sigaction(regs);    break;
    case 175: ret = sys_rt_sigprocmask(regs);  break;
    case 176: ret = sys_rt_sigpending(regs);   break;
    case 73:  ret = sys_rt_sigpending(regs);   break;  /* sigpending */
    /* fsync(118)/fdatasync(148): our filesystems are RAM/simple-backed and every
     * write is already durable to our backing store, so a sync is a correct
     * no-op.  MUST return 0 (success), not -ENOSYS — glibc's fsync propagates
     * ENOSYS to the app, and code that treats a failed fsync as a failed write
     * (Firefox's startupCache / sqlite / prefs) can then error out. */
    case 118: ret = 0;                         break;  /* fsync */
    case 148: ret = 0;                         break;  /* fdatasync */
    /* sync(36)/syncfs(344): for the same reason there is nothing to flush
     * (ext2 writes through); syncfs still wants a valid descriptor. */
    case 36:  ret = 0;                         break;  /* sync */
    /* splice(313)/tee(315): not implemented, answered the way Linux answers
     * for descriptors that cannot be spliced (-EINVAL), on which callers
     * (coreutils cat 9.8+) fall back to read/write.  vmsplice (316) too. */
    case 313: case 315: case 316: ret = -22;  break;
    case 344: ret = ((int)regs->ebx < 0 || (int)regs->ebx >= MAX_FD ||
                     current_proc->ofile[(int)regs->ebx].type == FD_NONE) ? -9 : 0;
              break;  /* syncfs */
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
    case 9:   ret = sys_link_paths(AT_FDCWD, (const char *)(uintptr_t)regs->ebx,
                                   AT_FDCWD, (const char *)(uintptr_t)regs->ecx, 0);
              break;  /* link (-EPERM where the filesystem has none: tmpfs) */
    case 303: ret = sys_link_paths((int)regs->ebx, (const char *)(uintptr_t)regs->ecx,
                                   (int)regs->edx, (const char *)(uintptr_t)regs->esi,
                                   (int)regs->edi);
              break;  /* linkat */
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
    /* Signal-delivering timers (proc/ktimer.c) */
    case 27:  ret = sys_alarm(regs);           break;
    case 104: ret = sys_setitimer(regs);       break;
    case 105: ret = sys_getitimer(regs);       break;
    case 259: ret = sys_timer_create(regs);    break;
    case 260: ret = sys_timer_settime(regs);   break;
    case 261: ret = sys_timer_gettime(regs);   break;
    case 262: ret = sys_timer_getoverrun(regs); break;
    case 263: ret = sys_timer_delete(regs);    break;
    case 408: ret = sys_timer_gettime64(regs); break;
    case 409: ret = sys_timer_settime64(regs); break;
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
    case 208: ret = sys_setresuid(regs);       break;  /* setresuid32 */
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
    case 506: ret = shm_sys_chmod((int)regs->ebx, regs->ecx); break;
    case 503:  /* kprof: dump the cycle accounting (see include/kernel/kprof.h) */
        kprof_dump("mark");
        ret = 0;
        break;
    case 504:  /* kprof: reset the counters */
        kprof_reset();
        ret = 0;
        break;
    case 505:  /* register Ctrl+Alt+Backspace kill target (desktop only) */
        keyboard_set_kill_target((int)regs->ebx);
        ret = 0;
        break;

    case 296: ret = sys_mkdirat(regs);         break;
    case 297: ret = sys_mknodat(regs);         break;
    case 298: ret = sys_fchownat(regs);        break;
    case 304: ret = sys_symlinkat(regs);       break;
    case 300: ret = sys_fstatat64(regs);       break;
    case 301: ret = sys_unlinkat(regs);        break;
    case 302: ret = sys_renameat(regs);        break;
    case 305: ret = sys_readlinkat(regs);      break;
    case 353: ret = sys_renameat2(regs);       break;
    case 306: ret = sys_fchmodat(regs);        break;
    case 30:  ret = sys_utime(regs);           break;
    case 226: case 227: case 228: case 229: case 230: case 231:
    case 232: case 233: case 234: case 235: case 236: case 237:
              ret = sys_xattr(regs, (int)num); break;  /* *xattr */
    case 271: ret = regs->ebx ? do_futimesat(AT_FDCWD,
                  (const char *)(uintptr_t)regs->ebx,
                  (const void *)(uintptr_t)regs->ecx) : -14;   break;  /* utimes */
    case 299: ret = do_futimesat((int)regs->ebx,
                  (const char *)(uintptr_t)regs->ecx,
                  (const void *)(uintptr_t)regs->edx);         break;  /* futimesat */
    case 320: ret = sys_utimensat_common(regs, 0); break;
    case 412: ret = sys_utimensat_common(regs, 1); break;  /* utimensat_time64 */
    case 307: ret = sys_faccessat(regs);       break;
    case 439: ret = sys_faccessat2(regs);      break;
    case 212: ret = sys_chown(regs);           break;  /* chown32 (musl chown) */
    case 198: ret = sys_lchown(regs);          break;  /* lchown32 */
    case 8:   ret = sys_creat(regs);           break;
    case 80:  ret = sys_getgroups16(regs);     break;
    case 81:  ret = sys_setgroups16(regs);     break;
    case 205: ret = sys_getgroups(regs);       break;
    case 206: ret = sys_setgroups(regs);       break;
    default:
        /* Don't spam the log for common no-op syscalls */
        if (num != 174 && num != 175 && num != 176 &&
            num != 57 && num != 65 && num != 66 && num != 82 &&
            num != 85 && num != 132 && num != 172 && num != 186) {
            /* Report each unimplemented number ONCE.  Firefox calls getrusage
             * (77) in a loop, and an unconditional printk there emitted
             * thousands of identical lines per startup. */
            static uint8_t warned[512];
            uint32_t slot = num < 512 ? num : 511;
            if (!warned[slot]) {
                warned[slot] = 1;
                printk("[SYSCALL] unimplemented %u from pid %d\n",
                       (unsigned)num, current_proc ? current_proc->pid : -1);
            }
        }
        break;
    }

    kprof_probe_end(KPP_SYS_BODY, kp_body);
    regs->eax = (uint32_t)(int32_t)ret;

    uint64_t kp_epi = kprof_probe_begin();

    /* Deliver any pending signals before returning to user mode; an
     * interrupted blocking call is restarted or fails with EINTR here (the
     * syscall number lets the restart re-issue it). */
    signal_return_to_user(regs, (int)num);

    /* Linux-style wakeup preemption: if this syscall woke another thread, yield
     * at the return-to-user boundary so the woken thread runs promptly (closes
     * the glibc-2.36 condvar signal-steal window for the IPC Launch thread). */
    kprof_probe_end(KPP_SYS_EPI, kp_epi);
    uint64_t kp_rs = kprof_probe_begin();
    resched_on_return();
    kprof_probe_end(KPP_SYS_RESCHED, kp_rs);
}
