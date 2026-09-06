#include <kernel/types.h>
#include <kernel/config.h>
#include <kernel/multiboot.h>
#include "../drivers/serial.h"
#include "../drivers/rtc.h"
#include "../drivers/ac97.h"
#include "../arch/i686/cpu/fpu.h"
#include "../drivers/vga.h"
#include "../kernel/printk.h"
#include "../arch/i686/cpu/gdt.h"
#include "../arch/i686/cpu/tss.h"
#include "../arch/i686/cpu/idt.h"
#include "../arch/i686/cpu/pic.h"
#include "../arch/i686/cpu/apic.h"
#include "../arch/i686/cpu/smp.h"
#include "../arch/i686/cpu/percpu.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../arch/i686/mm/paging.h"
#include "../mm/heap.h"
#include "../arch/i686/cpu/pit.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "../fs/vfs.h"
#include "../fs/initrd.h"
#include "../fs/ext2.h"
#include "../fs/tmpfs.h"
#include "../fs/devfs.h"
#include "../fs/procfs.h"
#include "../drivers/ata.h"
#include "../drivers/pci.h"
#include "../drivers/rtl8139.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../kernel/random.h"
#include "../net/net.h"
#include "../net/lwip_glue.h"
#include <stdint.h>

void kernel_main(u32 mb_magic, u32 mb_phys) {
    /* ── M2: Serial first ─────────────────────────────────────────────────── */
    serial_init();
    serial_puts("[BOOT] Serial up.\r\n");
    rtc_init();

    if (mb_magic != 0x2BADB002) {
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
    multiboot_info_t *mbi = (multiboot_info_t *)(mb_phys + KERNEL_VMA);

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
    ac97_init();
    net_lwip_init();
    ata_init();
    if (ata_present()) {
        vfs_node_t *disk_root = ext2_mount(0);
        if (disk_root) {
            vfs_mount("/disk", disk_root);
            vfs_set_root_overlay(disk_root);
        }
    }

    /* ── tmpfs at /tmp ───────────────────────────────────────────────────── */
    {
        vfs_node_t *tmp_root = tmpfs_mount();
        if (tmp_root) vfs_mount("/tmp", tmp_root);
    }

    /* ── devfs at /dev ───────────────────────────────────────────────────── */
    {
        vfs_node_t *dev_root = devfs_mount();
        if (dev_root) vfs_mount("/dev", dev_root);
    }

    /* ── procfs at /proc ─────────────────────────────────────────────────── */
    {
        vfs_node_t *proc_root = procfs_mount();
        if (proc_root) vfs_mount("/proc", proc_root);
    }

    /* ── M8/M9/M10: Scheduler + user mode ───────────────────────────────── */
    proc_init();
    scheduler_init();
    keyboard_init();
    mouse_init();
    pit_init(100);

    /* Network bottom-half: keeps DHCP/TCP alive without userspace polling */
    if (net_find_interface("eth0"))
        proc_create_kthread(knetd, "knetd");
    ac97_start_thread();

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

    printk("[BOOT] Jumping to scheduler.\n");
    /* The process table is now fully built — release the APs so they can scan
     * it and run threads concurrently (no effect when uniprocessor). */
    g_smp_go = 1;
    /* Enter the scheduler holding the Big Kernel Lock (depth 1).  The first
     * dispatched user process releases it in forkret on its way to user mode;
     * thereafter the BKL is held only while a CPU executes kernel code. */
    bkl_acquire();
    scheduler_start(); /* never returns */
}
