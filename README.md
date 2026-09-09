# MaeroOS

A 32-bit operating system written from scratch for i686: kernel, C library, userland,
window system and desktop. It boots under `qemu-system-i386` from a Multiboot kernel
image or a GRUB ISO, speaks the Linux i386 system-call ABI over `int 0x80`, and runs
real Linux i386 ELF binaries (static and dynamically linked) alongside its own programs.

![The MaeroOS desktop](docs/screenshots/desktop.png)

The desktop at 1280x800. `Console` and `System` are the desktop's own built-in windows,
a fallback shell and an event log; `maeroX :0` is the X11 server running as an ordinary
desktop application and reporting `0 clients, DISPLAY=:0`.

## Why it is unusual

- The kernel implements the **Linux i386 syscall table**, not a bespoke one.
  `proc/syscall.c` dispatches 176 Linux syscall numbers, so software built with an
  ordinary `i686-linux-musl` or `i686-linux-gnu` toolchain runs without patching.
- **Dynamic linking works.** The real musl `ld-musl-i386.so.1` loads PIEs and external
  shared objects, which is what makes prebuilt Linux packages usable at all.
- **SMP.** Application processors are booted through a real-mode trampoline and run
  threads under a recursive Big Kernel Lock with IPI-based TLB shootdown.
- **The X11 wire protocol.** `maeroX` is a native MaeroOS GUI application that acts as
  an X server on `/tmp/.X11-unix/X0`, including enough of the RENDER extension for Cairo
  to composite and for text to arrive as glyphs. The genuine libX11, libxcb, Cairo, Pango
  and GTK3 stack has been cross-built and run against it.

## Project status

What is proven by the automated QEMU tests in `tools/`:

| Area | Proof | Target |
|---|---|---|
| Boot, shell, procfs, PTYs, threads, shm, RNG | `ls`, `cat`, `sysprobe ok`, `shmprobe ok`, `threadprobe ok`, `ptytest ok`, `cttytest ok`, `/proc/self/status` | `make smoke` |
| toybox 0.8.13 as a static guest binary | `TOYBOX_OK` plus 17 applet checks | `make smoke-toybox` |
| Native coreutils-style commands | `uname`, `whoami`, `hostname`, `free`, `df`, `uptime`, `which` | `make smoke-cmds` |
| ext2 disk, login/passwd, init services, sessions | 108 assertions including `passwd: password updated for root` and `svc` state transitions | `make smoke-disk` |
| TCP/IP over lwIP and RTL8139 | `MAEROS_HTTP_OK` fetched from a host HTTP server | `make smoke-net` |
| Kernel firewall | after `fwctl enable` plus a drop rule the same fetch fails, `fwctl list` reports the firewall `enabled` with the `drop out tcp` rule, and `fwctl flush` restores the fetch | `make smoke-fw` |
| Dynamic linker | `DYNPROBE_OK` from a PIE loaded through musl `ld.so` | `make smoke-dyn` |
| External shared libraries and pthreads | `GREET_OK sum=42`, `ZLIB_OK ver=1.3`, `THREADS_OK count=200000`, `UNIX_SOCK_OK` | `make smoke-dynlib` |
| X11 server | `XHANDSHAKE_OK`, `XDRAW_OK` (`w=320 h=200` from `GetGeometry`), `XEVENT_OK`, and `XREAL_PAINTED` from a client linked against the cross-built libX11 | `make smoke-x` |
| GLib, Cairo, Pango, GTK3 | `GLIB_OK` (v2.78), `CAIRO_OK rect_px=0xe69919`, `PANGO_OK`, `GTK_OK init`, `GTK_WINDOW_SHOWN`, `GTK_DRAWN` | `make smoke-gtk` |

### What does not work

- **Firefox 115.15.0esr does not reach first paint.** The prebuilt i686 ESR build loads
  and `/disk/firefox/firefox-bin --version` prints `Mozilla Firefox 115.15.0esr` (the
  `run-firefox` comment in the Makefile records this), but the browser stalls during
  startup before rendering a page. Much of the Firefox-specific tracing left in
  `proc/syscall.c`, `proc/scheduler.c` and `proc/usocket.c` exists to chase that stall.
  This is the current frontier of the project, not a finished feature.
- **More than 512 MiB of RAM hangs at boot.** The physical and virtual memory managers
  need work before the 1 to 2 GiB that Gecko wants is usable. See `README-BROWSER.md`.
- **No W^X.** ELF segments are mapped writable; `proc/elf.c` does not yet enforce
  per-segment protection.
- **inotify is deliberately absent.** Numbers 291, 292, 293 and 332 return `-ENOSYS`
  on purpose so GLib falls back to its polling backend (see the comment on those
  cases in `proc/syscall.c`).
- **No SMP scaling.** One Big Kernel Lock serialises all kernel execution
  (`arch/i686/cpu/bkl.c`).
- **Only Linux and macOS hosts are covered.** The Makefile looks up `mke2fs`, `debugfs`
  and GNU `sed` on `PATH` (with the Homebrew locations as a fallback) and
  `tools/run-maeros.sh` picks the QEMU display, audio and screen-size probes per host,
  but nothing has been tried on Windows or the BSDs.

## What is in the box

### Kernel

- Multiboot 1 higher-half kernel: `arch/i686/boot/boot.asm` sets up paging before jumping
  to `0xC0000000`, `linker.ld` links the image at `KERNEL_VMA + 0x00100000`.
- Boot order lives in one readable function, `kernel_main` in `kernel/main.c`: serial,
  RTC, GDT/TSS/IDT/FPU, PIC, VGA, physical memory, paging, LAPIC, framebuffer, heap,
  VFS, initrd, PCI, network, ATA/ext2, tmpfs, devfs, procfs, scheduler, input, PIT,
  application processors, then `/disk/init` or `/init`.
- 21,612 lines of C, headers and assembly across the kernel directories, of which
  `proc/syscall.c` is 6,653.
- Limits in `include/kernel/config.h`: `MAX_PROCS` 128, `MAX_FD` 128, 32 KiB kernel
  stacks, a 256 MiB kernel heap window at `0xD0000000`, 64-page user stacks below
  `0xC0000000`.

### Memory

- `mm/pmm.c`: bitmap frame allocator with a per-frame `uint16_t` refcount array and a
  use-after-free detector that reports frames handed out with a stale refcount.
- `arch/i686/mm/paging.c`: recursive page-directory self-map at PDE 1023, copy-on-write
  fork, demand-paged anonymous VMAs, and an exception table (`__start___ex_table`) so a
  faulting `copy_from_user` returns `-EFAULT` instead of panicking.
- `mm/heap.c`: free-list allocator whose block headers carry a `0xDEADBEEF` magic that is
  checked on every access.

### Processes, threads and IPC

- `sys_clone` handles `CLONE_VM`, `CLONE_SETTLS` (per-thread TLS through GDT entry 6),
  `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID` and `CLONE_CHILD_CLEARTID`, which is what
  makes `pthread_join` return.
- `futex` WAIT/WAKE backs musl mutexes and condition variables.
- `proc/usocket.c`: AF_UNIX stream sockets with `socketpair`, named bind/listen/connect,
  `sendmsg`/`recvmsg`, and `SCM_RIGHTS` file-descriptor passing integrated into `poll`
  and `select`. This is the transport X11 clients connect over.
- `proc/pipe.c` pipes and FIFOs, `proc/shm.c` shared memory, `proc/signal.c` signal
  delivery with a `sigcontext`/`ucontext` frame laid out exactly as glibc expects, so a
  handler can read `uc_mcontext`.
- `proc/elf.c` loads `ET_EXEC` and `ET_DYN` (the latter at a load bias) and records
  `PT_INTERP` rather than rejecting it. `sys_exec` in `proc/syscall.c` is what acts on it:
  it maps the interpreter at `0x40000000`, then enters it with a full aux vector
  (`AT_PHDR`, `AT_BASE`, `AT_ENTRY`, `AT_PAGESZ`, `AT_RANDOM`).

### SMP

`arch/i686/cpu/` holds the LAPIC driver (`apic.c`), AP startup through
`arch/i686/boot/ap_trampoline.asm` (`smp.c`), per-CPU scheduler state (`percpu.h`) and
the recursive Big Kernel Lock (`bkl.c`). A CPU spinning for the lock keeps servicing TLB
shootdown requests while it waits, because it holds interrupts off and would otherwise
deadlock the sender.

### Filesystems

`fs/vfs.c` mount table with a root overlay, `fs/initrd.c` ustar archive read from the
Multiboot module, `fs/ext2.c` (1,522 lines, read and write, mounted at `/disk` and
overlaid on `/`), `fs/tmpfs.c` at `/tmp`, `fs/devfs.c` at `/dev` (`null`, `zero`, `tty`,
`ptmx`, `pts/`, `random`, `urandom`, `fb0`, `dsp`, `shm`, `input/event0`, `input/event1`,
standard fd aliases), and `fs/procfs.c` at `/proc` (`self`, per-pid `status`, `stat`,
`statm`, `maps`, `fd`, `cmdline`, `environ`, `auxv`, `exe`, plus `meminfo`, `version`,
`uptime`, `cpuinfo`, `kmsg`, `processes`, `pci`, `netif`, `firewall`, `sys/vm/`).

### Networking

lwIP 2.2.1 is vendored at `third_party/lwip` and driven by `net/lwip_glue.c` over the
RTL8139 driver in `drivers/rtl8139.c`. DHCP runs at boot; `net/socket.c` implements the
BSD socket calls both through `socketcall` (102) and the direct i386 numbers 359 to 373;
`net/firewall.c` is a rule-based packet filter configured by `fwctl` and readable at
`/proc/firewall`; a kernel thread `knetd` (`net/net.c`) keeps timers and TCP alive
without userspace polling.

### Drivers

ATA PIO (`ata.c`), PCI enumeration (`pci.c`), RTL8139 (`rtl8139.c`), Intel 82801AA AC'97
audio (`ac97.c`), Multiboot VBE framebuffer (`framebuffer.c`), VGA text (`vga.c`), PS/2
keyboard and mouse (`keyboard.c`, `mouse.c`), CMOS RTC (`rtc.c`) and 16550 serial
(`serial.c`).

### Userland

`userspace/` is 26,190 lines across 192 files, built with the same `i686-elf-gcc` and
linked against its own freestanding libc (`userspace/libc/`: syscall stubs, stdio, stdlib,
string, dirent, termios, sockets, a DNS resolver, pthreads and a toybox compatibility
layer, entered from `crt0.asm`). `userspace/Makefile` produces 100 binaries into
`testfiles/`.

**init** (`userspace/init/init.c`) prefers a disk userland over the initrd, runs `/etc/rc`
through the shell, then reads two tables: `/etc/inittab` for console sessions to respawn
(up to 4) and `/etc/services` for `once` and `respawn` services (up to 8, each separately
enabled or disabled). It opens a control FIFO at `/tmp/initctl`, writes
`/tmp/services.status` and `/tmp/sessions.status` for `svc` and `session` to read back,
and appends to `/var/log/init.log` when the disk root is writable. Respawn has a backoff:
only an exit within 3 seconds counts as a rapid failure, each one adds 0.25 s of delay up
to 1 s, and after 8 in a row the session is parked instead of restarted. When a
framebuffer is present, init plays a startup chime through `wavplay` and runs the desktop
as the unprivileged user.

**The shell** (`userspace/shell/shell.c`, 1,528 lines) handles single and double quoting,
`;`, `&&`, `||` and `&`, pipelines of up to 16 commands, redirections (`<`, `>`, `>>`,
`2>`, `2>&1` and `<<` heredocs), `$VAR`, `$?`, `$$` and `$(...)` command substitution,
shell variables with `export` and `unset`, `if`/`elif`/`else`/`fi`, `while`/`do`/`done`,
`for x in ... do ... done`, and job control (`jobs`, `fg`, `bg`, `wait`, backed by
`setpgid` and `tcsetpgrp`). Other built-ins are `cd`, `pwd`, `echo`, `read`, `trap`,
`type`/`which`, `exit`, `true`, `false`, `clear` and `.`/`source`. It keeps arrow-key
history, and `-c` runs a single command string, which is how init and the launchers call
it.

**Commands.** Coreutils-style tools (`ls`, `cat`, `cp`, `rm`, `grep`, `sed`, `awk`,
`find`, `sort`, `diff`, `tar`, `xargs` and more), system tools (`ps`, `top`, `free`, `df`,
`du`, `uptime`, `dmesg`, `lspci`, `ifconfig`, `fwctl`, `svc`, `session`), account tools
(`login`, `passwd`, `doas`, `sudo`, `getty`, `id`, `whoami`) and the `*probe` binaries the
smoke tests assert on. Passwords are PBKDF2-HMAC-SHA256 at 100,000 iterations, implemented
in `userspace/auth/auth.c` and stored in `/etc/shadow`. Eighteen BusyBox applet names
(`vi`, `less`, `ping`, `mount`, `gzip` and others) are installed into `/bin` as copies of
a small `bbwrap` launcher.

### Desktop and window system

`desktop` (`userspace/desktop/desktop.c`, 4,889 lines) opens `/dev/fb0`,
`/dev/input/event0` and `/dev/input/event1`, and composites the whole screen into a back
buffer that it presents once per frame. It draws a PPM wallpaper (kept in a blurred copy
for the window backdrop), desktop icons that launch on double-click, and a taskbar with a
start menu that has a search filter and a right-click context menu. Windows have title
bars with minimize, maximize and close, plus drag, edge resize, half-screen snapping, a
show-desktop toggle, a minimize animation and a clock. The accent colour and wallpaper
path are read from `/etc/desktop.conf`. Two windows belong to the desktop itself: `Console`,
a fallback shell, and `System`, an event log. Themed icons come from `.mic` files generated
by `tools/mkicons.py`, and text is drawn with antialiased fonts generated by
`tools/mkfont.py`.

Applications reach the compositor through `libwm` (`userspace/libwm/wm.c`): a line
protocol written to the FIFO `/tmp/wmctl` (`app`, `title`, `geom`, `rect`, `text`, `icon`,
`focus`, `surface`, `commit`), with events read back from `/tmp/wmevents`. `wm_surface`
hands the desktop a shared-memory id, so a client renders into its own buffer and the
desktop composites it; that is what the kernel's shm syscalls are for. `libgui` builds
panels, labels, buttons, text inputs, checkboxes, scrollbars, list boxes and images on top
of `libdraw` (rectangles, rounded frames, alpha blending, antialiased text, `.mic`
images). `term`, `edit`, `calc`, `view`, `files`, `taskmgr`, `settings`, `store` and
`browse` are written this way; `term` runs a real shell on a PTY and several instances can
run side by side.

`maerox` (`userspace/maerox/maerox.c`, 1,334 lines) is the X11 server. It takes a desktop
slot like any other libgui application, listens on the AF_UNIX socket `/tmp/.X11-unix/X0`
for up to 8 clients, and answers the connection setup with one screen, two pixmap formats
and a single depth-24 TrueColor visual. The core request dispatch covers window and pixmap
lifecycle (CreateWindow, ChangeWindowAttributes, ConfigureWindow, MapWindow, UnmapWindow,
CreatePixmap, FreePixmap), drawing (CreateGC, ChangeGC, FreeGC, PolyFillRectangle,
PolyRectangle, PutImage, GetImage, CopyArea, ClearArea) and the queries a real Xlib client
blocks on (GetGeometry, GetWindowAttributes, QueryTree, InternAtom, GetAtomName,
GetProperty, ChangeProperty, GetInputFocus, GetSelectionOwner, QueryPointer,
GetKeyboardMapping, GetModifierMapping, QueryExtension). It sends `Expose`, `MapNotify`,
`ConfigureNotify`, `ButtonPress` and `ButtonRelease` back to clients. `QueryExtension`
reports RENDER as present, and the RENDER opcodes go far enough for Cairo and GTK to
paint: QueryPictFormats, CreatePicture, Composite, FillRectangles, CreateSolidFill,
CreateGlyphSet, AddGlyphs and CompositeGlyphs, so text arrives as real glyphs rather than
bitmaps.

`store` and `pkg` install packages from a repository built by `tools/mkrepo.py` out of
`ports/packages/*/pkg.conf`. `pkg update`, `list`, `install` and `remove` fetch
`index.txt` and per-package ustar archives over HTTP into `/disk/apps/<name>/`, defaulting
to the QEMU host at `10.0.2.2:8000` and overridable in `/disk/etc/pkg.conf`; `store` is
the libgui front end that drives the `pkg` binary. `make repo-serve` serves the repository
from the host.

`ff` (`userspace/ff/ff.c`) is the one-command Firefox launcher: it starts a windowed
maeroX in a desktop slot, creates a writable profile and home under `/tmp`, and execs
Firefox with the `LD_LIBRARY_PATH`, `DISPLAY`, fontconfig and GTK icon-theme environment
that build needs.

### Ports

Imported software is cross-built in a `i686-linux-musl` Docker container
(`ports/Dockerfile.cross`) and shipped on the ext2 disk: BusyBox 1.35.0, fbDOOM with the
shareware WAD, links 2.30 with framebuffer graphics, zlib, libpng, libjpeg, the
libX11/libxcb client stack (`ports/x11/build-x11.sh`), the GLib/Cairo/Pango/GTK3 stack
(`ports/gtk/`), and a prebuilt Firefox 115 ESR (`ports/firefox/`). The extracted trees and
the toolchain tarball are gitignored because they run to several gigabytes, so a fresh
clone will not contain them; the `ports/build-*.sh` scripts refetch and rebuild.

## Building and running

### Prerequisites

| Tool | Needed for |
|---|---|
| `i686-elf-gcc`, `i686-elf-ar`, `i686-elf-strip` | kernel and userland |
| `nasm` | assembly sources |
| `qemu-system-i386` | every `run*` and `smoke*` target |
| `grub-mkrescue` (or `i686-elf-grub-mkrescue`) and `xorriso` | `make iso`, and therefore the framebuffer desktop |
| `mke2fs` and `debugfs` from e2fsprogs | `make disk` |
| GNU `sed` (`gsed` on macOS) and a host `cc` | `make toybox` |
| `python3` | smoke tests and the asset and repository generators |
| `docker` | rebuilding anything under `ports/` |

The Makefile finds these on `PATH` (plus `/usr/sbin` and the Homebrew e2fsprogs
directory), so a missing tool fails when its target runs, not when Make parses the file.

#### Linux (Debian/Ubuntu)

`tools/setup-linux.sh` bootstraps a fresh Ubuntu box. It prints the apt package list,
builds an `i686-elf` binutils + GCC (C only, with libgcc) into `~/opt/cross`, downloads
the musl.cc `i686-linux-musl` toolchain into `ports/` and `~/opt`, and checks every
tool. The only step that needs root is the apt install, and even that is optional.

```sh
tools/setup-linux.sh --dry-run   # show what it would do (both plans)
tools/setup-linux.sh --sudo      # force the apt path: print the list, install nothing
tools/setup-linux.sh --apt       # apt install (sudo), build and fetch toolchains, verify
tools/setup-linux.sh --no-sudo   # no root at all, see below
tools/setup-linux.sh --check     # just report present/missing tools
export PATH="$HOME/opt/bin:$HOME/opt/cross/bin:$HOME/opt/i686-linux-musl-cross/bin:$PATH"
```

**Without root.** `--no-sudo` never calls `sudo`. It is also chosen automatically when
`make`, `nasm`, QEMU, the GRUB BIOS modules or a compiler are missing and `--apt` was not
given; the run says so in its header and names `--sudo`, which forces the apt path and
prints the package list without installing anything. `--dry-run` shows the apt list in
both modes, so the apt command is always obtainable.
It asks apt which of `make nasm bison flex m4 texinfo qemu-system-x86 qemu-system-gui
grub-pc-bin mtools xorriso e2fsprogs xz-utils zstd shellcheck` (plus dependencies) are
not installed, downloads exactly those with `apt-get download`, which checks each `.deb`
against the signed archive index, unpacks them with `dpkg-deb -x` into `~/opt/hostpkgs`,
and writes wrapper scripts into `~/opt/bin` that add what the relocated binaries need:
the library path, `-L` firmware directories and `QEMU_MODULE_DIR` for QEMU,
`-d ~/opt/hostpkgs/usr/lib/grub/i386-pc` for `grub-mkrescue`, `M4=` for bison and flex,
`PERL5LIB` for `makeinfo`. If the host has no C/C++ compiler, the self-contained musl.cc
`x86_64-linux-musl-native` toolchain is fetched into `~/opt` and exposed as `cc`/`c++`
wrappers that link statically (its dynamic loader is not installed on the host); the
`i686-elf` binutils and GCC are then built with it, with gmp/mpfr/mpc/isl in-tree via
gcc's `contrib/download_prerequisites` and without LTO/plugin support, since a static
`ld` cannot load plugins. A host that already has `gcc` and `g++` keeps them and the musl
toolchain is not fetched. Only `~/opt/bin` is added to `PATH`; nothing outside `~/opt`
and `ports/` is written. `make toybox` uses `cc` as `HOSTCC`. The run is idempotent
and resumable: unpacked packages, extracted tarballs and finished toolchains are skipped
on the next run.

Nothing in `~/opt/bin` permanently shadows a package installed later. No wrapper is
written for a tool the system already provides, every wrapper starts by handing off to a
system tool of the same name if one exists, a wrapper that a package supersedes is
rewritten as a plain hand-off on the next `--no-sudo` run, and `--sudo` or `--apt`
deletes those files outright. GRUB is the one exception: its wrappers keep precedence
until the system GRUB also has its BIOS modules in `/usr/lib/grub/i386-pc`, because
without them it cannot build the ISO. `--check` lists which tools come from wrappers and
which of those are already superseded.

`make initrd` also builds toybox from `third_party/toybox/.config.maeros`, the applet set
that compiles and links against the MaeroOS libc (about 100 applets; the rest need
headers or syscalls the libc does not provide yet).

`PREFIX=`, `OPT_DIR=`, `BIN_DIR=`, `HOSTPKGS_DIR=`, `MAEROS_SYS_PATH=`, `BINUTILS_VER=`
and `GCC_VER=` override the defaults; the script is idempotent and skips anything already built.
Downloads come from `ftp.gnu.org` and `musl.cc` over TLS and are checked against pinned
digests before anything is extracted; a different GCC or binutils version needs its
digest in `GCC_SHA256=` / `BINUTILS_SHA256=` (or `MAEROS_SKIP_HASH=1`). Running Docker (`docker.io`) and
`gcc-multilib` are optional and only matter for rebuilding `ports/`. `make start`
uses the `gtk` or `sdl` QEMU display and `pipewire`, `pa` or `alsa` audio, whichever
the installed QEMU supports, and sizes the guest to the primary monitor reported by
`xrandr` (or `xdpyinfo`, or 1280x800 without an X display).

#### macOS

Homebrew provides everything: `i686-elf-gcc`, `i686-elf-binutils`, `nasm`, `qemu`,
`i686-elf-grub`, `xorriso`, `e2fsprogs`, `gnu-sed` and `python3`. `make start` sizes
the guest from the Finder desktop bounds and uses the `cocoa` display with `coreaudio`.

### Targets

```sh
make                # build kernel.elf
make initrd         # build userspace + toybox, pack testfiles/ into initrd.tar
make run            # QEMU -kernel boot, serial on stdio, 512 MiB
make run-net        # -kernel boot with an RTL8139 on QEMU user networking
make run-disk       # -kernel boot with the ext2 disk.img attached
make iso            # GRUB ISO (maeros.iso)
make run-iso        # boot the ISO
make start          # build disk + ISO + package repo and open the graphical desktop
make disk           # build a 384 MiB ext2 disk from testfiles/
make disk-ff        # build a 1 GiB disk that also carries the Firefox tree
make run-firefox    # boot with disk-ff.img and 2 GiB of RAM
make debug          # QEMU with -d int,cpu_reset logging to /tmp/qemu.log
make gdb            # QEMU frozen, waiting for gdb on :1234
make clean
```

`make run` uses QEMU's built-in Multiboot loader, which does not supply VBE framebuffer
information, so that path is serial and VGA text only. The graphical desktop needs the
GRUB path: `make run-iso`, or `make start`, which also picks a guest resolution that fits
the host screen and serves the package repository on port 8000.

Everything goes to the serial console, so `make run` gives you the boot log and the
`MaeroOS$` prompt in your terminal. The boot log is also readable inside the guest with
`dmesg` and at `/proc/kmsg`.

## Testing

The ten smoke targets each boot QEMU, drive the guest shell over the serial console and
assert on the output. They are the project's regression suite; `tools/smoke.py` is the
baseline the others build on, so it is the one to read first.

```sh
make smoke          # boot, shell, procfs, PTYs, threads, shm, getrandom, ps
make smoke-cmds     # native uname/whoami/hostname/free/df/uptime/which
make smoke-toybox   # toybox as a static guest binary
make smoke-disk     # ext2, login/passwd, init sessions, service supervision, overlay
make smoke-net      # DHCP, TCP, HTTP GET from the host
make smoke-fw       # firewall rule blocks and unblocks that GET
make smoke-dyn      # PIE through the musl dynamic linker
make smoke-dynlib   # external .so files, zlib, pthreads, AF_UNIX
make smoke-x        # maeroX handshake, drawing and input events
make smoke-gtk      # GLib, Cairo, Pango and a real GTK3 window
```

Each script exits non-zero and prints the failing expectation, for example
`command 'threadprobe' did not produce 'threadprobe ok'`.

## Repository layout

| Path | Contents |
|---|---|
| `arch/i686/` | boot and AP trampoline assembly, GDT/IDT/TSS/PIC/PIT, LAPIC, SMP, BKL, paging |
| `drivers/` | ATA, PCI, RTL8139, AC'97, framebuffer, VGA, keyboard, mouse, RTC, serial |
| `fs/` | VFS, ustar initrd, ext2, tmpfs, devfs, procfs |
| `include/kernel/` | `config.h`, `types.h`, `multiboot.h`, `assert.h` |
| `kernel/` | `main.c`, `printk`, ring-buffer `klog`, `panic`, RNG, stack protector |
| `lib/` | freestanding `string` and `printf` |
| `mm/` | physical allocator, kernel heap, VMM helpers |
| `net/` | lwIP port and glue, sockets, firewall, `knetd` |
| `proc/` | scheduler, processes, ELF loader, syscalls, signals, pipes, AF_UNIX, shm |
| `userspace/` | libc, init, shell, libdraw/libwm/libgui, desktop, maeroX, commands |
| `ports/` | cross-build scripts, Dockerfiles and package recipes for imported software |
| `testfiles/` | the root filesystem staged into `initrd.tar` and `disk.img` |
| `third_party/` | vendored lwIP and toybox |
| `tools/` | smoke tests, QEMU launch script, icon/font/wallpaper/repo generators |
| `docs/` | screenshots |

## Architecture notes

- **Higher half at `0xC0000000`.** `boot.asm` identity-maps the first 12 MiB (PDE 0 to 2)
  and mirrors them at PDE 768 to 770 before enabling paging, then jumps to the virtual
  address and clears the identity entries for the first 8 MiB. User address space runs
  from 0 to `0xC0000000`.
- **Recursive page tables.** PDE 1023 points at the page directory itself, so any page
  table is reachable at `0xFFC00000 + (pde << 12)` without a temporary mapping.
- **Linux i386 ABI over `int 0x80`.** Arguments arrive in EBX, ECX, EDX, ESI, EDI, EBP,
  which is why `mmap2` reads its file offset from `regs->ebp`. The kernel adds four
  numbers of its own outside the Linux table, 500 to 502 for shared memory and 505 for
  the desktop kill target.
- **One Big Kernel Lock.** Application processors run user threads concurrently, but any
  CPU entering the kernel takes the recursive BKL. The first user process dispatched
  releases it in `forkret` on the way to user mode.
- **Boot filesystem then disk.** The initrd is a flat ustar archive that lives in RAM for
  the whole session, so the large GTK and Firefox trees are kept off it and shipped on
  the ext2 disk instead (`INITRD_EXCLUDE` in the Makefile). The disk is also overlaid on
  `/`, so writes to `/home` land on `/disk/home`.
- **maeroX speaks the X11 wire protocol** over AF_UNIX rather than emulating a toolkit,
  which is why the unmodified libX11 and libxcb from upstream work against it.

For the longer story of how the browser stack was built up, phase by phase, see
[README-BROWSER.md](README-BROWSER.md). For the planned direction, see
[ROADMAP.md](ROADMAP.md).

## Third-party code

| Component | Version | Location | License |
|---|---|---|---|
| lwIP | 2.2.1 | `third_party/lwip` | BSD 3-clause (`third_party/lwip/COPYING`) |
| toybox | 0.8.13 | `third_party/toybox` | 0BSD (`third_party/toybox/LICENSE`) |
| fbDOOM | id Software Doom source | `ports/fbDOOM` | GPL v2 |
| BusyBox | 1.35.0 | `ports/busybox`, `ports/packages/busybox` | GPL v2 |
| links | 2.30 | `ports/links-2.30.tar.gz`, `ports/packages/links` | GPL v2 |
| zlib, libpng, libjpeg | 1.3.1, 1.6.43, 9f | upstream tarballs in `ports/` | zlib, libpng and IJG terms |
| libX11 / libxcb, GLib, Cairo, Pango, GTK3 | built by `ports/x11` and `ports/gtk` | not vendored | upstream terms |
| Firefox ESR | 115.15.0 | fetched by `ports/firefox` | upstream terms |
| Doom shareware WAD | `doom1.wad` | `ports/doom1.wad` | id Software shareware terms |

Each of these keeps its own license. Nothing in this list has been relicensed.

## License

MaeroOS (the kernel, userland, tools and documentation) is released under the MIT
License, see [LICENSE](LICENSE). The vendored and imported components listed above
remain under their own licenses; the MaeroOS backends inside `ports/fbDOOM` are
derivative works of the GPL v2 Doom source and stay GPL v2.
