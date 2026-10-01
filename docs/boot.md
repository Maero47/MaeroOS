# Boot paths: Multiboot 1, Multiboot 2, BIOS and UEFI

MaeroOS stays a 32-bit i686 kernel, and it boots on UEFI-only PCs without
going 64-bit: [Limine](https://github.com/limine-bootloader/limine) loads
Multiboot 2 kernels from legacy BIOS and from UEFI firmware. That includes
x86_64 UEFI, where Limine's 64-bit `BOOTX64.EFI` drops to 32-bit protected
mode before it jumps to the kernel.

| Image / command | Loader | Protocol | Firmware | Framebuffer |
|---|---|---|---|---|
| `make run` (`-kernel`) | QEMU | Multiboot 1 | SeaBIOS | none (VGA text) |
| `make iso` (`maeros.iso`) | GRUB | Multiboot 1 | BIOS | VBE (`gfxpayload`) |
| `make limine-iso` (`maeros-limine.iso`) | Limine 11.4.1 | Multiboot 2 | BIOS, x86_64 UEFI, IA32 UEFI | VBE or UEFI GOP |

`kernel.elf` carries both headers (`arch/i686/boot/boot.asm`). The Multiboot 2
header has address, entry, optional 32-bpp framebuffer, and module alignment
tags. `tools/limine.conf` asks for 1920x1080x32. If the firmware has no such
mode, Limine picks another one.

## How the kernel takes the boot info

* `_start` accepts either magic in EAX. For Multiboot 2, it copies the info
  block, at most 32 KiB, into `mb2_info_copy` in `.bss` while paging is still
  off. Under UEFI the loader may place the block anywhere below 4 GiB, which
  includes addresses past the 12 MiB that the boot page tables map.
* `boot_info_init()` (`kernel/boot_info.c`) gives the kernel the Multiboot 1
  `multiboot_info_t` view that `pmm_init`, `framebuffer_init` and the initrd
  code already read. For Multiboot 2 it rebuilds that view from the tags:
  memory map (tag 6, falling back to the EFI memory map, tag 17), modules
  (tag 3), command line, loader name, basic meminfo and framebuffer (tag 8,
  with the RGB field layout). `mem_upper` comes from the highest available
  byte of the map. The basic meminfo tag ends at the first firmware hole,
  which under OVMF is far below the end of RAM.
* The boot log has one summary line, for example:
  `[BOOT] multiboot2: loader "Limine 11.4.1" (UEFI), 20 mmap entries, 1 module(s), fb yes, ACPI RSDP v2`,
  followed by the usual `[FB] 1920x1080@32 ... phys=0x80000000` line.

## API for other subsystems (`include/kernel/boot_info.h`)

```c
const void *boot_info_rsdp(uint32_t *len); /* ACPI RSDP copy, or NULL      */
int         boot_info_is_efi(void);        /* booted from UEFI firmware    */
int         boot_info_protocol(void);      /* 1 or 2                       */
const char *boot_info_cmdline(void);       /* "" if none                   */
const char *boot_info_loader_name(void);   /* "" if none                   */
```

`boot_info_rsdp()` returns a kernel-virtual pointer to a **copy** of the RSDP
structure from Multiboot 2 tag 15 (ACPI 2.0+, 36 bytes, XSDT valid) or tag 14
(ACPI 1.0, 20 bytes). Multiboot 2 passes the bytes, not the firmware address.
The checksums are verified, and the copy is valid for the life of the kernel.
Read `RsdtAddress` / `XsdtAddress` from it and map the tables as usual.
It returns NULL on every Multiboot 1 boot. An ACPI parser should then scan
the EBDA and 0xE0000-0xFFFFF for `"RSD PTR "`. That scan is valid only when
`boot_info_is_efi()` is 0, because UEFI machines need not have those legacy
areas. `drivers/acpi.c` (`uacpi_kernel_get_rsdp`) does exactly this: the
loader's copy first (its physical address is the direct map's, since it lives
in kernel .bss), the scan only on a BIOS boot.

## Things UEFI firmware leaves differently

* **8259 masks.** SeaBIOS leaves IRQ0 (PIT) and the IRQ2 cascade unmasked, but
  OVMF masks every line except the keyboard. `pic_remap()` keeps the
  firmware's masks, so `pit_init()` now unmasks IRQ0 itself, and
  `pic_unmask()` of a slave line also unmasks the cascade. Without this fix,
  the scheduler tick never fired under UEFI and anything that sleeps or burns
  CPU stalled. The visible symptom was a login that hung after the password.
* No VGA text mode. The GOP framebuffer is the only display, and VGA text
  writes go nowhere.
* No MP table or BIOS-area RSDP. Use `boot_info_rsdp()`.
* **Memory from 8 MiB up is reserved on OVMF x64** (its MEMFD area: SEC page
  tables, lock box, work area). Limine needs `[1 MiB, bss_end_addr)` free, and
  `bss_end_addr` is `_kernel_phys_end + 64 KiB`, so the image must end below
  that. Otherwise Limine stops with "multiboot2: Could not find viable load
  address for executable". `linker.ld` asserts the limit. Big tables belong on
  the heap; the frame refcounts moved there for this reason.

## Testing: `make smoke-uefi` (part of `make check`)

`tools/smoke_uefi.py` boots `maeros-limine.iso` with a copy of `disk.img` three
times: SeaBIOS (`qemu-system-i386`), OVMF x64 (`qemu-system-x86_64`, the
common real-PC case) and OVMF IA32 (`qemu-system-i386`). For each boot it
checks the Multiboot 2 summary line (Limine, the expected firmware, a module,
a framebuffer and an RSDP), logs the framebuffer resolution, logs in on the
serial getty, waits for the desktop on that framebuffer and takes a QMP
screendump of it. It then requires `[ACPI] ready` with the RSDP taken from the
loader and runs `poweroff`, which must end QEMU through ACPI S5. Artifacts go to `build/smoke-uefi/<name>/`.

If an OVMF build is missing, the test reports SKIP for that configuration
(set `SMOKE_UEFI_REQUIRE=1` to make it fail instead). `tools/setup-linux.sh`
installs both builds: x64 from apt `ovmf` (with `--no-sudo` it is unpacked
into `~/opt/hostpkgs`), and IA32 from Debian's pinned `ovmf-ia32` .deb.

The UEFI boots run under **TCG** by default (`SMOKE_UEFI_ACCEL=kvm` to
override). Under KVM, OVMF's CD driver takes one port exit per 16-bit word, and
Limine spends 3-5 minutes loading the 90 MB initrd. Under TCG it takes about
8 seconds. The kernel runs normally under both.

Manual runs:

```sh
make limine-iso
qemu-system-i386 -cdrom maeros-limine.iso -m 512M -vga std -serial stdio     # BIOS
cp ~/opt/hostpkgs/usr/share/OVMF/OVMF_VARS_4M.fd build/vars.fd
qemu-system-x86_64 -m 512M -vga std -serial stdio -cdrom maeros-limine.iso \
  -drive if=pflash,format=raw,readonly=on,file=$HOME/opt/hostpkgs/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=build/vars.fd                             # x86_64 UEFI
```

On real hardware, write the ISO to a USB stick (`dd`). It is a hybrid image
with an MBR, a BIOS boot sector and an EFI system partition. Secure Boot must
be off, because Limine is not signed.

## Limine binaries

`tools/fetch-limine.sh` downloads the `v11.4.1-binary` tag tarball, verifies
its pinned sha256 and builds the `limine` host tool (`bios-install`). The
files land in `third_party/limine/bin/`, which is gitignored.
`third_party/limine/LICENSE` (BSD-2-Clause) is kept in the tree, and it is also
copied into the image as `/boot/limine/LICENSE`.

References: Multiboot 2 specification
(https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html), Limine
`CONFIG.md` and `USAGE.md` for the pinned release.
