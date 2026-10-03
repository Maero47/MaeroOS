#include "procfs.h"
#include "../drivers/blkpart.h"
#include "vfs.h"
#include "../proc/process.h"
#include "../drivers/pci.h"
#include "../net/net.h"
#include "../net/lwip_glue.h"
#include "../net/firewall.h"
#include "../net/xsock.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../kernel/klog.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/mm/paging.h"
#include "../arch/i686/cpu/percpu.h"
#include "../arch/i686/cpu/smp.h"
#include "../drivers/rtc.h"
#include "../lib/printf.h"
#include "../proc/signal.h"
#include "../proc/sysvipc.h"
#include "inotify.h"
#include <stdarg.h>
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
        if (pid > 1000000) return -1;          /* no such pid; and no overflow */
        pid = pid * 10 + (*name - '0');
        name++;
    }
    return pid;
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

/* ── /proc/mounts, /proc/partitions ───────────────────────────────────────── */

static uint32_t procfs_text_window(char *content, uint32_t total, uint32_t off,
                                   uint32_t len, uint8_t *buf) {
    if (off >= total) return 0;
    uint32_t avail = total - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, content + off, avail);
    return avail;
}

static uint32_t procfs_mounts_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                   uint8_t *buf) {
    (void)n;
    enum { CAP = 8192 };
    char *content = (char *)kmalloc(CAP);
    if (!content) return 0;
    uint32_t r = procfs_text_window(content, vfs_mounts_format(content, CAP),
                                    off, len, buf);
    kfree(content);
    return r;
}

static uint32_t procfs_partitions_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                       uint8_t *buf) {
    (void)n;
    enum { CAP = 4096 };
    char *content = (char *)kmalloc(CAP);
    if (!content) return 0;
    uint32_t r = procfs_text_window(content, blkpart_format(content, CAP),
                                    off, len, buf);
    kfree(content);
    return r;
}

/* /proc/filesystems: what mount(2) takes.  busybox mount without -t tries
 * the types not marked nodev in this order, so vfat and exfat (which only
 * need their boot sector to say no) come before the ext family. */
static uint32_t procfs_filesystems_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                        uint8_t *buf) {
    (void)n;
    static const char text[] =
        "\tvfat\n\texfat\n\text4\n\text3\n\text2\n"
        "nodev\tmsdos\nnodev\ttmpfs\nnodev\tproc\nnodev\tdevtmpfs\n";
    char tmp[sizeof(text)];
    memcpy(tmp, text, sizeof(text));
    return procfs_text_window(tmp, sizeof(text) - 1, off, len, buf);
}

/* ── /proc/kmsg ───────────────────────────────────────────────────────────── */

static uint32_t procfs_kmsg_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                 uint8_t *buf) {
    (void)n;
    /* Snapshot the kernel ring buffer into a per-read buffer and serve the
     * requested window of it.  A shared static snapshot, refreshed only at
     * off == 0, let two concurrent readers overwrite each other's copy
     * mid-pass and return torn output (same fix as /proc/<pid>/maps). */
    enum { KMSG_CAP = 65536 };
    char *content = (char *)kmalloc(KMSG_CAP);
    if (!content) return 0;
    uint32_t content_len = klog_snapshot(content, KMSG_CAP);

    uint32_t avail = 0;
    if (off < content_len) {
        avail = content_len - off;
        if (avail > len) avail = len;
        __builtin_memcpy(buf, content + off, avail);
    }
    kfree(content);
    return avail;
}

/* Copy a byte range out of a captured buffer. */
static uint32_t procfs_copy_blob(const uint8_t *blob, uint32_t blob_len,
                                 uint32_t off, uint32_t len, uint8_t *buf) {
    if (off >= blob_len) return 0;
    uint32_t avail = blob_len - off;
    if (avail > len) avail = len;
    __builtin_memcpy(buf, blob + off, avail);
    return avail;
}

#include "procpid.inc"
#include "procsys.inc"

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
static vfs_node_t proc_cputime_node;
static vfs_node_t proc_kmsg_node;
static vfs_node_t proc_mounts_node;
static vfs_node_t proc_partitions_node;
static vfs_node_t proc_filesystems_node;
static vfs_node_t proc_stat_node;
static vfs_node_t proc_loadavg_node;
static vfs_node_t proc_vmstat_node;
static vfs_node_t proc_root_node;

/* ── /proc/cpuinfo ────────────────────────────────────────────────────────── */

static uint32_t procfs_cpuinfo_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                     uint8_t *buf) {
    (void)n;
    /* The flags line names only what the kernel turned on that a program
     * may want to know about: PAE paging and NX (wxprobe checks for "nx"). */
    char content[160];
    uint32_t total = 0;
    pappend(content, &total, sizeof(content),
            "processor\t: 0\n"
            "vendor_id\t: MaeroOS\n"
            "model name\t: i686\n"
            "cpu MHz\t\t: 1000\n"
            "bogomips\t: 2000.00\n"
            "flags\t\t: fpu");
    if (paging_pae) pappend(content, &total, sizeof(content), " pae");
    if (paging_nx)  pappend(content, &total, sizeof(content), " nx");
    pappend(content, &total, sizeof(content), "\n");
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
    /* Built per read (see /proc/kmsg): no shared buffer to tear. */
    enum { PCI_CAP = 4096 };
    char *content = (char *)kmalloc(PCI_CAP);
    uint32_t content_len;
    if (!content) return 0;

    {
        uint32_t pos = 0;
        int count = pci_device_count();
        for (int i = 0; i < count; i++) {
            const pci_device_t *d = pci_get_device(i);
            if (!d) continue;

            pappend_hex(content, &pos, PCI_CAP, d->bus, 2);
            pappend(content, &pos, PCI_CAP, ":");
            pappend_hex(content, &pos, PCI_CAP, d->slot, 2);
            pappend(content, &pos, PCI_CAP, ".");
            pappend_hex(content, &pos, PCI_CAP, d->func, 1);
            pappend(content, &pos, PCI_CAP, " ");
            pappend_hex(content, &pos, PCI_CAP, d->vendor_id, 4);
            pappend(content, &pos, PCI_CAP, ":");
            pappend_hex(content, &pos, PCI_CAP, d->device_id, 4);
            pappend(content, &pos, PCI_CAP, " class=");
            pappend_hex(content, &pos, PCI_CAP, d->class_code, 2);
            pappend(content, &pos, PCI_CAP, ":");
            pappend_hex(content, &pos, PCI_CAP, d->subclass, 2);
            pappend(content, &pos, PCI_CAP, " prog=");
            pappend_hex(content, &pos, PCI_CAP, d->prog_if, 2);
            pappend(content, &pos, PCI_CAP, " irq=");
            pappend_int(content, &pos, PCI_CAP, d->irq_line);
            pappend(content, &pos, PCI_CAP, " bar0=");
            pappend_hex(content, &pos, PCI_CAP, d->bar[0], 8);
            pappend(content, &pos, PCI_CAP, "\n");
        }
        if (count == 0)
            pappend(content, &pos, PCI_CAP, "no pci devices\n");
        if (pos >= PCI_CAP) pos = PCI_CAP - 1;
        content[pos] = '\0';
        content_len = pos;
    }

    uint32_t avail = procfs_copy_blob((const uint8_t *)content, content_len,
                                      off, len, buf);
    kfree(content);
    return avail;
}

/* ── /proc/netif ──────────────────────────────────────────────────────────── */

static uint32_t procfs_netif_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                  uint8_t *buf) {
    (void)n;
    char content[1024];
    uint32_t pos = 0;
    net_poll_all();

    /* One line per NIC: "<name>: <driver> up <driver details> txpkts=...
     * rxpkts=... drop=...", and lwIP's address on eth0, the interface it
     * runs on. */
    for (int i = 0; i < net_interface_count(); i++) {
        netif_t *iface = net_get_interface(i);
        char tmp[256];       /* driver details run to ~200 bytes */
        if (iface->loopback)
            continue;             /* NICs only; lo is in /proc/net/dev */
        pappend(content, &pos, sizeof(content), iface->name);
        pappend(content, &pos, sizeof(content), ": ");
        pappend(content, &pos, sizeof(content),
                iface->driver ? iface->driver : "nic");
        pappend(content, &pos, sizeof(content), " up ");
        if (iface->describe && iface->describe(iface, tmp, sizeof(tmp)) > 0)
            pappend(content, &pos, sizeof(content), tmp);
        pappend(content, &pos, sizeof(content), " txpkts=");
        pappend_int(content, &pos, sizeof(content), iface->tx_packets);
        pappend(content, &pos, sizeof(content), " rxpkts=");
        pappend_int(content, &pos, sizeof(content), iface->rx_packets);
        pappend(content, &pos, sizeof(content), " drop=");
        pappend_int(content, &pos, sizeof(content), iface->rx_dropped);
        if (strcmp(iface->name, "eth0") == 0 &&
            net_lwip_ipv4(tmp, sizeof(tmp)) > 0)
            pappend(content, &pos, sizeof(content), tmp);
        pappend(content, &pos, sizeof(content), "\n");
    }
    if (pos == 0)
        pappend(content, &pos, sizeof(content), "no network interfaces\n");
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
    enum { FW_CAP = 4096 };
    char *content = (char *)kmalloc(FW_CAP);
    if (!content) return 0;
    uint32_t pos = firewall_dump(content, FW_CAP);
    uint32_t avail = procfs_copy_blob((const uint8_t *)content, pos, off, len, buf);
    kfree(content);
    return avail;
}

/* Each write is one or more newline-separated control lines (fwctl).  A line
 * the firewall rejects fails the write with EINVAL, so fwctl (and a config
 * reload) can tell a bad rule from one that is now in force. */
static uint32_t procfs_firewall_write(vfs_node_t *n, uint32_t off, uint32_t len,
                                      const uint8_t *buf) {
    (void)n; (void)off;
    char line[256];
    uint32_t i = 0, l = 0;
    int bad = 0;
    while (i < len) {
        char c = (char)buf[i++];
        if (c == '\n' || l >= sizeof(line) - 1) {
            line[l] = '\0';
            if (l && firewall_ctl_line(line) < 0) bad = 1;
            l = 0;
            continue;
        }
        line[l++] = c;
    }
    if (l) { line[l] = '\0'; if (firewall_ctl_line(line) < 0) bad = 1; }
    return bad ? (uint32_t)-22 : len;   /* -EINVAL */
}

/* ── /proc/processes ─────────────────────────────────────────────────────── */

static uint32_t procfs_processes_read(vfs_node_t *n, uint32_t off,
                                      uint32_t len, uint8_t *buf) {
    (void)n;
    /* Built per read (see /proc/kmsg): no shared buffer to tear. */
    enum { PROCS_CAP = 8192 };
    char *content = (char *)kmalloc(PROCS_CAP);
    uint32_t content_len;
    if (!content) return 0;

    {
        uint32_t pos = 0;
        pappend(content, &pos, PROCS_CAP,
                "PID PPID PGRP SID STATE TTY TIME NAME\n");
        for (int i = 0; i < MAX_PROCS; i++) {
            struct proc *p = &ptable[i];
            if (p->state == PROC_UNUSED) continue;
            int ppid = p->parent ? p->parent->pid : 0;

            pappend_int(content, &pos, PROCS_CAP, p->pid);
            pappend(content, &pos, PROCS_CAP, " ");
            pappend_int(content, &pos, PROCS_CAP, ppid);
            pappend(content, &pos, PROCS_CAP, " ");
            pappend_int(content, &pos, PROCS_CAP, p->pgrp);
            pappend(content, &pos, PROCS_CAP, " ");
            pappend_int(content, &pos, PROCS_CAP, p->sid);
            pappend(content, &pos, PROCS_CAP, " ");
            pappend(content, &pos, PROCS_CAP, proc_state_short(p->state));
            pappend(content, &pos, PROCS_CAP, " ");
            pappend_tty(content, &pos, PROCS_CAP, p);
            pappend(content, &pos, PROCS_CAP, " ");
            /* CPU time consumed, in 1/100ths of a second (PIT ticks) */
            pappend_int(content, &pos, PROCS_CAP,
                        (int)(p->utime_ticks / 100));
            pappend(content, &pos, PROCS_CAP, ".");
            pappend_int(content, &pos, PROCS_CAP,
                        (int)(p->utime_ticks % 100));
            pappend(content, &pos, PROCS_CAP, " ");
            pappend(content, &pos, PROCS_CAP, p->name);
            pappend(content, &pos, PROCS_CAP, "\n");
        }
        if (pos >= PROCS_CAP) pos = PROCS_CAP - 1;
        content[pos] = '\0';
        content_len = pos;
    }

    uint32_t avail = procfs_copy_blob((const uint8_t *)content, content_len,
                                      off, len, buf);
    kfree(content);
    return avail;
}

/* Append an unsigned decimal. */
static void pappend_uint(char *buf, uint32_t *pos, uint32_t cap, uint32_t val) {
    char tmp[12];
    int i = 0;
    do { tmp[i++] = (char)('0' + val % 10u); val /= 10u; } while (val);
    while (i > 0 && *pos + 1 < cap) buf[(*pos)++] = tmp[--i];
}

/* ── /proc/cputime ───────────────────────────────────────────────────────────
 * Exact CPU time per thread, in microseconds, from the scheduler's dispatch
 * timestamps (not tick sampling), plus the time the CPUs sat idle:
 *   idle <us>
 *   handoffs <n>     (futex sync hand-offs the scheduler took)
 *   <pid> <tgid> <us> <name>
 * tools/bench_gfx.py reads it to split CPU between the desktop, maeroX and a
 * client; the counters only grow, so readers take differences. */
extern uint32_t sched_idle_us;
extern uint32_t sched_handoffs;

static uint32_t procfs_cputime_read(vfs_node_t *n, uint32_t off,
                                    uint32_t len, uint8_t *buf) {
    (void)n;
    enum { CAP = 8192 };
    char *content = (char *)kmalloc(CAP);
    uint32_t pos = 0;
    if (!content) return 0;
    pappend(content, &pos, CAP, "idle ");
    pappend_uint(content, &pos, CAP, sched_idle_us);
    pappend(content, &pos, CAP, "\n");
    pappend(content, &pos, CAP, "handoffs ");
    pappend_uint(content, &pos, CAP, sched_handoffs);
    pappend(content, &pos, CAP, "\n");
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_UNUSED) continue;
        pappend_int(content, &pos, CAP, p->pid);
        pappend(content, &pos, CAP, " ");
        pappend_int(content, &pos, CAP, p->tgid);
        pappend(content, &pos, CAP, " ");
        pappend_uint(content, &pos, CAP, p->run_us);
        pappend(content, &pos, CAP, " ");
        pappend(content, &pos, CAP, p->name);
        pappend(content, &pos, CAP, "\n");
    }
    if (pos >= CAP) pos = CAP - 1;
    uint32_t avail = procfs_copy_blob((const uint8_t *)content, pos, off, len, buf);
    kfree(content);
    return avail;
}

/* readdir for /proc: enumerate fixed pseudo-files */
/* ── /proc/net: dev and route, the files busybox ifconfig and route read ─── */
static vfs_node_t proc_net_node;
static vfs_node_t proc_net_dev_node;
static vfs_node_t proc_net_route_node;
static vfs_node_t proc_net_if_inet6_node;

static uint32_t procfs_net_text_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                     uint8_t *buf) {
    uint32_t cap = 4096;
    char *content = (char *)kmalloc(cap);
    if (!content) return 0;
    net_poll_all();
    uint32_t pos = n == &proc_net_dev_node ? netdev_proc_dev(content, cap)
                 : n == &proc_net_if_inet6_node ? netdev_proc_if_inet6(content, cap)
                 : netdev_proc_route(content, cap);
    uint32_t avail = 0;
    if (off < pos) {
        avail = pos - off;
        if (avail > len) avail = len;
        __builtin_memcpy(buf, content + off, avail);
    }
    kfree(content);
    return avail;
}

static int procfs_net_readdir(vfs_node_t *node, uint32_t idx, vfs_dirent_t *out) {
    (void)node;
    static const char *const names[] = { "dev", "route", "if_inet6" };
    if (idx > 2) return -1;
    out->ino  = 84 + idx;
    out->type = VFS_FLAG_FILE;
    strncpy(out->name, names[idx], 255);
    out->name[255] = '\0';
    return 0;
}

static vfs_node_t *procfs_net_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    if (strcmp(name, "dev") == 0) return &proc_net_dev_node;
    if (strcmp(name, "route") == 0) return &proc_net_route_node;
    if (strcmp(name, "if_inet6") == 0) return &proc_net_if_inet6_node;
    return NULL;
}

static int procfs_root_readdir(vfs_node_t *node, uint32_t idx,
                                vfs_dirent_t *out) {
    (void)node;
    static const struct { const char *name; uint32_t type; uint32_t ino; } entries[] = {
        { "version", VFS_FLAG_FILE, 1 },
        { "self",    VFS_FLAG_SYMLINK, 2 },
        { "thread-self", VFS_FLAG_SYMLINK, 20 },
        { "sys",     VFS_FLAG_DIR,  79 },
        { "stat",    VFS_FLAG_FILE, 21 },
        { "loadavg", VFS_FLAG_FILE, 22 },
        { "vmstat",  VFS_FLAG_FILE, 23 },
        { "sysvipc", VFS_FLAG_DIR,  24 },
        { "meminfo", VFS_FLAG_FILE, 3 },
        { "cpuinfo", VFS_FLAG_FILE, 4 },
        { "pci",     VFS_FLAG_FILE, 5 },
        { "netif",   VFS_FLAG_FILE, 6 },
        { "processes", VFS_FLAG_FILE, 7 },
        { "kmsg",    VFS_FLAG_FILE, 8 },
        { "firewall", VFS_FLAG_FILE, 9 },
        { "uptime",  VFS_FLAG_FILE, 10 },
        { "mounts",  VFS_FLAG_FILE, 11 },
        { "partitions", VFS_FLAG_FILE, 12 },
        { "net",     VFS_FLAG_DIR,  83 },
        { "cputime", VFS_FLAG_FILE, 19 },
        { "filesystems", VFS_FLAG_FILE, 82 },
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
        /* One directory per process (thread-group leader); threads are
         * reachable by tid but listed only under task/ (Linux). */
        if (p->state == PROC_UNUSED || p->pid != p->tgid) continue;
        if (found++ != want) continue;

        uint32_t pos = 0;
        out->ino = pp_inode(p->pid, PK_DIR, 0, 0);
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
    if (strcmp(name, "self")    == 0) return &proc_selflink_node;
    if (strcmp(name, "thread-self") == 0) return &proc_threadself_node;
    if (strcmp(name, "sys")     == 0) return &proc_sys_node;
    if (strcmp(name, "stat")    == 0) return &proc_stat_node;
    if (strcmp(name, "loadavg") == 0) return &proc_loadavg_node;
    if (strcmp(name, "vmstat")  == 0) return &proc_vmstat_node;
    if (strcmp(name, "sysvipc") == 0) return &proc_sysvipc_node;
    if (strcmp(name, "meminfo") == 0) return &proc_meminfo_node;
    if (strcmp(name, "cpuinfo") == 0) return &proc_cpuinfo_node;
    if (strcmp(name, "uptime")  == 0) return &proc_uptime_node;
    if (strcmp(name, "pci")     == 0) return &proc_pci_node;
    if (strcmp(name, "netif")   == 0) return &proc_netif_node;
    if (strcmp(name, "firewall") == 0) return &proc_firewall_node;
    if (strcmp(name, "processes") == 0) return &proc_processes_node;
    if (strcmp(name, "cputime") == 0) return &proc_cputime_node;
    if (strcmp(name, "kmsg")    == 0) return &proc_kmsg_node;
    if (strcmp(name, "mounts")  == 0) return &proc_mounts_node;
    if (strcmp(name, "partitions") == 0) return &proc_partitions_node;
    if (strcmp(name, "net")     == 0) return &proc_net_node;
    if (strcmp(name, "filesystems") == 0) return &proc_filesystems_node;

    int pid = procfs_parse_pid(name);
    if (pid > 0) {
        struct proc *p = procfs_find_pid(pid);
        if (p) return procfs_pid_node(pid, 0);
    }
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

vfs_node_t *procfs_mount(void) {
    memset(&proc_mounts_node, 0, sizeof(proc_mounts_node));
    strncpy(proc_mounts_node.name, "mounts", 255);
    proc_mounts_node.flags   = VFS_FLAG_FILE;
    proc_mounts_node.inode   = 11;
    proc_mounts_node.mask    = 0444;
    proc_mounts_node.read_fn = procfs_mounts_read;

    memset(&proc_partitions_node, 0, sizeof(proc_partitions_node));
    strncpy(proc_partitions_node.name, "partitions", 255);
    proc_partitions_node.flags   = VFS_FLAG_FILE;
    proc_partitions_node.inode   = 12;
    proc_partitions_node.mask    = 0444;
    proc_partitions_node.read_fn = procfs_partitions_read;

    memset(&proc_filesystems_node, 0, sizeof(proc_filesystems_node));
    strncpy(proc_filesystems_node.name, "filesystems", 255);
    proc_filesystems_node.flags   = VFS_FLAG_FILE;
    proc_filesystems_node.inode   = 82;
    proc_filesystems_node.mask    = 0444;
    proc_filesystems_node.read_fn = procfs_filesystems_read;

    /* /proc/version */
    memset(&proc_version_node, 0, sizeof(proc_version_node));
    strncpy(proc_version_node.name, "version", 255);
    proc_version_node.flags   = VFS_FLAG_FILE;
    proc_version_node.inode   = 1;
    proc_version_node.read_fn = procfs_version_read;

    /* /proc/self and /proc/thread-self: links into /proc/<pid>. */
    memset(&proc_selflink_node, 0, sizeof(proc_selflink_node));
    strncpy(proc_selflink_node.name, "self", 255);
    proc_selflink_node.flags   = VFS_FLAG_SYMLINK;
    proc_selflink_node.inode   = 2;
    proc_selflink_node.mask    = 0777;
    proc_selflink_node.read_fn = procfs_selflink_read;
    memset(&proc_threadself_node, 0, sizeof(proc_threadself_node));
    strncpy(proc_threadself_node.name, "thread-self", 255);
    proc_threadself_node.flags   = VFS_FLAG_SYMLINK;
    proc_threadself_node.inode   = 20;
    proc_threadself_node.mask    = 0777;
    proc_threadself_node.read_fn = procfs_selflink_read;

    /* /proc/stat, /proc/loadavg, /proc/vmstat */
    memset(&proc_stat_node, 0, sizeof(proc_stat_node));
    strncpy(proc_stat_node.name, "stat", 255);
    proc_stat_node.flags   = VFS_FLAG_FILE;
    proc_stat_node.inode   = 21;
    proc_stat_node.mask    = 0444;
    proc_stat_node.read_fn = procfs_stat_sys_read;
    memset(&proc_loadavg_node, 0, sizeof(proc_loadavg_node));
    strncpy(proc_loadavg_node.name, "loadavg", 255);
    proc_loadavg_node.flags   = VFS_FLAG_FILE;
    proc_loadavg_node.inode   = 22;
    proc_loadavg_node.mask    = 0444;
    proc_loadavg_node.read_fn = procfs_loadavg_read;
    memset(&proc_vmstat_node, 0, sizeof(proc_vmstat_node));
    strncpy(proc_vmstat_node.name, "vmstat", 255);
    proc_vmstat_node.flags   = VFS_FLAG_FILE;
    proc_vmstat_node.inode   = 23;
    proc_vmstat_node.mask    = 0444;
    proc_vmstat_node.read_fn = procfs_vmstat_read;
    sysctl_init();
    sysvipc_dir_init();

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

    /* /proc/cputime */
    memset(&proc_cputime_node, 0, sizeof(proc_cputime_node));
    strncpy(proc_cputime_node.name, "cputime", 255);
    proc_cputime_node.flags   = VFS_FLAG_FILE;
    proc_cputime_node.inode   = 19;
    proc_cputime_node.read_fn = procfs_cputime_read;

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
    memset(&proc_net_node, 0, sizeof(proc_net_node));
    strncpy(proc_net_node.name, "net", 255);
    proc_net_node.flags      = VFS_FLAG_DIR;
    proc_net_node.inode      = 83;
    proc_net_node.readdir_fn = procfs_net_readdir;
    proc_net_node.finddir_fn = procfs_net_finddir;
    memset(&proc_net_dev_node, 0, sizeof(proc_net_dev_node));
    strncpy(proc_net_dev_node.name, "dev", 255);
    proc_net_dev_node.flags   = VFS_FLAG_FILE;
    proc_net_dev_node.inode   = 84;
    proc_net_dev_node.mask    = 0444;
    proc_net_dev_node.read_fn = procfs_net_text_read;
    memset(&proc_net_route_node, 0, sizeof(proc_net_route_node));
    strncpy(proc_net_route_node.name, "route", 255);
    proc_net_route_node.flags   = VFS_FLAG_FILE;
    proc_net_route_node.inode   = 85;
    proc_net_route_node.mask    = 0444;
    proc_net_route_node.read_fn = procfs_net_text_read;
    memset(&proc_net_if_inet6_node, 0, sizeof(proc_net_if_inet6_node));
    strncpy(proc_net_if_inet6_node.name, "if_inet6", 255);
    proc_net_if_inet6_node.flags   = VFS_FLAG_FILE;
    proc_net_if_inet6_node.inode   = 86;
    proc_net_if_inet6_node.mask    = 0444;
    proc_net_if_inet6_node.read_fn = procfs_net_text_read;

    return &proc_root_node;
}
