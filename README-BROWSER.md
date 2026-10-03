# MaeroOS Browser Roadmap — from `browse` to Firefox

## Where we are

| Rung | Browser | Status |
|------|---------|--------|
| 1 | `browse` (native) | ✅ Text browsing, links, history, redirects |
| 2 | **links2 -g** (Linux port) | ✅ links 2.30 with framebuffer graphics, shipped as a package (`ports/packages/links`) |
| 3 | NetSurf framebuffer | Skipped: every rung after links2 went straight at Firefox |
| 4 | **Dynamic linking (ld.so)** | ✅ Phases 30 + 31: PIEs and external shared libraries load through the real musl `ld.so` |
| 5 | X11 compatibility layer | ✅ Phases 32b–34: AF_UNIX, the maeroX server, real libX11/libxcb and GTK3 (`make smoke-x`, `make smoke-gtk`) |
| 6 | **Firefox 115.15.0esr** (official i686 build) | ✅ Paints its window about 5 s after `firefox-bin` starts and loads pages over HTTP and HTTPS (`make smoke-firefox`, `make smoke-firefox-web`, `--sites` for live sites) |

What is still missing on rung 6 is listed under "What does not work" in
[README.md](README.md): the content sandbox has no namespaces ([docs/sandbox.md](docs/sandbox.md)), scripts DejaVu does not
cover (CJK, Indic) come out as missing-glyph boxes, and `ff` brings its own
profile. The sections below record how each rung was cleared, oldest first.

## ✅ Phase 30 — the dynamic linker

MaeroOS loads **dynamically-linked** ELF binaries, not just static ones. The
`dynprobe` PIE (built `i686-linux-musl-gcc -fpie -pie`) boots through the
genuine musl dynamic linker and prints `DYNPROBE_OK` (`make smoke-dyn`).
Mechanism:

1. **`proc/elf.c` — `elf_load_bias()`**: accepts `ET_DYN` (PIE / ld.so) with a
   load bias; `ET_EXEC` still loads at fixed VAs (bias forced to 0). PT_INTERP
   is *recorded*, not rejected. Adjacent-page sharing between segments is
   handled (reuse an already-mapped frame rather than clobber it).
2. **`proc/syscall.c` — exec**: a PIE loads at `0x10000000`; if it names an
   interpreter, the kernel loads `ld-musl-i386.so.1` at `0x40000000`, bumps
   `mmap_next` above the interpreter image, and **irets to the interpreter's
   entry** with a full aux vector — `AT_PHDR/PHENT/PHNUM` (of the *program*),
   `AT_BASE` (interpreter), `AT_ENTRY` (the program), `AT_RANDOM`, `AT_PAGESZ`.
   The interpreter then relocates, resolves libc symbols, and jumps to `main`.
3. The syscalls ld.so needs were already present: `mmap2` (anon + MAP_PRIVATE
   file), `mprotect` (RELRO), `set_thread_area` (TLS via GDT entry 6), `brk`.
4. The interpreter path resolves with fallbacks (`/lib/...` → `/<basename>` →
   `/disk/lib/...`) because the initrd is flat; the ld.so ships at
   `testfiles/ld-musl-i386.so.1` (→ `/ld-musl-i386.so.1`).

## ✅ Phase 31 — external shared libraries

Phase 30 ran a PIE whose only dependency was libc (which musl's ld.so *is*, so
it reused its own image). Phase 31 makes the loader handle **arbitrary external
`.so` files from disk** — the prerequisite for the entire GTK/Firefox stack.

- **The one enabling fix** (`proc/syscall.c` `sys_mmap2`): the syscall now reads
  the mmap2 file offset from `regs->ebp` (the i386 6th-arg register, captured by
  the `int 0x80` `pusha`) instead of hardcoding 0. A shared library's data/GOT
  segment lives at a nonzero file offset; with offset 0 it loaded garbage and
  crashed. Also frees the old frame on `MAP_FIXED` overlay (ld.so's
  reserve-then-map-segments pattern) so it doesn't leak.
- **Library search** (`userspace/init/init.c` + `/etc/ld-musl-i386.path`):
  `LD_LIBRARY_PATH=/:/lib:/disk:/disk/lib` so musl ld.so finds libs on the flat
  initrd and the ext2 disk (`AT_SECURE=0`, so the env is honoured).
- **Proofs** (`make smoke-dynlib`): `dynprobe2` calls into a custom external
  `libgreet.so.1` → **`GREET_OK sum=42`** (cross-module PLT/GOT relocation);
  `zprobe` links a real shared **`libz.so.1` (zlib 1.3.1, a Firefox dependency)**
  → **`ZLIB_OK ver=1.3.1`** with a compress/uncompress round-trip. Built by
  `ports/build-dynlib.sh`.
- **RAM**: default `make run` bumped 128M → **512M**. Later the direct map was
  capped at 256 MiB with the rest reached as high memory, so 1 GiB and 2 GiB
  boot too; `run-firefox` uses 2 GiB.

## ✅ Phase 32a — real pthreads

Firefox is heavily multithreaded, so threads are non-negotiable.  The kernel now
runs genuine multithreaded programs:

- **`sys_clone`** (`proc/syscall.c`) handles `CLONE_VM` (shared address space) +
  **`CLONE_SETTLS`** (each thread gets its own TLS base — the thread pointer at
  `%gs:0`) + `CLONE_PARENT_SETTID`/`CHILD_SETTID` + **`CLONE_CHILD_CLEARTID`**.
- **`proc_exit`** (`proc/scheduler.c`) writes 0 to the exiting thread's
  `clear_child_tid` word and futex-wakes it — that is how `pthread_join()`
  returns. `set_tid_address(258)` records the address.
- The scheduler already reloads per-thread TLS (GDT entry 6) on every switch,
  and `futex(240)` WAIT/WAKE already back musl's mutex/condvar.
- **Proof** (`make smoke-dynlib`): `pthreadprobe` spawns 4 threads that bump a
  mutex-guarded counter 50 000× each → **`THREADS_OK count=200000`** (no lost
  updates, per-thread TLS intact, all joined).

## ✅ Phases 32b–33 — AF_UNIX and the X server

1. **AF_UNIX sockets (Phase 32b).** `proc/usocket.c` adds local
   stream sockets — `socketpair`, named `bind`/`listen`/`connect`/`accept`, and
   bidirectional streaming, integrated into the FD layer (`FD_USOCKET`) and
   `poll`/`select`. Proven: `usockprobe` → `UNIX_SOCK_OK`. This is the transport
   X11/Wayland/D-Bus connect over.
2. **An X11 server (Phase 33).** 33a: `userspace/maerox/` is a
   native libgui app that owns a desktop window, listens on AF_UNIX
   `/tmp/.X11-unix/X0`, and completes the **X11 connection handshake** (byte-exact
   setup reply: 1 screen, a 24-bit TrueColor visual, standard pixmap formats).
   Proven: `make smoke-x` → `xprobe` → `XHANDSHAKE_OK`.
   **33b:** the request loop (CreateWindow, CreateGC, MapWindow,
   PolyFillRectangle, PutImage, GetGeometry, + the startup queries) renders
   client windows into the desktop surface — a real X client (`xdraw`) draws a
   window with a rectangle + gradient on the MaeroOS desktop (`XDRAW_OK`).
   **33c:** maeroX became a **fully interactive X server** — it sends
   `Expose`/`ConfigureNotify` on map and forwards clicks to clients as
   `ButtonPress`/`ButtonRelease` (`xevent` → `XEVENT_OK`; clicking an X window
   draws marks at the exact coordinates).
   **33d:** the **real libX11/libxcb** stack is cross-built for
   musl/i686 (`ports/x11/build-x11.sh`), and a **genuine Xlib application**
   (`xreal`, linked against the actual libX11 GTK/Firefox use) connects via
   `XOpenDisplay`, creates a window, and paints with `XFillRectangle` on the
   MaeroOS desktop (`XREAL_PAINTED`). The unlock was kernel `sendmsg`/`recvmsg`
   on AF_UNIX (libxcb's transport). The X client library is now **real**.
   **Keyboard:** keys travel PS/2 → `drivers/keyboard.c` → the desktop →
   the WM event channel (`rkey`, the uncooked stream) → maeroX → X11
   `KeyPress`/`KeyRelease` on the focused window. maeroX answers
   `GetKeyboardMapping` with the US layout (two keysyms per keycode),
   `GetModifierMapping` with the real Shift/Lock/Control/Alt keycodes, tracks an
   input focus that `SetInputFocus`/`GetInputFocus` agree with, and sends
   `FocusIn`/`FocusOut`. A release is delivered to whoever received the press —
   the desktop routes by slot, maeroX by window — so a focus change mid-keystroke
   cannot split a key in two. A **click cannot take the keyboard away** from the
   window a client asked for: the click hit-test and the compositor share one
   definition of "topmost" — creation order, never resource-array slot order —
   and a click offers the focus through the same kiosk policy a map does, so it
   stops at `focus_explicit`. Without that, clicking in a Firefox page moved the
   focus to the full-screen MozContainer child, a `FocusIn` GDK discards after a
   `FocusOut` it does not, and typing stopped working — on the runs where the
   two windows happened to land in that slot order. `xkey` proves it (`XKEY_OK`,
   which now also injects clicks with the slots arranged both ways),
   `python3 tools/smoke_firefox.py --type "<text>"` types `<text>` into
   Firefox's address bar with QEMU `sendkey` and saves `screen-typed.png`, and
   `--keycheck` asserts maeroX's key trace for pairing, `Mod1Mask` on an AltGr
   combination, that neither half of an Alt-Tab leaks to a client, and that the
   session has no key-injection channel. That channel — maeroX's XTEST
   stand-in, which the headless probe drives — takes an explicit `-K` and is
   refused in a windowed server, so the desktop build has none.

## ✅ Phases 34–35 — GTK and Firefox

The GTK stack (GLib, Cairo, Pango, GTK3) was cross-built on the same
libX11/libxcb (`ports/gtk/`, `make smoke-gtk`), and maeroX grew the RENDER
extension Cairo composites with. Firefox itself is the official 115.15.0esr
linux-i686 tarball on a glibc GTK3 runtime from Debian i386 packages
(`ports/firefox/README.md`). The kernel work that took it from a stall to a
first paint is in `docs/audit/firefox-first-paint.md`, the startup profile
that brought first paint down to about 5 s is in `docs/perf/firefox-startup.md`.

## How to test: does Firefox paint, and load a page?

`make smoke-firefox` answers "does Firefox paint?". It
boots the GRUB ISO (the only boot path with a framebuffer) plus
`disk-ff.img` headless in QEMU, lets the desktop launch `ff`, and judges
PASS/FAIL from the serial console. It exits 0 only when `ff` prints
`ff: Firefox painted`, which the launcher does only after maeroX saw the
first PutImage **and** the browser process survived a 5 s grace check (a
crash-reporter dialog painting does not count).

```sh
. ~/opt/cross/maeros-env.sh
make smoke-firefox                                   # KVM if /dev/kvm is writable, else TCG; -smp 1
make smoke-firefox SMOKE_FF_ARGS="--smp 2"           # SMP guest
make smoke-firefox SMOKE_FF_ARGS="--accel tcg --timeout 1200"
python3 tools/smoke_firefox.py --help                # all options
```

What happens in the guest: with a framebuffer and the disk userland,
`/disk/init` runs `/etc/rc`, the services and then the graphical session
as the unprivileged user, so no shell prompt reaches the serial line while
the desktop is up. The desktop auto-launches `/disk/ff` a few seconds after
its window manager starts because the marker file `/disk/ffauto`
(`testfiles/ffauto`, committed) is on the disk; the desktop inherits init's
console, so everything `ff` and Firefox print lands on the serial line.
Remove `testfiles/ffauto` and rebuild the disk to get back a desktop that
waits for a click, but the smoke test then fails with "the ff launcher
never started".

Verdicts and exit codes:

| Serial evidence | Result | Exit |
|---|---|---|
| `ff: Firefox painted — window is up (attempt N)` | PASS | 0 |
| `ff: gave up after N attempts` (6 attempts of 300 s by default, overridable in `/disk/ffcfg/ffwatch`) | FAIL | 1 |
| `=== KERNEL PANIC ===` | FAIL | 1 |
| QEMU dies, the launcher never starts, or the overall timeout (6 min KVM / 15 min TCG) passes | FAIL | 1 |
| Missing ISO/disk/QEMU/`testfiles/ffauto`, or a boot without a framebuffer | ERROR | 2 |

Every run writes `build/ff-smoke/<timestamp>-<accel>-smpN/` (gitignored):

- `serial.log` — the complete serial console, verbatim.
- `screen.png` — the last VGA screendump (`screen.ppm` if PNG is not
  possible); on a PASS, `screen-paint.png` is the richest of ten frames
  sampled over `--hold` seconds (25) after the paint line.
- `summary.txt` — verdict, timeline (graphical session, `ff` start, first
  `firefox-bin` exec, first Firefox X window, first paint), one line per
  concluded attempt (stalled / exited status=N / crash-reporter paint), the
  last maeroX trace line with its `putimg=` counter, every
  `[SIG] pid=N killed by signal S` line, the last 40 kernel trace lines and
  the `moz.log` tail that `ff` dumps on a stall.
- `qemu-cmdline.txt` — the exact QEMU command.
- `screen-typed.png` (`--type`), `screen-web.png` and `fonts.json` (`--web`),
  `site-N-*.png` (`--sites`), described below.

### Loading a page

`make smoke-firefox-web` (`--web`) goes one step further. The guest gets an
rtl8139 on QEMU user networking, and after the paint the harness serves a
page from a thread (HTML, a CSS rule, a 96x96 pure-red PNG), types `http://10.0.2.2:PORT/` into the address bar
through QEMU `sendkey`, and PASSes only when the image is requested **and**
its red block shows up in a screendump **and** the page's text checks pass.
`summary.txt` gets `web` lines with every request and the time from Enter to
it, and `screen-web.png` is the frame the block was found in, taken again
once the text is drawn. On the current tree a load takes 1-3 s. `--pcap`
also records the NIC's traffic to `net.pcap` in the artifacts.

The guest reaches the real internet through slirp, so Firefox talks to
Mozilla's services during the run. App updates are turned off by policy,
`testfiles/etc/firefox/policies/policies.json` (`DisableAppUpdate`; the
`app.update.enabled` pref in `user.js` has been ignored since Firefox 63). Without
it Firefox asks `aus5.mozilla.org` at startup, is offered a newer ESR that it
cannot install (it has no write access to `/disk/firefox`), and opens an "Update
available" panel. The panel takes the keyboard while the URL is typed and keeps
the single CPU busy redrawing. The harness refuses to start without the policy.

The same page carries a font test: one row per family (`sans-serif`,
`serif`, `monospace`, `system-ui`, example.com's `-apple-system ... Arial,
sans-serif` stack, `Times New Roman`, `Courier New`), each with Latin,
`äöüçşğ ÄÖÜÇŞĞİı`, Greek, Cyrillic and bold / italic / bold italic. A script
in the page measures every character on a canvas and compares its advance
and ink box with those of an unassigned code point, i.e. Firefox's
missing-glyph hex box in that family; a match is a box. It also checks that
serif, sans and monospace are different faces, that monospace is fixed
width, that bold and italic change the rendering and that serif italic is a
real italic face (its `f` has a descender), plus a negative control: U+A000,
which no installed font has, must be seen as a box. The page POSTs the result
to the harness (`fonts.json` in the artifacts, `web ... fonts:` lines in
`summary.txt`) and shows it as its title (`FONTS OK` / `FONTS FAIL n`).

The fonts are DejaVu Sans, Serif and Sans Mono in four styles each, from the
pinned `fonts-dejavu-*` 2.37-8 packages (`ports/firefox/fetch-runtime.sh`
puts them in `/disk/firefox/share/fonts/dejavu`, 5.3 MB), and
`testfiles/etc/fonts/fonts.conf` maps the CSS generics, `system-ui` and the
usual named families (Arial, Helvetica, Segoe UI, Times New Roman, Courier
New, ...) onto them. They cover Latin, Greek and Cyrillic; CJK, Arabic
shaping beyond DejaVu's, Indic scripts etc. still come out as boxes (colour
emoji come from Firefox's own Twemoji).

`--net` alone attaches the NIC, so any address can be tried by hand, the
real network included (QEMU's slirp routes it):

```sh
python3 tools/smoke_firefox.py --net --type $'https://example.com/\n' --type-settle 60
```

### Real websites (manual only)

`--sites` (implies `--net`) drives Firefox through real sites over the live
internet. It is **never** part of a default smoke target, since what a live
site serves changes from day to day, and it reports what happened instead of
judging it. Keep it to a few loads per site.

```sh
python3 tools/smoke_firefox.py --sites default          # Wikipedia, gnu.org, HN, DDG html, GitHub
python3 tools/smoke_firefox.py --sites "https://www.gnu.org/|Philosophy,https://example.com/"
```

For each `URL[|link text]` it types the URL, presses Enter (and presses it
again, counted as `Enter xN`, if the screen does not move at all), then waits
for the frame to **settle**: three identical screendumps in a row, so a
spinning tab throbber still counts as loading. Then it presses PageDown twice
and, when a link text is given, follows that link with Firefox's quick find
for links (`'` + text + Enter), timing it the same way. `summary.txt` gets one
`site` line per URL: time from Enter to a settled frame, how much of the screen
PageDown moved, the link's settle time, and any `[SIG]` kill, `ff` restart,
`[SYSCALL] unimplemented` (ENOSYS), `[OOM]` or panic line printed during that site. The frames are saved as
`site-N-load.png`, `site-N-scroll.png` and `site-N-click.png`.
`--site-timeout` (150 s) bounds each wait.

`/disk/ffcfg/ffmozlogfile` (one line, e.g. `/disk/ffout/moz.log`) moves
Firefox's MOZ_LOG onto the ext2 disk, where it survives the run; read it
back from the image with `debugfs -R "dump ffout/moz.log.moz_log out.log"`.
`/disk/ffcfg/ffmozlog` still picks the modules.

`make run-firefox` boots the same ISO + disk with a window (and KVM when
available) for watching by hand. `make disk-ff` fetches the Firefox runtime
via `ports/firefox/fetch-runtime.sh` when `testfiles/firefox/firefox-bin`
is missing. Note that `disk-ff` rebuilds the 1 GiB image on every
invocation (a few minutes).

## Practical notes

- The Firefox runtime tree (`testfiles/firefox/`, gitignored) and the glibc
  in `testfiles/lib/` are rebuilt from public sources by
  `sh ports/firefox/fetch-runtime.sh` (official Firefox 115 ESR tarball plus
  Debian i386 packages, no Docker/root); `ports/firefox/check-runtime.sh`
  proves the tree is closed under dynamic linking.  See `ports/firefox/README.md`.
- Cross builds: `ports/Dockerfile.cross` (linux/amd64 + musl.cc i686).
  It copies in `ports/i686-linux-musl-cross.tgz`, which is gitignored and
  downloaded there by `tools/setup-linux.sh`.
- Most ports build `-static -no-pie`; **as of Phase 30 the kernel also runs
  PT_INTERP (dynamic) binaries** via musl ld.so (`ports/build-dynprobe.sh`
  builds the `-fpie -pie` probe and exports the linker).
- The ABI surface real binaries exercise is documented by what busybox
  and DOOM needed (Phase 19).
