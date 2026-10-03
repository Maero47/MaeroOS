#include <kernel/types.h>
#include <kernel/config.h>
#include <kernel/multiboot.h>
#include <kernel/boot_info.h>
#include "../drivers/serial.h"
#include "../drivers/rtc.h"
#include "../drivers/ac97.h"
#include "../drivers/hda.h"
#include "../arch/i686/cpu/fpu.h"
#include "../drivers/vga.h"
#include "../kernel/printk.h"
#include "../arch/i686/cpu/gdt.h"
#include "../arch/i686/cpu/tss.h"
#include "../arch/i686/cpu/idt.h"
#include "../arch/i686/cpu/pic.h"
#include "../arch/i686/cpu/tsc.h"
#include "../arch/i686/cpu/apic.h"
#include "../arch/i686/cpu/smp.h"
#include "../arch/i686/cpu/percpu.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../arch/i686/mm/paging.h"
#include "../mm/heap.h"
#include "../mm/kstack.h"
#include "../arch/i686/cpu/pit.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "../fs/vfs.h"
#include "../fs/initrd.h"
#include "../fs/ext2.h"
#include "../fs/tmpfs.h"
#include "../fs/devfs.h"
#include "../fs/procfs.h"
#include "../fs/mount.h"
#include "../drivers/blkpart.h"
#include "../drivers/ata.h"
#include "../drivers/ahci.h"
#include "../drivers/nvme.h"
#include "../drivers/blkdev.h"
#include "../drivers/pci.h"
#include "../drivers/acpi.h"
#include "../drivers/rtl8139.h"
#include "../drivers/e1000.h"
#include "../drivers/r8169.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/usb/xhci.h"
#include "../kernel/random.h"
#include "../net/net.h"
#include "../net/lwip_glue.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../drivers/alsa.h"
#include <stdint.h>

void stack_chk_seed(void);   /* kernel/stack_chk.c */

#if KSTACK_TEST == 4
/* `make KSTACK_TEST=4`: the BSP overflows the stack its scheduler loop runs
 * on, which must hit the guard below it (see bsp_scheduler_entry). */
static volatile uint32_t bsp_test_depth;

static uint32_t __attribute__((noinline)) bsp_test_recurse(uint32_t n) {
    volatile uint8_t frame[256];
    frame[0] = (uint8_t)n;
    bsp_test_depth = n;
    if (n == 0xFFFFFFFFU) return 0;     /* never: the guard stops us first */
    return bsp_test_recurse(n + 1) + frame[0];
}
#endif

/* The BSP's scheduler loop, entered on a guarded stack by kernel_main. */
static void __attribute__((noreturn)) bsp_scheduler_entry(void) {
#if KSTACK_TEST == 4
    uint32_t esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    printk("[KSTACK-TEST] mode 4: BSP scheduler stack, esp=0x%08x\n",
           (unsigned)esp);
    bsp_test_recurse(0);
    printk("[KSTACK-TEST] FAILED: overflow went unnoticed (depth %u)\n",
           (unsigned)bsp_test_depth);
    for (;;) __asm__ volatile("hlt");
#endif
    scheduler_start();
    for (;;) __asm__ volatile("hlt");   /* unreachable */
}

/* root=/dev/<name> or root=PARTUUID=<gpt guid> on the command line picks the
 * partition ext2 mounts at /disk (maeros-install writes the latter).  Without
 * root= the boot disk's whole device is /disk, as before.  A root= that names
 * nothing leaves /disk unmounted: mounting some other disk in its place could
 * hand an installed system the wrong filesystem. */
static void mount_disk_root(void) {
    char want[64];
    const char *cl = boot_info_cmdline();
    const char *arg = NULL;
    for (const char *p = cl; *p; p++)
        if ((p == cl || p[-1] == ' ') && strncmp(p, "root=", 5) == 0) { arg = p + 5; break; }
    blkpart_t *bp = NULL;
    char devpath[24];
    uint32_t start = 0;
    if (arg) {
        uint32_t n = 0;
        while (arg[n] && arg[n] != ' ' && n < sizeof(want) - 1) { want[n] = arg[n]; n++; }
        want[n] = '\0';
        if (strncmp(want, "PARTUUID=", 9) == 0)
            bp = blkpart_find_partuuid(want + 9);
        else if (strncmp(want, "/dev/", 5) == 0)
            bp = blkpart_find(want + 5);
        if (!bp) {
            printk("[BOOT] root=%s: no such device; /disk is not mounted\n", want);
            return;
        }
        snprintf(devpath, sizeof(devpath), "/dev/%s", bp->name);
        printk("[BOOT] root=%s is %s (start %u, %u sectors)\n", want, devpath,
               (unsigned)bp->start, (unsigned)bp->nsect);
        blk_set_boot(bp->dev);
        start = bp->start;
    } else {
        if (!blk_present()) return;
        snprintf(devpath, sizeof(devpath), "%s", blk_boot_devpath());
        bp = blkpart_find(devpath + 5);
    }
    /* ext2 checks the superblock's size against the device's. */
    vfs_node_t *disk_root = ext2_mount(start, bp ? bp->nsect : 0);
    if (!disk_root) {
        if (arg) printk("[BOOT] root=%s: no ext2/ext4 filesystem on %s\n", want, devpath);
        return;
    }
    if (bp) blk_set_busy(bp->dev);
    vfs_mount("/disk", disk_root);
    vfs_set_root_overlay(disk_root);
    vfs_mount_note(devpath, "/disk", ext2_boot_fstype(), 0);
}

void kernel_main(u32 mb_magic, u32 mb_phys) {
    /* First, before any protected frame is live that will return: see
     * stack_chk_seed().  kernel_main itself never returns. */
    stack_chk_seed();

    /* ── M2: Serial first ─────────────────────────────────────────────────── */
    serial_init();
    serial_puts("[BOOT] Serial up.\r\n");
    rtc_init();

    if (mb_magic != MULTIBOOT1_BOOTLOADER_MAGIC &&
        mb_magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        serial_puts("[BOOT] ERROR: bad multiboot magic!\r\n");
        for (;;) __asm__ volatile("hlt");
    }

    /* ── M3: GDT / IDT / PIC ─────────────────────────────────────────────── */
    gdt_init();
    tss_init();
    idt_init();
    fpu_init();
    pic_remap();

    /* ── M4: VGA + printk ────────────────────────────────────────────────── */
    vga_init();
    printk("=== MaeroOS Kernel (M15: ATA + ext2 + syscalls) ===\n");
    printk("[BOOT] Magic: 0x%08x   MB_info_phys: 0x%08x\n", mb_magic, mb_phys);

    /* ── M5: PMM ─────────────────────────────────────────────────────────── */
    /* Multiboot 2 (Limine, BIOS or UEFI) is translated into the Multiboot 1
     * view everything below reads; see include/kernel/boot_info.h. */
    multiboot_info_t *mbi = boot_info_init(mb_magic, mb_phys);

    /*
     * Save module addresses before pmm_init writes the bitmap over them.
     * The bitmap is placed at _kernel_phys_end rounded up to 4 KiB, which
     * can collide with QEMU's multiboot modules table.
     */
    uint32_t initrd_phys_start = 0, initrd_phys_end = 0;
    if ((mbi->flags & MULTIBOOT_FLAG_MODS) && mbi->mods_count > 0) {
        multiboot_module_t *mods =
            (multiboot_module_t *)(mbi->mods_addr + KERNEL_VMA);
        initrd_phys_start = mods[0].mod_start;
        initrd_phys_end   = mods[0].mod_end;
    }

    pmm_init(mbi);

    if (initrd_phys_start) {
        pmm_reserve_region(initrd_phys_start,
                           initrd_phys_end - initrd_phys_start);
        printk("[BOOT] Initrd at phys 0x%08x-0x%08x (%u bytes)\n",
               (unsigned)initrd_phys_start, (unsigned)initrd_phys_end,
               (unsigned)(initrd_phys_end - initrd_phys_start));
    }
    random_init(mb_phys, initrd_phys_start ^ initrd_phys_end);

    /* ── M6: VMM ─────────────────────────────────────────────────────────── */
    /* "nonx" on the command line: PAE paging without EFER.NXE, for telling
     * an NX fault from anything else (tbl_set then never writes bit 63). */
    {
        const char *c = boot_info_cmdline();
        for (; c && *c; c++)
            if ((c == boot_info_cmdline() || c[-1] == ' ') &&
                c[0] == 'n' && c[1] == 'o' && c[2] == 'n' && c[3] == 'x' &&
                (c[4] == '\0' || c[4] == ' '))
                paging_nx_disable();
    }
    paging_init();
    paging_set_kernel_permissions();

    /* ── SMP S1: enable the BSP's Local APIC (needs paging for the MMIO map) ── */
    apic_init();
    if (apic_available())
        printk("[APIC] BSP Local APIC enabled: id=%u, cpu-count hint=%u\n",
               (unsigned)apic_id(), (unsigned)apic_cpu_count_hint());

    framebuffer_init(mbi);

    /* ── M7: Heap ────────────────────────────────────────────────────────── */
    heap_init();
    pmm_refcount_init();
#if KHEAP_TEST
    heap_selftest();
#endif

    /* ── M11: VFS + initrd ───────────────────────────────────────────────── */
    vfs_init();
    if (initrd_phys_start) {
        initrd_init(initrd_phys_start, initrd_phys_end);
    } else {
        printk("[BOOT] No initrd — filesystem unavailable.\n");
    }

    /* ── ATA + ext2 disk ────────────────────────────────────────────────── */
    net_init();
    pci_init();
    rtl8139_init();
    e1000_init();
    r8169_init();
    ac97_init();
    xhci_init();
    hda_init();
    alsa_init();          /* /dev/snd on whichever of the two is up */
    net_lwip_init();
    ata_init();
    ahci_init();
    nvme_init();
    blk_init();
    blkpart_init();       /* /dev/hda, /dev/sda1, ...: partitions for mount(2) */
    vfs_mount_note("rootfs", "/", "rootfs", 0);
    mount_disk_root();

    /* ── tmpfs at /tmp ───────────────────────────────────────────────────── */
    {
        vfs_node_t *tmp_root = tmpfs_mount();
        if (tmp_root) vfs_mount("/tmp", tmp_root);
        if (tmp_root) vfs_mount_note("tmpfs", "/tmp", "tmpfs", 0);
    }

    /* ── devfs at /dev ───────────────────────────────────────────────────── */
    vfs_node_t *dev_root = devfs_mount();
    if (dev_root) {
        vfs_mount("/dev", dev_root);
        vfs_mount_note("devtmpfs", "/dev", "devtmpfs", 0);
    }

    /* ── procfs at /proc ─────────────────────────────────────────────────── */
    vfs_node_t *proc_root = procfs_mount();
    if (proc_root) {
        vfs_mount("/proc", proc_root);
        vfs_mount_note("proc", "/proc", "proc", 0);
    }
    mount_set_boot_roots(proc_root, dev_root);

    /* ── M8/M9/M10: Scheduler + user mode ───────────────────────────────── */
    proc_init();
    scheduler_init();
    keyboard_init();
    mouse_init();
    pit_init(100);
    /* Measure the TSC against PIT channel 2 while interrupts are still off:
     * the tick interrupt can be coalesced, so counting ticks does not measure
     * wall time (arch/i686/cpu/tsc.c). */
    tsc_init();

    /* ACPI tables + AML namespace (uACPI).  Needs the heap, PCI and a clock;
     * must precede init so init's page directory inherits the map window. */
    acpi_init();

    __asm__ volatile("sti");

    /* ── SMP S2: bring up application processors (needs heap, LAPIC, PIT for
     * timed delays + interrupts on).  APs park (hlt) until the per-CPU scheduler
     * (S4) puts them to work. */
    smp_boot_aps();

    /* ── M12: Launch init. Prefer persistent disk userland when available. */
    {
        const char *init_paths[] = { "/disk/init", "/init" };
        int launched = 0;
        for (uint32_t i = 0; i < 2; i++) {
            vfs_node_t *init_node = vfs_open(init_paths[i]);
            if (!init_node) continue;
            printk("[BOOT] Launching %s\n", init_paths[i]);
            if (proc_create_from_elf(init_node, "init")) {
                launched = 1;
                break;
            }
            printk("[BOOT] failed to launch %s\n", init_paths[i]);
        }
        if (!launched) {
            printk("[BOOT] no working init found at /disk/init or /init!\n");
            for (;;) __asm__ volatile("hlt");
        }
    }

    /* Kernel threads come after init, so init is pid 1 (ps -p 1, kill -1
     * semantics).  QEMU's default NIC is an e1000, so most boots have one. */
    acpi_start_thread();
    /* Network bottom-half: keeps DHCP/TCP alive without userspace polling.
     * Always: lo needs lwIP's timers (retransmits, delayed ACKs, TIME_WAIT)
     * even on a machine with no NIC. */
    proc_create_kthread(knetd, "knetd");
    ac97_start_thread();
    hda_start_thread();
    xhci_start_thread();

    printk("[BOOT] Jumping to scheduler.\n");
    /* The process table is now fully built — release the APs so they can scan
     * it and run threads concurrently (no effect when uniprocessor). */
    g_smp_go = 1;
    /* Enter the scheduler holding the Big Kernel Lock (depth 1).  The first
     * dispatched user process releases it in forkret on its way to user mode;
     * thereafter the BKL is held only while a CPU executes kernel code. */
    bkl_acquire();

    /* Boot ran on the 16 KiB .bss stack from boot.asm, which has nothing
     * mapped-out below it: an overflow there would silently overwrite .bss.
     * The scheduler loop (and every interrupt taken while it idles) runs on
     * this CPU for good, so move it to a guarded stack from the kstack window,
     * as the APs already do (smp.c).  Nothing on the old stack is used again. */
    void *stack = kstack_alloc();
    if (!stack) {
        printk("[BOOT] no guarded stack for the BSP; staying on the boot stack\n");
        bsp_scheduler_entry();
    }
    printk("[BOOT] BSP scheduler stack [0x%08x,0x%08x)\n",
           (unsigned)(uintptr_t)stack, (unsigned)(uintptr_t)stack + KSTACKSIZE);
    __asm__ volatile("mov %0, %%esp\n\t"
                     "xor %%ebp, %%ebp\n\t"
                     "call *%1"
                     :: "r"((uint32_t)(uintptr_t)stack + KSTACKSIZE),
                        "r"(bsp_scheduler_entry)
                     : "memory");
    __builtin_unreachable();
}
