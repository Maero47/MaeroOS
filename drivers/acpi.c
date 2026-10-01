/*
 * ACPI glue for the vendored uACPI interpreter (third_party/uacpi, MIT).
 *
 *   - uacpi_kernel_* host API: memory mapping, port I/O, PCI config space,
 *     heap, time, locks, events, the SCI and deferred work;
 *   - acpi_init(): RSDP scan, tables, namespace load + init, SCI hookup and
 *     a summary of FADT / MADT / HPET on the log;
 *   - acpi_poweroff() / acpi_reboot(): S5 and the FADT reset register, behind
 *     reboot(2) (proc/syscall.c);
 *   - the power button: a fixed event that kacpid turns into SIGUSR2 for init
 *     (busybox init's "power off" signal), provided init handles it.
 *
 * Locking: every kernel path runs under the Big Kernel Lock, so the uACPI
 * mutexes below only have to cope with a holder that sleeps (yield/sleep_on
 * hand the BKL over); interrupt-time paths use the spinlocks with IF=0.
 */
#include "acpi.h"
#include <uacpi/uacpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/sleep.h>
#include <uacpi/status.h>
#include <io.h>
#include <kernel/config.h>
#include "../kernel/printk.h"
#include "../mm/heap.h"
#include "../arch/i686/mm/paging.h"
#include "../arch/i686/cpu/irq.h"
#include "../arch/i686/cpu/pic.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/cpu/tsc.h"
#include "../arch/i686/cpu/spinlock.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "../proc/signal.h"
#include "../lib/string.h"

static int acpi_ready;            /* namespace loaded, ACPI mode entered */
static uint32_t madt_cpus;
static volatile int kacpid_running;   /* the scheduler is up: sleeping is legal */

int acpi_available(void) { return acpi_ready; }
uint32_t acpi_madt_cpu_count(void) { return madt_cpus; }

/* ── Waiting ─────────────────────────────────────────────────────────────── */

/* Busy-wait about `us` microseconds: a port-0x80 write takes ~1 us on every
 * PC and needs no calibrated clock (acpi_init runs before kacpid exists). */
static void acpi_udelay(uint32_t us) {
    while (us--) io_wait();
}

/* Give the CPU away for a moment when that is possible, else spin briefly. */
static void acpi_relax(void) {
    if (kacpid_running && current_proc) yield();
    else acpi_udelay(10);
}

static void acpi_msleep(uint32_t ms) {
    if (kacpid_running && current_proc) {
        static int sleep_chan;     /* nobody wakes it: only the deadline does */
        uint32_t ticks = (ms + 9) / 10;
        current_proc->wake_tick = pit_ticks() + (ticks ? ticks : 1);
        sleep_on(&sleep_chan);
    } else {
        acpi_udelay(ms * 1000);
    }
}

/* ── RSDP ────────────────────────────────────────────────────────────────── */

static int rsdp_valid(const uint8_t *p) {
    if (memcmp(p, "RSD PTR ", 8) != 0) return 0;
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) sum += p[i];      /* ACPI 1.0 part */
    return sum == 0;
}

/* ACPI 6.5 §5.2.5.1: the first 1 KiB of the EBDA, then the BIOS ROM area
 * 0xE0000-0xFFFFF, on 16-byte boundaries.  Both are below 1 MiB, inside
 * boot.asm's direct map at KERNEL_VMA. */
static uint32_t rsdp_scan(uint32_t start, uint32_t len) {
    for (uint32_t a = start & ~15U; a + 20 <= start + len; a += 16)
        if (rsdp_valid((const uint8_t *)(uintptr_t)(a + KERNEL_VMA)))
            return a;
    return 0;
}

static uint32_t rsdp_phys;

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) {
    if (!rsdp_phys) {
        uint32_t ebda = (uint32_t)*(volatile uint16_t *)(uintptr_t)(0x40E + KERNEL_VMA) << 4;
        if (ebda >= 0x80000 && ebda < 0xA0000) rsdp_phys = rsdp_scan(ebda, 1024);
        if (!rsdp_phys) rsdp_phys = rsdp_scan(0xE0000, 0x20000);
    }
    if (!rsdp_phys) return UACPI_STATUS_NOT_FOUND;
    *out = rsdp_phys;
    return UACPI_STATUS_OK;
}

/* ── Physical memory mapping ─────────────────────────────────────────────── */

/*
 * A dedicated window of kernel address space (config.h ACPI_MAP_START/END).
 * Mappings are cached and never torn down: uACPI maps the same handful of
 * ranges (tables, SystemMemory operation regions) again and again, and never
 * reusing a virtual address means no other CPU can hold a stale TLB entry for
 * it, which this kernel has no shootdown for.  Everything is mapped uncached,
 * since an operation region may be device MMIO.
 */
#define ACPI_MAP_SLOTS 128
static struct { uint32_t phys, pages, virt; } map_cache[ACPI_MAP_SLOTS];
static int map_count;
static uint32_t map_next = ACPI_MAP_START;
static spinlock_t map_lock;

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    if (addr > 0xFFFFFFFFULL || len == 0) return NULL;
    uint32_t phys = (uint32_t)addr;
    uint32_t base = phys & ~0xFFFU;
    uint64_t end64 = ((uint64_t)phys + len + 0xFFF) & ~0xFFFULL;
    if (end64 > 0x100000000ULL) return NULL;
    uint32_t pages = (uint32_t)((end64 - base) >> 12);

    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&map_lock);
    void *ret = NULL;
    for (int i = 0; i < map_count; i++) {
        if (base >= map_cache[i].phys &&
            base + pages * 4096 <= map_cache[i].phys + map_cache[i].pages * 4096) {
            ret = (void *)(uintptr_t)(map_cache[i].virt + (phys - map_cache[i].phys));
            goto out;
        }
    }
    if (map_count == ACPI_MAP_SLOTS || pages > (ACPI_MAP_END - map_next) / 4096) {
        printk("[ACPI] map window full: cannot map 0x%08x+%u\n",
               (unsigned)phys, (unsigned)len);
        goto out;
    }
    uint32_t virt = map_next;
    for (uint32_t i = 0; i < pages; i++) {
        /* Cannot fail: acpi_init reserved every page table of the window. */
        if (paging_map(virt + i * 4096, base + i * 4096,
                       PAGE_PRESENT | PAGE_WRITABLE | PAGE_NOCACHE) != 0)
            goto out;
    }
    map_next += pages * 4096;
    map_cache[map_count].phys = base;
    map_cache[map_count].pages = pages;
    map_cache[map_count].virt = virt;
    map_count++;
    ret = (void *)(uintptr_t)(virt + (phys - base));
out:
    spin_unlock(&map_lock);
    if (flags & 0x200) __asm__ volatile("sti");
    return ret;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    (void)addr; (void)len;      /* cached for good, see above */
}

/* ── Logging ─────────────────────────────────────────────────────────────── */

void uacpi_kernel_log(uacpi_log_level lvl, const uacpi_char *msg) {
    const char *tag = lvl <= UACPI_LOG_ERROR ? "error: " :
                      lvl == UACPI_LOG_WARN  ? "warning: " : "";
    printk("[ACPI] %s%s", tag, msg);
}

/* ── Port I/O ────────────────────────────────────────────────────────────── */

/* The handle is the base port itself; +1 keeps port 0 from looking NULL. */
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len,
                                 uacpi_handle *out) {
    if (base > 0xFFFF || len > 0x10000 - base) return UACPI_STATUS_INVALID_ARGUMENT;
    *out = (uacpi_handle)(uintptr_t)(base + 1);
    return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle h) { (void)h; }

#define IO_PORT(h, off) ((uint16_t)((uintptr_t)(h) - 1 + (off)))
uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v) {
    *v = inb(IO_PORT(h, off)); return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v) {
    *v = inw(IO_PORT(h, off)); return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v) {
    *v = inl(IO_PORT(h, off)); return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v) {
    outb(IO_PORT(h, off), v); return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v) {
    outw(IO_PORT(h, off), v); return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v) {
    outl(IO_PORT(h, off), v); return UACPI_STATUS_OK;
}

/* ── PCI configuration space (mechanism #1, segment 0 only) ──────────────── */

static spinlock_t pci_lock;

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle *out) {
    if (a.segment != 0 || a.device > 31 || a.function > 7)
        return UACPI_STATUS_UNIMPLEMENTED;
    *out = (uacpi_handle)(uintptr_t)(0x80000000U | ((uint32_t)a.bus << 16) |
                                     ((uint32_t)a.device << 11) |
                                     ((uint32_t)a.function << 8));
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle h) { (void)h; }

/* Select the dword holding `off`; returns the data port for it.  Called with
 * pci_lock held and IF=0. */
static uint16_t pci_select(uacpi_handle h, uacpi_size off) {
    outl(0xCF8, (uint32_t)(uintptr_t)h | ((uint32_t)off & 0xFC));
    return (uint16_t)(0xCFC + (off & 3));
}

#define PCI_ACCESS(stmt) do {                                              \
        if (off > 0xFF) return UACPI_STATUS_INVALID_ARGUMENT;              \
        uint32_t fl;                                                       \
        __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");     \
        spin_lock(&pci_lock);                                              \
        uint16_t port = pci_select(h, off);                                \
        stmt;                                                              \
        spin_unlock(&pci_lock);                                            \
        if (fl & 0x200) __asm__ volatile("sti");                           \
        return UACPI_STATUS_OK;                                            \
    } while (0)

uacpi_status uacpi_kernel_pci_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v) {
    PCI_ACCESS(*v = inb(port));
}
uacpi_status uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v) {
    PCI_ACCESS(*v = inw(port));
}
uacpi_status uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v) {
    PCI_ACCESS(*v = inl(port));
}
uacpi_status uacpi_kernel_pci_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v) {
    PCI_ACCESS(outb(port, v));
}
uacpi_status uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v) {
    PCI_ACCESS(outw(port, v));
}
uacpi_status uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v) {
    PCI_ACCESS(outl(port, v));
}

/* ── Heap, time ──────────────────────────────────────────────────────────── */

void *uacpi_kernel_alloc(uacpi_size size) { return kmalloc(size ? size : 1); }
void uacpi_kernel_free(void *mem) { if (mem) kfree(mem); }

/* The tick-based clock_mono_ns() stands still until interrupts are enabled,
 * after acpi_init; the TSC, calibrated by tsc_init just before, does not. */
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    uint32_t cpt = tsc_cycles_per_tick();
    if (!cpt) return clock_mono_ns();
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | lo;
    const uint64_t tick_ns = 1000000000ULL / 100;     /* pit_init(100) */
    return (tsc / cpt) * tick_ns + (tsc % cpt) * tick_ns / cpt;
}
void uacpi_kernel_stall(uacpi_u8 usec) { acpi_udelay(usec); }
void uacpi_kernel_sleep(uacpi_u64 msec) {
    acpi_msleep(msec > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)msec);
}

/* ── Interrupt state, thread id, spinlocks ───────────────────────────────── */

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    return fl;
}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state st) {
    if (st & 0x200) __asm__ volatile("sti" ::: "memory");
}

/* NULL (boot, before any process runs) is a valid id; only -1 is reserved. */
uacpi_thread_id uacpi_kernel_get_thread_id(void) { return current_proc; }

uacpi_handle uacpi_kernel_create_spinlock(void) {
    spinlock_t *l = kmalloc(sizeof(*l));
    if (l) spin_init(l);
    return l;
}
void uacpi_kernel_free_spinlock(uacpi_handle h) { kfree(h); }
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h) {
    uacpi_cpu_flags fl = uacpi_kernel_disable_interrupts();
    spin_lock((spinlock_t *)h);
    return fl;
}
void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags fl) {
    spin_unlock((spinlock_t *)h);
    uacpi_kernel_restore_interrupts(fl);
}

/* ── Mutexes and events ──────────────────────────────────────────────────── */

/* Timeouts are in ms; 0xFFFF waits for ever.  pit_ticks() only advances once
 * the PIT runs, so before that the deadline is counted in relax() rounds. */
typedef struct { uint32_t start, ticks, spins; uint16_t ms; } acpi_deadline_t;
static void deadline_start(acpi_deadline_t *d, uint16_t ms) {
    d->start = pit_ticks(); d->ticks = (ms + 9u) / 10u; d->spins = 0; d->ms = ms;
}
static int deadline_passed(acpi_deadline_t *d) {
    if (d->ms == 0xFFFF) return 0;
    if (d->ms == 0) return 1;
    if (kacpid_running) return pit_ticks() - d->start >= d->ticks;
    return ++d->spins >= (uint32_t)d->ms * 100u;   /* relax() spins ~10 us */
}

typedef struct { volatile uint32_t locked; } acpi_mutex_t;

uacpi_handle uacpi_kernel_create_mutex(void) {
    return kcalloc(1, sizeof(acpi_mutex_t));
}
void uacpi_kernel_free_mutex(uacpi_handle h) { kfree(h); }

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h, uacpi_u16 timeout) {
    acpi_mutex_t *m = h;
    acpi_deadline_t d;
    deadline_start(&d, timeout);
    while (__sync_lock_test_and_set(&m->locked, 1)) {
        if (deadline_passed(&d)) return UACPI_STATUS_TIMEOUT;
        acpi_relax();
    }
    return UACPI_STATUS_OK;
}
void uacpi_kernel_release_mutex(uacpi_handle h) {
    __sync_lock_release(&((acpi_mutex_t *)h)->locked);
}

typedef struct { volatile int32_t count; } acpi_event_t;

uacpi_handle uacpi_kernel_create_event(void) {
    return kcalloc(1, sizeof(acpi_event_t));
}
void uacpi_kernel_free_event(uacpi_handle h) { kfree(h); }

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 timeout) {
    acpi_event_t *e = h;
    acpi_deadline_t d;
    deadline_start(&d, timeout);
    for (;;) {
        int32_t c = e->count;
        if (c > 0 && __sync_bool_compare_and_swap(&e->count, c, c - 1))
            return UACPI_TRUE;
        if (c <= 0) {
            if (deadline_passed(&d)) return UACPI_FALSE;
            acpi_relax();
        }
    }
}
void uacpi_kernel_signal_event(uacpi_handle h) {
    __sync_fetch_and_add(&((acpi_event_t *)h)->count, 1);
}
void uacpi_kernel_reset_event(uacpi_handle h) {
    ((acpi_event_t *)h)->count = 0;
}

/* ── Firmware requests (AML Breakpoint / Fatal) ──────────────────────────── */

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req) {
    if (req->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        printk("[ACPI] firmware Fatal(type 0x%x, code 0x%x) — ignored\n",
               (unsigned)req->fatal.type, (unsigned)req->fatal.code);
    return UACPI_STATUS_OK;
}

/* ── The SCI ─────────────────────────────────────────────────────────────── */

/* uACPI installs exactly one interrupt handler (the SCI). */
static uacpi_interrupt_handler sci_handler;
static uacpi_handle sci_ctx;
static uint32_t sci_irq = 0xFFFFFFFFU;

static void sci_trampoline(registers_t *regs) {
    (void)regs;
    if (sci_handler) sci_handler(sci_ctx);
}

uacpi_status uacpi_kernel_install_interrupt_handler(
        uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
        uacpi_handle *out) {
    if (irq >= 16 || sci_handler) return UACPI_STATUS_INVALID_ARGUMENT;
    sci_handler = handler;
    sci_ctx = ctx;
    sci_irq = irq;
    irq_install_handler((uint8_t)irq, sci_trampoline);
    pic_unmask((uint8_t)irq);
    *out = (uacpi_handle)(uintptr_t)(irq + 1);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
        uacpi_interrupt_handler handler, uacpi_handle h) {
    (void)handler; (void)h;
    if (sci_irq < 16) irq_remove_handler((uint8_t)sci_irq);
    sci_handler = NULL;
    sci_irq = 0xFFFFFFFFU;
    return UACPI_STATUS_OK;
}

/* ── Deferred work: kacpid ───────────────────────────────────────────────── */

/* GPE methods (_Lxx/_Exx) and Notify handlers are queued from the SCI and run
 * by the kacpid kernel thread, never in interrupt context. */
#define WORK_QUEUE 32
static struct { uacpi_work_handler fn; uacpi_handle ctx; } work_q[WORK_QUEUE];
static volatile uint32_t work_head, work_tail;   /* tail: next to run */
static volatile int work_busy;
static spinlock_t work_lock;
static int kacpid_chan;
static volatile int power_button_pressed;
static struct proc *kacpid_proc;

static int work_pop(uacpi_work_handler *fn, uacpi_handle *ctx) {
    uint32_t fl = uacpi_kernel_disable_interrupts();
    spin_lock(&work_lock);
    int got = work_tail != work_head;
    if (got) {
        *fn = work_q[work_tail % WORK_QUEUE].fn;
        *ctx = work_q[work_tail % WORK_QUEUE].ctx;
        work_tail++;
        work_busy = 1;
    }
    spin_unlock(&work_lock);
    uacpi_kernel_restore_interrupts(fl);
    return got;
}

static void work_drain(void) {
    uacpi_work_handler fn;
    uacpi_handle ctx;
    while (work_pop(&fn, &ctx)) {
        fn(ctx);
        work_busy = 0;
    }
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,
                                        uacpi_work_handler fn, uacpi_handle ctx) {
    (void)type;
    uint32_t fl = uacpi_kernel_disable_interrupts();
    spin_lock(&work_lock);
    int full = work_head - work_tail >= WORK_QUEUE;
    if (!full) {
        work_q[work_head % WORK_QUEUE].fn = fn;
        work_q[work_head % WORK_QUEUE].ctx = ctx;
        work_head++;
    }
    spin_unlock(&work_lock);
    uacpi_kernel_restore_interrupts(fl);
    if (full) return UACPI_STATUS_OUT_OF_MEMORY;
    if (kacpid_running) wake_up(&kacpid_chan);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    if (!kacpid_running || current_proc == kacpid_proc) {
        work_drain();                  /* nobody else will run it */
        return UACPI_STATUS_OK;
    }
    while (work_tail != work_head || work_busy) acpi_relax();
    return UACPI_STATUS_OK;
}

/* Linux's ACPI button driver reports the press to user space; busybox init
 * powers off on SIGUSR2, and so does ours.  Init's default action for an
 * unhandled signal would kill it, so only signal an init that catches it. */
static void notify_init_power_button(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PROC_UNUSED || p->pid != 1) continue;
        sighandler_t h = p->sighand ? p->sighand->handlers[SIGUSR2] : SIG_DFL;
        if (h == SIG_DFL || h == SIG_IGN) {
            printk("[ACPI] power button: init does not handle SIGUSR2\n");
            return;
        }
        printk("[ACPI] power button: sending SIGUSR2 to init\n");
        signal_send_group(p, SIGUSR2);
        return;
    }
}

static void kacpid(void) {
    kacpid_running = 1;
    for (;;) {
        work_drain();
        if (power_button_pressed) {
            power_button_pressed = 0;
            notify_init_power_button();
        }
        if (work_tail == work_head && !power_button_pressed) {
            current_proc->wake_tick = pit_ticks() + 100;   /* 1 s backstop */
            sleep_on(&kacpid_chan);
        }
    }
}

static uacpi_interrupt_ret power_button_event(uacpi_handle ctx) {
    (void)ctx;
    power_button_pressed = 1;
    if (kacpid_running) wake_up(&kacpid_chan);
    return UACPI_INTERRUPT_HANDLED;
}

/* ── Boot ────────────────────────────────────────────────────────────────── */

static uacpi_iteration_decision madt_entry(uacpi_handle user,
                                           struct acpi_entry_hdr *e) {
    (void)user;
    if (e->type == ACPI_MADT_ENTRY_TYPE_LAPIC) {
        struct acpi_madt_lapic *l = (void *)e;
        if (l->flags & (ACPI_PIC_ENABLED | ACPI_PIC_ONLINE_CAPABLE)) madt_cpus++;
    } else if (e->type == ACPI_MADT_ENTRY_TYPE_IOAPIC) {
        struct acpi_madt_ioapic *io = (void *)e;
        printk("[ACPI] MADT: I/O APIC id %u at 0x%08x, GSI base %u\n",
               (unsigned)io->id, (unsigned)io->address, (unsigned)io->gsi_base);
    } else if (e->type == ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE) {
        struct acpi_madt_interrupt_source_override *o = (void *)e;
        printk("[ACPI] MADT: ISA IRQ %u -> GSI %u (flags 0x%x)\n",
               (unsigned)o->source, (unsigned)o->gsi, (unsigned)o->flags);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static void log_tables(void) {
    struct acpi_fadt *fadt;
    if (uacpi_table_fadt(&fadt) == UACPI_STATUS_OK) {
        printk("[ACPI] FADT rev %u: SCI IRQ %u, PM1a_CNT 0x%x, PM1b_CNT 0x%x, "
               "reset reg %s 0x%x (value 0x%x)\n",
               (unsigned)fadt->hdr.revision, (unsigned)fadt->sci_int,
               (unsigned)fadt->x_pm1a_cnt_blk.address,
               (unsigned)fadt->x_pm1b_cnt_blk.address,
               (fadt->flags & ACPI_RESET_REG_SUP) ? "supported," : "absent,",
               (unsigned)fadt->reset_reg.address, (unsigned)fadt->reset_value);
    }
    uacpi_table t;
    if (uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &t) == UACPI_STATUS_OK) {
        struct acpi_madt *madt = t.ptr;
        uacpi_for_each_subtable(t.hdr, sizeof(*madt), madt_entry, NULL);
        printk("[ACPI] MADT: LAPIC at 0x%08x, %u processor(s), %s8259 PICs\n",
               (unsigned)madt->local_interrupt_controller_address,
               (unsigned)madt_cpus, (madt->flags & 1) ? "dual " : "no ");
        uacpi_table_unref(&t);
    }
    if (uacpi_table_find_by_signature(ACPI_HPET_SIGNATURE, &t) == UACPI_STATUS_OK) {
        struct acpi_hpet *hpet = t.ptr;
        printk("[ACPI] HPET: block at 0x%08x, %u comparators\n",
               (unsigned)hpet->address.address,
               (unsigned)(((hpet->block_id >> 8) & 0x1F) + 1));
        uacpi_table_unref(&t);
    }
}

#define ACPI_TRY(call) do {                                                  \
        uacpi_status st_ = (call);                                           \
        if (uacpi_unlikely_error(st_)) {                                     \
            printk("[ACPI] %s failed: %s\n", #call,                          \
                   uacpi_status_to_string(st_));                             \
            return;                                                          \
        }                                                                    \
    } while (0)

void acpi_init(void) {
    spin_init(&map_lock);
    spin_init(&pci_lock);
    spin_init(&work_lock);
    if (paging_reserve_range(ACPI_MAP_START, ACPI_MAP_END, 0) != 0) {
        printk("[ACPI] no memory for the mapping window's page tables\n");
        return;
    }
    uacpi_phys_addr rsdp;
    if (uacpi_kernel_get_rsdp(&rsdp) != UACPI_STATUS_OK) {
        printk("[ACPI] no RSDP found — ACPI disabled\n");
        return;
    }
    printk("[ACPI] RSDP at 0x%08x\n", (unsigned)rsdp_phys);

    ACPI_TRY(uacpi_initialize(0));
    log_tables();
    ACPI_TRY(uacpi_namespace_load());
    ACPI_TRY(uacpi_namespace_initialize());
    ACPI_TRY(uacpi_finalize_gpe_initialization());
    acpi_ready = 1;

    uacpi_status st = uacpi_install_fixed_event_handler(
        UACPI_FIXED_EVENT_POWER_BUTTON, power_button_event, NULL);
    if (st != UACPI_STATUS_OK)
        printk("[ACPI] power button: %s\n", uacpi_status_to_string(st));

    printk("[ACPI] ready (uACPI %u.%u.%u)\n",
           UACPI_MAJOR, UACPI_MINOR, UACPI_PATCH);
}

void acpi_start_thread(void) {
    if (!acpi_ready) return;
    kacpid_proc = proc_create_kthread(kacpid, "kacpid");
    if (!kacpid_proc)
        printk("[ACPI] cannot start kacpid; GPEs and the power button are dead\n");
}

/* ── Power off / reboot ──────────────────────────────────────────────────── */

void acpi_poweroff(void) {
    printk("[ACPI] powering off\n");
    if (acpi_ready) {
        uacpi_status st = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
        if (st != UACPI_STATUS_OK)
            printk("[ACPI] prepare for S5: %s\n", uacpi_status_to_string(st));
        __asm__ volatile("cli");
        st = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
        /* Only reached if the write to PM1 control did not take. */
        acpi_udelay(100000);
        printk("[ACPI] S5 failed: %s\n", uacpi_status_to_string(st));
    }
    /* Without (working) ACPI: the PM1a control ports emulators are known to
     * use, with their fixed S5 sleep types. */
    __asm__ volatile("cli");
    outw(0x604, 0x2000);     /* QEMU (SeaBIOS PM base 0x600) */
    outw(0xB004, 0x2000);    /* Bochs / older QEMU */
    outw(0x4004, 0x3400);    /* VirtualBox */
    printk("[ACPI] power off failed — halting\n");
    for (;;) __asm__ volatile("cli; hlt");
}

void acpi_reboot(void) {
    printk("[ACPI] restarting\n");
    __asm__ volatile("cli");
    if (acpi_ready) {
        uacpi_status st = uacpi_reboot();
        acpi_udelay(100000);
        printk("[ACPI] reset register: %s\n", uacpi_status_to_string(st));
    }

    /* 1. 0xCF9 reset-control register (QEMU/modern chipsets). */
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x0E);
    acpi_udelay(10000);

    /* 2. Pulse the 8042 keyboard-controller CPU reset line. */
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 0x02)) break;   /* wait input buffer empty */
    outb(0x64, 0xFE);
    acpi_udelay(10000);

    /* 3. Triple fault — install an empty IDT so the very next interrupt
     *    cascades #GP → #DF → triple fault → CPU reset.  Foolproof on x86;
     *    QEMU with -no-reboot exits, otherwise the machine restarts. */
    static const struct { uint16_t limit; uint32_t base; }
        __attribute__((packed)) null_idtr = { 0, 0 };
    __asm__ volatile("lidt %0" :: "m"(null_idtr) : "memory");
    __asm__ volatile("int $0x03");
    for (;;) __asm__ volatile("hlt");
}
