# MaeroOS

[![CI](https://github.com/Maero47/MaeroOS/actions/workflows/ci.yml/badge.svg)](https://github.com/Maero47/MaeroOS/actions/workflows/ci.yml)

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
  `proc/syscall.c` dispatches 225 Linux syscall numbers, so software built with an
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
| Boot, shell, procfs, PTYs, threads, shm, AF_UNIX, RNG, bad user pointers | `ls`, `cat`, `sysprobe ok`, `shmprobe ok`, `unixprobe ok` (stream, datagram and seqpacket sockets), `threadprobe ok`, `ptytest ok`, `cttytest ok`, `memprobe ok`, `wxprobe ok` (read-only text/rodata/mprotect'ed pages refuse writes, also after fork), `/proc/self/status` | `make smoke` |
| toybox 0.8.13, syscall edge cases, signals, timers, libc, job control, `pkg` archive checks | `TOYBOX_OK` plus about 90 applet checks, `sysmiscprobe ok`, `abi2probe ok` (open modes, `O_APPEND`, groups, `access`), `sigexecprobe ok`, `sigshareprobe ok` (process-wide pending signals, group stop), `timerprobe ok` (`alarm`, `setitimer`, `timer_*`), `LIBCTEST PASS`, `^Z`/`jobs`/`fg` on a pipeline, `pkg` refusing `../etc`, and the host-side `tools/test_pkg_tarx.py`, `tools/test_pkg_sign.py` and `tools/test_regex.py` | `make smoke-toybox` |
| Native coreutils-style commands, user faults | `kwprobe ok` (a user write to kernel memory dies of SIGSEGV, a user `int3` of SIGTRAP), then `uname`, `whoami`, `hostname`, `free`, `df`, `uptime`, `which` | `make smoke-cmds` |
| ext2 disk, symlinks, login/passwd/doas, permissions, init services, sessions | 169 checks including `passwd` keeping `/etc/shadow` root-only 0600, `login` dropping to `uid=1000`, `doas` ignoring a planted `./ls`, `credprobe ok`, `fsprobe ok`, `symprobe ok` (ext2 symlinks, checked on the image with `debugfs` too) and `svc` state transitions | `make smoke-disk` |
| SATA AHCI disk (pc + `-device ahci`, then `-M q35`), no IDE disk | `/disk` mounted from `ahci0`, `diskprobe ok`, `fsprobe ok`, `symprobe ok`, a 3 MiB random file read and copied with matching md5 in the guest and on the host (`debugfs`), persistence across a reboot onto q35, `e2fsck -fn` showing no new damage, `reboot`/`poweroff` ending QEMU | `make smoke-ahci` |
| NVMe disk (pc + `-device nvme`, then `-M q35` with MDTS=2 and two namespaces), no IDE/AHCI disk | `/disk` mounted from `nvme0`, both namespaces found, `diskprobe ok`, `fsprobe ok`, a 3 MiB random file read and copied with matching md5 in the guest and on the host, transfers using PRP lists and split at MDTS (driver counters at shutdown), persistence across a reboot, `e2fsck -fn` clean of new damage, `reboot`/`poweroff` ending QEMU | `make smoke-nvme` |
| `pkg` and the signed repo index | `tools/test_pkg_sign.py` runs the RFC 8032 vectors through the Python signer and the C verifier and rejects altered, unsigned and foreign-key indexes; in the guest, `pkg update` rejects unsigned, tampered and rolled-back indexes, installs and runs busybox, and `install` rejects a tarball whose SHA-256 does not match | `make smoke-pkg` |
| TCP/IP over lwIP and RTL8139 | `MAEROS_HTTP_OK` fetched from a host HTTP server; `sockprobe` checks that `send` after `shutdown(SHUT_WR)` fails with `EPIPE`, a two-step shutdown ends in a FIN and no RST, closing TIME_WAIT sockets keeps TCP working, and a non-blocking client gets `EINPROGRESS`, `SO_ERROR`, `EAGAIN`, `ECONNREFUSED` and both socket names; `abi2probe net` checks `MSG_NOSIGNAL` and `EPIPE` after a reset | `make smoke-net` |
| Intel e1000, DHCP DNS, name resolution | the same suite on an e1000; the DHCP lease's DNS server is in `/etc/resolv.conf`; against a DNS responder in the harness, `getent` resolves A, AAAA, a CNAME, a PTR and an NXDOMAIN, `/etc/hosts` wins over DNS, and `httpget`, `toybox wget` and `toybox nc` connect by name | `make smoke-net-e1000` |
| Kernel firewall | after `fwctl enable` plus a drop rule the same fetch fails, `fwctl list` reports the firewall `enabled` with the `drop out tcp` rule, malformed rules (`/33`, an overflowing prefix, port 70000, `tcpp`) are rejected under `policy out drop`, and `fwctl flush` restores the fetch | `make smoke-fw` |
| Dynamic linker | `DYNPROBE_OK` from a PIE loaded through musl `ld.so`; `WXPIE_OK` (the PIE's text and RELRO, libc's text and a `PROT_READ\|PROT_EXEC` library mapping are read-only) | `make smoke-dyn` |
| External shared libraries and pthreads | `GREET_OK sum=42`, `ZLIB_OK ver=1.3`, `THREADS_OK count=200000`, `UNIX_SOCK_OK` | `make smoke-dynlib` |
| X11 server | `XHANDSHAKE_OK`, `XDRAW_OK` (`w=320 h=200` from `GetGeometry`), `XEVENT_OK`, and `XREAL_PAINTED` from a client linked against the cross-built libX11 | `make smoke-x` |
| Desktop and its apps | driven with QMP mouse and keyboard input on the ISO: the launcher opens the terminal, a command typed into it runs in the terminal's own shell (its pid is checked), the image viewer shows `/disk/wallpaper.ppm`, Files enters a directory by double-click, Settings applies an accent (the desktop reloads, `desktop.conf` changes), the Store shows its verified list or the "run pkg update" state, the Task Manager lists the desktop and the Store, windows close by button, Esc and Alt-Tab and the focus passes to the topmost window left; every window must also show up in a screendump | `make smoke-gui` |
| GLib, Cairo, Pango, GTK3 | `GLIB_OK` (v2.78), `CAIRO_OK rect_px=0xe69919`, `PANGO_OK`, `GTK_OK init`, `GTK_WINDOW_SHOWN`, `GTK_DRAWN` (needs probe binaries a fresh clone lacks, see Testing) | `make smoke-gtk` |
| ACPI (uACPI 6.1.0) | on QEMU's `pc` and `q35` machines the kernel finds the RSDP, loads the AML namespace (`[ACPI] ready`), `poweroff` ends QEMU through S5 (`\_PTS`, `\_S5`), `reboot` restarts it through the FADT reset register (q35) or 0xCF9 (pc, whose FADT has none), and the ACPI power button (`system_powerdown`) reaches init as SIGUSR2 and powers off | `make smoke-acpi` |
| PC without legacy devices | `-M q35,i8042=off -smp 2`: no PS/2 controller (`[KBD]  no PS/2 controller`), `/disk` from `ahci0`, both CPUs from the MADT, the desktop's Terminal opened and typed into with a USB tablet and keyboard, and `doas poweroff` typed there makes QEMU exit through S5 | `make smoke-pc` |
| Firefox 115.15.0esr | `ff: Firefox painted` (the browser window, about 5 s after `firefox-bin` starts); with `--web`, a page served from the host (HTML, a CSS rule, a PNG) requested and its image on screen about 3 s after Enter (needs the Firefox tree, see `ports/firefox/`) | `make smoke-firefox`, `make smoke-firefox-web` |

### What does not work

- **Firefox 115.15.0esr is usable, not finished.** The prebuilt i686 ESR build paints
  its window and loads pages over HTTP and HTTPS (a real `https://example.com` loads
  through QEMU's user network, DNS and TLS included), but: text in scripts the disk has
  no font for (it ships DejaVu Sans, Serif and Sans Mono, which cover Latin, Greek and
  Cyrillic, and Twemoji; not CJK or Indic) is drawn as missing-glyph boxes; the
  content sandbox is off (`MOZ_DISABLE_CONTENT_SANDBOX`, `security.sandbox.content.level
  0`); startup still takes about 5 s after `firefox-bin` starts; and `ff` has to bring
  its own profile (`testfiles/ffprofile`) that turns off first-run dialogs, telemetry
  and add-on scans. `docs/audit/firefox-first-paint.md` and `docs/perf/firefox-startup.md` record how it
  got here.
- **No execute protection (NX).** The kernel runs i686 page tables without PAE, which
  have no no-execute bit, so every readable user page is also executable. Write
  protection is enforced: `proc/elf.c` maps each `PT_LOAD` segment with its own
  `p_flags` (`.text` and `.rodata` read-only, the in-tree programs are linked by
  `userspace/user.ld` into separate R-X and RW segments), `ld.so` can `mprotect` a
  `PT_GNU_RELRO` range read-only, and a write to a read-only page is `SIGSEGV`
  (`SEGV_ACCERR`), in a forked child as well.
- **inotify is deliberately absent.** Numbers 291, 292, 293 and 332 return `-ENOSYS`
  on purpose so GLib falls back to its polling backend (see the comment on those
  cases in `proc/syscall.c`).
- **Unfinished credential and socket semantics.** `setfsuid`/`setfsgid` just report
  the effective id; a path lookup does not check search permission on the directories
  it walks through; AF_INET sockets have no `listen`/`accept` (`-EOPNOTSUPP`), and a
  blocking UDP `recv` returns `EAGAIN` instead of waiting.
- **One repo signing key per build host.** `index.txt` is Ed25519-signed and `pkg`
  checks it, but the key is created per host (`~/.config/maeros/repo-signing.key`) and
  its public half is compiled into `pkg`: a repo and a `pkg` built on different hosts,
  or across a key change, do not work together. There is no key rotation or revocation,
  and the rollback check is only as strong as the cached index, which the desktop user
  owns.
- **No SMP scaling.** One Big Kernel Lock serialises all kernel execution
  (`arch/i686/cpu/bkl.c`).
- **Missing Linux interfaces.** There is no `mount` syscall, no SysV IPC and no utmp.
  `/proc/<pid>/` has only `status` and `stat` (the full set is under `/proc/self`), and
  there is no `/proc/stat`, so toybox is built without `killall` (it matches names
  through other processes' `cmdline`), `vmstat` and `who`. There is no IPv6 stack:
  the resolver returns AAAA records, but only IPv4 sockets connect.
- **Only Linux and macOS hosts are covered.** The Makefile looks up `mke2fs`, `debugfs`
  and GNU `sed` on `PATH` (with the Homebrew locations as a fallback) and
  `tools/run-maeros.sh` picks the QEMU display, audio and screen-size probes per host,
  but nothing has been tried on Windows or the BSDs.

### Security notes

- **Default passwords.** `testfiles/etc/shadow` is committed with `root`/`root` and
  `user`/`user` (PBKDF2 hashes), so every image built from the tree has them, and the
  desktop runs as `user` without asking. Change them with `passwd` on anything that
  is reachable from a network.
- What is enforced: per-segment write protection (not NX, see above), file and
  credential checks, guarded kernel stacks, a signed package index, and a maeroX that
  validates every request length and has no key-injection channel in the desktop
  build.
- What is not: the Firefox content sandbox, execute protection, search permission on
  path lookups, and repo-key rotation (all under "What does not work").

## What is in the box

### Kernel

- Multiboot 1 higher-half kernel: `arch/i686/boot/boot.asm` sets up paging before jumping
  to `0xC0000000`, `linker.ld` links the image at `KERNEL_VMA + 0x00100000`.
- Boot order lives in one readable function, `kernel_main` in `kernel/main.c`: serial,
  RTC, GDT/TSS/IDT/FPU, PIC, VGA, physical memory, RNG, paging, LAPIC, framebuffer,
  heap, VFS, initrd, network, PCI and its drivers, ATA/ext2, tmpfs, devfs, procfs,
  scheduler, input, PIT, TSC, kernel threads, application processors, then `/disk/init`
  or `/init`.
- About 31,600 lines of C, headers and assembly across the kernel directories, of which
  `proc/syscall.c` is about 9,300.
- Limits in `include/kernel/config.h`: `MAX_PROCS` 256, `MAX_FD` 512, 32 KiB kernel
  stacks, a 256 MiB kernel heap window at `0xD0000000`, 64-page user stacks below
  `0xC0000000`.
- Kernel stacks (`mm/kstack.c`) live outside the heap in 512 slots at `0xF0000000`,
  each below a 32 KiB unmapped guard, so an overflow faults instead of corrupting a
  neighbour. A double fault switches to a per-CPU task with its own stack
  (`arch/i686/cpu/dfault.c`) and reports over serial instead of resetting the machine.

### Memory

- `mm/pmm.c`: bitmap frame allocator with a per-frame `uint16_t` refcount array and a
  use-after-free detector that reports frames handed out with a stale refcount.
- `arch/i686/mm/paging.c`: recursive page-directory self-map at PDE 1023, copy-on-write
  fork, demand-paged anonymous VMAs, and an exception table (`__start___ex_table`) so a
  faulting `copy_from_user`/`copy_to_user` (`proc/syscall.c`) returns `-EFAULT` instead
  of panicking. The kernel touches user memory only through these copies; file and
  socket I/O is bounced through kernel buffers, and a user fault on a kernel page is a
  SIGSEGV rather than a retry loop.
- RAM above 512 MiB boots: the higher-half direct map stops at 256 MiB (the heap window
  at `0xD0000000`) and frames above it are reached through temporary maps, so the kernel
  reaches the shell with `-m 1024M` and `-m 2048M` (`run-firefox` uses 2 GiB).
- `mm/heap.c`: TLSF-style segregated free lists over boundary-tagged blocks, with O(1)
  coalescing; a `0xDEADBEEF` magic and a used/free state word are checked on free,
  `realloc` and `heap_check`.

### Processes, threads and IPC

- `sys_clone` handles `CLONE_VM`, `CLONE_SETTLS` (per-thread TLS through GDT entry 6),
  `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID` and `CLONE_CHILD_CLEARTID`, which is what
  makes `pthread_join` return.
- Timers and waits: `alarm`, `setitimer`/`getitimer` and the POSIX `timer_*` calls
  (`proc/ktimer.c`), `waitid` and `getrusage`.
- Credentials follow Linux: real, effective and saved set-IDs (`proc/process.h`),
  `setuid`/`setreuid`/`setresuid` and their gid twins with the unprivileged-move rules,
  set-uid exec, and supplementary groups (`setgroups`/`getgroups`). `vfs_access_check` (`fs/vfs.c`) applies owner/group/other bits to
  open, exec, directory changes and `access`; `chmod`/`chown` are owner- or root-only.
  `/tmp` is sticky, new files are owned by the effective ids with the creation mode
  minus the umask, `O_EXCL` and `O_APPEND` are honoured, and `rename`
  is atomic within one filesystem and fails with `EXDEV` across filesystems. `kill`
  checks the sender against the target's ids; a SIGKILL also reaches the target's
  descendants, each of which must pass the same check.
- `futex` (WAIT/WAKE, REQUEUE, WAKE_OP and the BITSET variants) backs musl and glibc
  mutexes and condition variables.
- `proc/usocket.c`: AF_UNIX stream, datagram and seqpacket sockets with `socketpair`,
  named bind/listen/connect, `sendmsg`/`recvmsg`, `SCM_RIGHTS` file-descriptor passing
  and `SO_PEERCRED`, integrated into `poll` and `select`. This is the transport X11
  clients connect over.
- `proc/pipe.c` pipes and FIFOs, `proc/shm.c` shared memory (segments have an owner and
  a mode, changed with syscall 506), `proc/signal.c` signal delivery with a
  `sigcontext`/`ucontext` frame laid out exactly as glibc expects, so a handler can read
  `uc_mcontext`; process-directed signals wait in a pending set shared by all threads,
  and a stop signal stops the whole thread group.
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

`fs/vfs.c` mount table with a root overlay and path lookup (symlinks are followed
iteratively with a 40-link budget, then `ELOOP`), `fs/initrd.c` ustar archive read from
the Multiboot module, `fs/ext2.c` (read and write, mounted at `/disk` and overlaid on
`/`, symlinks included), `fs/tmpfs.c` at `/tmp` (file bodies in page frames rather than
the kernel heap, capped at 1 GiB per file, `EFBIG` beyond), `fs/devfs.c` at
`/dev` (`null`, `zero`, `tty`,
`ptmx`, `pts/`, `random`, `urandom`, `fb0`, `dsp`, `shm`, `input/event0`, `input/event1`,
`stdin`/`stdout`/`stderr`), and `fs/procfs.c` at `/proc` (per-pid `status` and `stat`;
`self` adds `statm`, `maps`, `fd`, `cmdline`, `environ`, `auxv`, `exe`; plus `meminfo`, `version`,
`uptime`, `cpuinfo`, `kmsg`, `processes`, `pci`, `netif`, `firewall`, `sys/vm/`).

### Networking

lwIP 2.2.1 is vendored at `third_party/lwip` and driven by `net/lwip_glue.c` over the
RTL8139 (`drivers/rtl8139.c`) or Intel e1000 (`drivers/e1000.c`) driver, whichever is
found first. DHCP runs at boot, and the lease's DNS servers are written to
`/etc/resolv.conf` when `/etc` is writable (a disk is attached); `net/socket.c` implements the
BSD socket calls both through `socketcall` (102) and the direct i386 numbers 359 to 373;
`net/firewall.c` is a rule-based packet filter configured by `fwctl` and readable at
`/proc/firewall`; a kernel thread `knetd` (`net/net.c`) keeps timers and TCP alive
without userspace polling.

### Drivers

ATA with bus-master DMA reads and PIO writes (`ata.c`), SATA AHCI with DMA reads and writes on every
controller and port (`ahci.c`), NVMe with one polled I/O queue pair per controller and every 512-byte
namespace as a disk (`nvme.c`; `/disk` mounts from the IDE master if there is one, else the first AHCI
disk, else the first NVMe namespace, via `blkdev.c`), PCI enumeration (`pci.c`), RTL8139 (`rtl8139.c`), Intel 8254x e1000 (`e1000.c`), Intel 82801AA AC'97
audio (`ac97.c`), Multiboot VBE framebuffer (`framebuffer.c`), VGA text (`vga.c`), PS/2
keyboard and mouse (`keyboard.c`, `mouse.c`), CMOS RTC (`rtc.c`) and 16550 serial
(`serial.c`).

USB (`drivers/usb/`): an xHCI host controller driver (`xhci.c`) whose kernel thread
`kusbd` enumerates root-hub ports and USB 2.0 hubs (hot-plug included) and polls the
event ring every tick; HID keyboards, mice and tablets (`usb_hid.c`: boot protocol, or a
report-descriptor parser for absolute pointers) feed the same `/dev/input/event0` and
`event1` as the PS/2 drivers; mass storage (`usb_msc.c`, bulk-only SCSI) appears as the
raw block device `/dev/usbdisk0`. Not yet: USB 3 hubs, interrupts/MSI, and mounting a
filesystem from the stick. Run it with `-device qemu-xhci -device usb-kbd -device
usb-tablet` (and `-device usb-storage,drive=...`).

### Userland

`userspace/` is about 39,500 lines across 228 source files, built with the same
`i686-elf-gcc` and
linked against its own freestanding libc (`userspace/libc/`: syscall stubs, stdio, stdlib,
string, dirent, termios, sockets, signals, time, regex, a small libm, a DNS resolver
(`netdb.c`: `getaddrinfo`/`getnameinfo` over `/etc/hosts` and `/etc/resolv.conf`),
pthreads and a toybox compatibility layer, entered from `crt0.asm`). `userspace/Makefile` installs the binaries into
`testfiles/`.

**init** (`userspace/init/init.c`) prefers a disk userland over the initrd, runs `/etc/rc`
through the shell, then reads two tables: `/etc/inittab` for console sessions to respawn
(up to 4) and `/etc/services` for `once` and `respawn` services (up to 8, each separately
enabled or disabled). It opens a control FIFO at `/tmp/initctl`, writes
`/tmp/services.status` and `/tmp/sessions.status` for `svc` and `session` to read back,
and appends to `/var/log/init.log` when the disk root is writable. Respawn has a backoff:
only an exit within 3 seconds counts as a rapid failure, each one adds 0.25 s of delay up
to 1 s, and after 8 in a row the session is parked for 30 s before init tries again. The
console session is `getty`, on the disk and on an initrd-only boot alike: it prints a
`maeros login:` prompt and execs `login`, which asks for the password (and waits 3 s
after a wrong one), drops to the account's ids and starts its shell. Both ignore ^C and
^Z until the shell runs: at the login prompt they just cancel the line, at `Password:`
they fail that attempt; logging out ends the
session and init respawns getty. When a framebuffer is present, init plays a startup
chime through `wavplay` and runs the desktop as the unprivileged user, with the console
getty running alongside it.

**The shell** (`userspace/shell/shell.c`) handles single and double quoting,
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
in `userspace/auth/auth.c` and stored in `/etc/shadow` (root-only, 0600). The accounts
are `root` and `user` (uid 1000); the default `user` password is `user`, stored hashed.
`login` switches to the target user's ids before starting the shell, `passwd` rewrites
the shadow file through a replace helper (`userspace/auth/replace.c`) that keeps
`/etc/shadow-` as a backup, and `doas` runs commands only from a fixed
`PATH=/disk:/disk/bin:/:/bin`, never the caller's. `/disk/etc` is 0755 root
(`tools/diskperms.txt`), so an ordinary user cannot add files to it. Eighteen BusyBox
applet names
(`vi`, `less`, `ping`, `mount`, `gzip` and others) are installed into `/bin` as copies of
a small `bbwrap` launcher.

### Desktop and window system

`desktop` (`userspace/desktop/desktop.c`) opens `/dev/fb0`,
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

`maerox` (`userspace/maerox/maerox.c`) is the X11 server. It takes a desktop
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
bitmaps. Client input is not trusted: every request is checked against its minimum
length and its value list (BadLength), glyph uploads and `GetImage` sizes are capped
(BadAlloc), and a zero-length (BIG-REQUESTS) request drops the client.

`store` and `pkg` install packages from a repository built by `tools/mkrepo.py` out of
`ports/packages/*/pkg.conf`. `pkg update`, `list`, `install` and `remove` fetch
`index.txt` and per-package ustar archives over HTTP into `/disk/apps/<name>/`, defaulting
to the QEMU host at `10.0.2.2:8000` and overridable in `/disk/etc/pkg.conf`. The index
is signed with Ed25519: `make repo` creates the signing key on first use in
`~/.config/maeros/repo-signing.key` (or `$MAEROS_REPO_KEY`), outside the tree, and the
userspace build compiles its public half into `pkg` (`userspace/pkg/repo_pubkey.h`,
generated). `pkg` (verifying with TweetNaCl, `third_party/tweetnacl`) refuses an index
that is unsigned, altered or signed by another key, and one whose serial (the build
time) is lower than the cached index's, and re-checks the cached index before every
`list` and `install`. `pkg`
refuses a tarball whose SHA-256 does not match the index, package names that are paths,
and archives with anything but regular files under flat, plain names, checking every
header before it writes; `store` is
the libgui front end that drives the `pkg` binary and checks the cached index's
signature itself before it lists anything. `make repo-serve` serves the repository
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
`-d ~/opt/hostpkgs/usr/lib/grub/i386-pc` for `grub-mkrescue` and `grub-mkimage` (also
written around the system binary when `grub-common` is installed without `grub-pc-bin`,
which otherwise builds an EFI-only ISO that QEMU cannot boot), `M4=` for bison and flex,
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
without them it cannot build the ISO; `--check` fails while the `grub-mkrescue` on `PATH`
cannot reach BIOS modules. `make iso` passes the relocated modules with `-d` itself when
`~/opt/bin` is not on `PATH`, and stops with the fix when there are none. `--check` lists
which tools come from wrappers and which of those are already superseded.

`make initrd` also builds toybox from `third_party/toybox/.config.maeros`, the applet set
that compiles and links against the MaeroOS libc (about 160 applets; the rest need
headers or syscalls the libc or kernel does not provide yet).

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
make run-net        # -kernel boot with an RTL8139 on QEMU user networking (NIC=e1000 for an e1000)
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

Everything goes to the serial console, so `make run` gives you the boot log and a
`maeros login:` prompt in your terminal. Log in as `root` (password `root`) or `user`
(password `user`); `exit` logs out and init starts a new getty. The console reads only
the serial line, so the login is in the terminal QEMU was started from, and it stays
available while the desktop runs (`make run-iso`, `make run-firefox`; the desktop itself
runs as `user` without a login, and `make start` writes the serial line to
`/tmp/maeros_serial.log` instead of a terminal). Change the passwords with `passwd`. The boot log is also readable
inside the guest with `dmesg` and at `/proc/kmsg`.

## Testing

The smoke targets each boot QEMU, log in as `root` at the console getty (the Firefox
ones watch the desktop session instead), drive the guest
shell over the serial console and assert on the output (the login and the headless QEMU
flags are shared in `tools/smokelib.py`). They are the project's regression suite;
`tools/smoke.py` is the baseline the others build on, so it is the one to read first.

```sh
make smoke          # boot, shell, procfs, PTYs, threads, shm, getrandom, ps
make smoke-cmds     # native uname/whoami/hostname/free/df/uptime/which
make smoke-toybox   # toybox as a static guest binary
make smoke-disk     # ext2, login/passwd, init sessions, service supervision, overlay
make smoke-ahci     # /disk on a SATA AHCI controller only (pc and q35)
make smoke-nvme     # /disk on an NVMe namespace only (pc and q35)
make smoke-net      # DHCP, TCP, HTTP GET from the host
make smoke-net-e1000  # the same on an e1000, plus resolv.conf from DHCP and DNS lookups
make smoke-pkg      # pkg against a host repo: signed index, install, rollback
make smoke-fw       # firewall rule blocks and unblocks that GET
make smoke-dyn      # PIE through the musl dynamic linker
make smoke-dynlib   # external .so files, zlib, pthreads, AF_UNIX
make smoke-x        # maeroX handshake, drawing and input events
make smoke-gtk      # GLib, Cairo, Pango and a real GTK3 window
make smoke-gui      # the desktop, driven by mouse and keyboard (needs the ISO)
make smoke-abi      # Linux-ABI probes (ports/abiprobes/README.md), needs i686-linux-musl-gcc and disk.img
make smoke-firefox  # does Firefox paint? (README-BROWSER.md)
make smoke-firefox-web  # ...and load a page served from the host over the network
```

`SMOKE_SMP=N make smoke-cmds` boots the same guest with `-smp N`. `make smoke-gtk` needs
`testfiles/cairoprobe`, `pangoprobe` and `gtkprobe`, which are not in git: they are
linked by hand inside the GTK build container (`ports/gtk/`), and without them the
script stops with instructions and runs nothing. `make KTRACE=1` builds the kernel with
the hot-path debug traces (per-exec, per-signal and per-fault lines, `kprof` probe spans
and its periodic dump); the setting is recorded in `.ktrace-stamp`, so switching it
rebuilds the kernel objects. Two debug-only self-tests use the same stamp:
`make KSTACK_TEST=1` to `4` overflows a kernel stack on purpose (the serial log must
show the double-fault or guard-page report, not a reset), and `make KHEAP_TEST=1` runs
a randomised heap stress with poisoned free memory (`[HEAP-TEST] PASS`; `=2` adds a
deliberate use-after-free that must end in a heap-corruption panic). Neither belongs
in a normal build.

`python3 tools/smoke_firefox.py --sites default` drives Firefox through a few live
sites and reports load and scroll times without judging them; it is manual only
(README-BROWSER.md).

Each script exits non-zero and prints the failing expectation, for example
`command 'threadprobe' did not produce 'threadprobe ok'`.

### GUI smoke test

`make smoke-gui` (`tools/smoke_gui.py`, about 30 s) boots `maeros.iso` with a copy of
`disk.img` (minus the `ffauto`/`gtkauto` autostart markers), `-vga std` and
`-display none`, and drives the PS/2 mouse and keyboard with QMP `input-send-event`.
The desktop and the apps print one line per state change to the serial console
(`gui_trace()` in libgui, `trace()` in `desktop.c`), for example

```
[desktop] launcher open settings=325,542
[desktop] window opened: Files slot=3 x=436 y=116 w=430 h=390 close=837,126
[files] dir proc at 55,120
[files] cwd /proc entries=17
[desktop] window closed: Files slot=3
```

and the test waits on those lines. They also carry the positions of the controls the
test clicks (the orb, the close button, a directory row, the Apply button), so the test
does not copy layout constants, and the desktop traces every click's position, which
the test compares with where it aimed. Side effects are checked from a root shell on the
serial getty (the file the terminal command wrote, `/disk/etc/desktop.conf`), and each
window must change its area of a screendump when it opens. Screendumps (`NN-step.png`)
and `serial.log` go to `build/smoke-gui/`; a failure adds `fail.png`. It uses KVM when
`/dev/kvm` is usable and TCG otherwise (`SMOKE_GUI_ACCEL=tcg` forces it); both take
about the same time.

A new app becomes testable by calling `gui_trace("app", ...)` where its state changes,
with click targets as window-relative points (surface point + `GUI_BODY_X`/`GUI_BODY_Y`).

`make smoke-usb` (`tools/smoke_usb.py`, about 35 s) does the same with USB input only:
a `qemu-xhci` with a `usb-kbd` and `usb-tablet` bound to the VGA display (so QMP
`input-send-event` reaches them and never the PS/2 devices), and a `usb-hub` with a
`usb-mouse` and a `usb-storage` stick behind it. It opens the Terminal and logs in
(`doas login root`, both passwords typed on the USB keyboard), checks that tablet clicks
land where sent and that the boot mouse moves the pointer, reads and writes the stick
through `/dev/usbdisk0` (checksummed against the image file) and unplugs and replugs it.

`make smoke-pc` (`tools/smoke_pc.py`, about 15 s) boots a PC with no legacy devices:
`-M q35,i8042=off -smp 2`, the disk on q35's AHCI controller, and a `usb-kbd` and
`usb-tablet` on a `qemu-xhci` as the only input. It checks that the kernel skips the
missing PS/2 controller, mounts `/disk` from `ahci0`, starts both CPUs from the MADT and
enumerates the USB devices, opens the Terminal with them, and types `doas poweroff`
there: QEMU, started without `-no-shutdown`, must exit through ACPI S5.

### Continuous integration

`make check` runs the suites that need nothing beyond a fresh clone: `smoke`,
`smoke-cmds`, `smoke-toybox`, `smoke-disk`, `smoke-net`, `smoke-net-e1000`, `smoke-fw`,
`smoke-dyn`, `smoke-dynlib`, `smoke-x`, `smoke-pkg` (which first builds `repo/` and, on a
host without one, a repo signing key), `smoke-gui` (which needs the ISO, so `check`
builds it), `smoke-acpi`, `smoke-ahci`, `smoke-nvme`, `smoke-usb` and `smoke-pc`. It runs them one after another, writes each suite's
console to `build/check/<suite>.log`, prints the tail of the log for any suite that
fails, carries on with the rest and exits non-zero at the end. `CHECK_SUITES="smoke
smoke-x" make check` runs a subset.

GitHub Actions (`.github/workflows/ci.yml`) runs the same command on every push and
pull request, on `ubuntu-latest`:

```sh
tools/setup-linux.sh --apt --no-musl      # host packages + i686-elf toolchain
make -j"$(nproc)" all initrd disk iso
make check
```

The smoke suites start QEMU with `-display none`, so they need no display (ssh, CI);
`SMOKE_DISPLAY=1` brings QEMU's window back for watching a run.

The built `~/opt/cross` toolchain is cached, keyed on the binutils/gcc versions, digests
and configure flags in `tools/setup-linux.sh` and on the runner's Ubuntu release, so only
the first run and version bumps pay for the gcc build. When `make check` fails, the
`build/check/` logs, and the `smoke-gui` screendumps and serial log, are uploaded as a
`smoke-logs-*` artifact of the run. The smoke suites do not use KVM (`make run` boots
QEMU with its default TCG accelerator), except `smoke-gui`, which uses it when
`/dev/kvm` is usable.

### Diagnosing a hang

A wedged boot prints nothing, so three things exist to make one visible.

* **`kwatch`** (`kernel/kwatch.c`) watches the syscall counter from the timer
  tick.  Twenty seconds without a single syscall is a stall, and it then prints
  how the window's cycles split between user, idle and kernel — an all-idle
  window means every thread is asleep and a wake-up was missed — followed by one
  line per live process: state, how long it has waited, the pipe / AF_UNIX ring
  / poll channel it is waiting on (decoded by scanning the fd tables), its last
  syscall and its user EIP.  The per-tick cost is one compare, so it is always
  on.  The same dump is printed on an **NMI**, which is delivered even when
  interrupts are off — that is the way into a guest spinning inside a syscall.
* **`tools/smoke_firefox.py`** photographs a wedged guest through the QEMU
  monitor before killing it: `info registers`, `info pic`, `info cpus` and the
  instructions at `$pc`, sampled four times, then an injected NMI whose dump
  lands in `serial.log`.  It writes `wedge-qmp.txt` next to the run's other
  artifacts.  Read `EFL`'s IF bit first: syscalls enter through an interrupt
  gate, so IF=0 with CPL=0 means the guest is spinning in a syscall and no
  interrupt can reach it — `info pic` then shows IRQ0 stuck in `irr`.
* **`tools/ff_boot_loop.py`** boots `smoke_firefox.py` over and over (one QEMU
  at a time) and reports the failure rate, the first-paint mean and where each
  failing run's artifacts landed, so an intermittent hang can be measured
  instead of guessed at:

  ```sh
  python3 tools/ff_boot_loop.py -n 60          # a campaign, ~1 hour
  python3 tools/ff_boot_loop.py -n 20 --stop-after-fails 1 -- --smp 2
  ```

## Repository layout

| Path | Contents |
|---|---|
| `arch/i686/` | boot and AP trampoline assembly, GDT/IDT/TSS/PIC/PIT, LAPIC, SMP, BKL, paging |
| `drivers/` | ATA, AHCI, NVMe, PCI, RTL8139, e1000, AC'97, framebuffer, VGA, keyboard, mouse, RTC, serial, ACPI (uACPI glue) |
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
| `third_party/` | vendored lwIP, toybox, TweetNaCl and uACPI |
| `tools/` | smoke tests, QEMU launch script, icon/font/wallpaper/repo generators |
| `docs/` | screenshots, the Firefox first-paint audit (`docs/audit/`) and startup profile (`docs/perf/`) |

## Architecture notes

- **Higher half at `0xC0000000`.** `boot.asm` identity-maps the first 12 MiB (PDE 0 to 2)
  and mirrors them at PDE 768 to 770 before enabling paging, then jumps to the virtual
  address and clears the identity entries for the first 8 MiB. User address space runs
  from 0 to `0xC0000000`.
- **Recursive page tables.** PDE 1023 points at the page directory itself, so any page
  table is reachable at `0xFFC00000 + (pde << 12)` without a temporary mapping.
- **Linux i386 ABI over `int 0x80`.** Arguments arrive in EBX, ECX, EDX, ESI, EDI, EBP,
  which is why `mmap2` reads its file offset from `regs->ebp`. The kernel adds seven
  numbers of its own outside the Linux table: 500 to 502 and 506 for shared memory
  (create, map, unmap, chmod), 503 and 504 to dump and reset the `kprof` cycle
  accounting, and 505 for the desktop kill target.
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
| TweetNaCl | 20140427 | `third_party/tweetnacl` | public domain (`third_party/tweetnacl/LICENSE`) |
| uACPI | 6.1.0 (`f5f6cc2`) | `third_party/uacpi` | MIT (`third_party/uacpi/LICENSE`, `NOTICE.maeros`) |
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
