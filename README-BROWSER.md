# MaeroOS Browser Roadmap — from `browse` to Firefox-class

## Where we are

| Rung | Browser | Status |
|------|---------|--------|
| 1 | `browse` (native) | ✅ Done — instant text browsing, links, history, redirects |
| 2 | **links2 -g** (Linux port) | Phase 21 — real pages **with images** (PNG/JPEG/GIF), tables, frames |
| 3 | NetSurf framebuffer | Next — true CSS layout engine (libcss), the real-web milestone |
| 4 | **Dynamic linking (ld.so)** | ✅ **Phase 30 + 31. PIEs run via the real `/lib/ld-musl-i386.so.1`, and EXTERNAL shared libraries now load (zlib runs).** Foundation for everything beyond. |
| 5 | X11/Wayland compatibility layer | Next — GUI toolkit apps (GTK) become possible |
| 6 | Firefox-class (Gecko/WebKit) | The summit |

## ✅ Phase 31 — external shared libraries now load (rung 2 cleared)

Per the user's **Firefox-only** decision, every rung from here is direct Firefox
infrastructure, verified by a technical proof rather than a stand-in browser.

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
- **RAM**: default `make run` bumped 128M → **512M**. Full 1–2 GB (needed by
  GTK/Firefox) requires PMM/VMM work for >512 MB and is a Phase 32 item.

## ✅ Phase 32a — real pthreads (rung 3 cleared)

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

## ⏳ The honest remaining ladder to Firefox

The kernel now has the hard low-level primitives Firefox needs — **dynamic
linking, external shared libraries, threads, futexes, shared memory, mmap**.
What remains is overwhelmingly a **multi-session userspace porting marathon**,
not kernel work, and Firefox does **not** run yet:

1. ~~**AF_UNIX sockets**~~ ✅ **DONE (Phase 32b).** `proc/usocket.c` adds local
   stream sockets — `socketpair`, named `bind`/`listen`/`connect`/`accept`, and
   bidirectional streaming, integrated into the FD layer (`FD_USOCKET`) and
   `poll`/`select`. Proven: `usockprobe` → `UNIX_SOCK_OK`. This is the transport
   X11/Wayland/D-Bus connect over.
2. **An X11 server subset** — ⏳ **STARTED (Phase 33a).** `userspace/maerox/` is a
   native libgui app that owns a desktop window, listens on AF_UNIX
   `/tmp/.X11-unix/X0`, and completes the **X11 connection handshake** (byte-exact
   setup reply: 1 screen, a 24-bit TrueColor visual, standard pixmap formats).
   Proven: `make smoke-x` → `xprobe` → `XHANDSHAKE_OK`.
   **33b done too:** the request loop (CreateWindow, CreateGC, MapWindow,
   PolyFillRectangle, PutImage, GetGeometry, + the startup queries) renders
   client windows into the desktop surface — a real X client (`xdraw`) draws a
   window with a rectangle + gradient on the MaeroOS desktop (`XDRAW_OK`).
   **33c done too:** maeroX is now a **fully interactive X server** — it sends
   `Expose`/`ConfigureNotify` on map and forwards clicks to clients as
   `ButtonPress`/`ButtonRelease` (`xevent` → `XEVENT_OK`; clicking an X window
   draws marks at the exact coordinates).
   **33d done — the big one:** the **real libX11/libxcb** stack is cross-built for
   musl/i686 (`ports/x11/build-x11.sh`), and a **genuine Xlib application**
   (`xreal`, linked against the actual libX11 GTK/Firefox use) connects via
   `XOpenDisplay`, creates a window, and paints with `XFillRectangle` on the
   MaeroOS desktop (`XREAL_PAINTED`). The unlock was kernel `sendmsg`/`recvmsg`
   on AF_UNIX (libxcb's transport). The X client library is now **real**.
   **Keyboard done:** keys travel PS/2 → `drivers/keyboard.c` → the desktop →
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

## Next: the GTK stack (Phase 34)

GTK + Cairo + Pango + GLib + GdkPixbuf + FreeType + fontconfig + HarfBuzz, built
on this same libX11/libxcb via the `build-x11.sh` pattern. maeroX will need many
more X requests (text/fonts, CopyArea, the RENDER extension). Then Firefox
(Phase 35) — which does not run yet, but the X foundation it stands on is now
real and proven.
3. **The GTK stack** — GTK + Cairo + Pango + GLib + GdkPixbuf + FreeType +
   fontconfig + HarfBuzz, built in the maeros-cross container and loaded via the
   Phase-31 loader.
4. **Firefox ESR (i686)** — Gecko + SpiderMonkey JIT, content processes,
   NSS/NSPR; a roughly month-scale build needing 1–2 GB RAM (and >512 MB RAM
   still hangs at boot — a PMM/VMM fix that gates this step).

Each of these is its own phase, built and tested incrementally.  The foundation
is now in place; the climb continues build-by-build.

## ✅ Phase 30 — the dynamic linker now works (first rung cleared)

As of Phase 30, MaeroOS loads **dynamically-linked** ELF binaries, not just
static ones. The `dynprobe` PIE (built `i686-linux-musl-gcc -fpie -pie`)
boots through the genuine musl dynamic linker and prints `DYNPROBE_OK`
(`make smoke-dyn`). Mechanism:

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

**Known limits (next on this rung):** `mmap2`'s file offset is hard-wired to 0
(EBP not captured at the int-0x80 boundary) — fine for `dynprobe` (its only
NEEDED lib is the ld.so itself, which musl reuses in place) but it must be
fixed before loading real multi-`.so` programs. No W^X yet (segments map
writable). Single TLS slot. These are the gating items before rung 5.

## Why Firefox can't be next

Firefox needs, hard-blocking, in order:
1. **Dynamic linker** — Firefox is ~100 shared libraries; our loader is static-only.
   This alone is a kernel + libc project (ld.so, ELF relocations, symbol
   interposition, dlopen).
2. **A windowing protocol** — GTK speaks X11/Wayland. We'd implement an X
   server subset or a Wayland compositor bridge to our WM protocol.
3. **JIT-grade memory APIs** — precise mprotect transitions, W^X toggling,
   guard pages, `membarrier`; SpiderMonkey stresses mmap semantics hard.
4. **Threads at scale** — content processes, thread pools, robust futexes
   (PI, requeue), TLS beyond one GDT slot.
5. **~2 GB RAM** and a 64-bit-friendly world — modern Firefox dropped i686
   support; we'd target an *old ESR* (52/68) even then.

Realistic effort: a SerenityOS-scale, year-class undertaking. SerenityOS
took ~4 years to first-paint a modern web engine — with a team.

## What each rung buys us

- **links2 (Phase 21)**: real web with images today. Self-contained C,
  builds static, has its own HTTP/HTTPS-(via openssl, skip)/decoders.
  Needs: /dev/fb0 mmap (done), FBIO ioctls (done), select() (done),
  termios raw (done).
- **NetSurf-fb**: real CSS rendering — pages actually look right. Static
  build possible (it runs on RISC OS/Amiga/Haiku). Bigger dependency tree
  (libcss, libdom, libnsgif/bmp, libparserutils) — a weekend-class port
  on top of our musl cross container.
- **ld.so**: unlocks prebuilt Linux software in general — the single
  highest-leverage OS feature after this plan.
- **X layer**: Xvfb-style in-process server speaking our shm surfaces;
  then GTK2-era apps (Dillo!, gFTP, Leafpad) run.

## How to test: does Firefox paint yet?

`make smoke-firefox` is the one number that matters for the summit. It
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
| `ff: gave up after 20 attempts` | FAIL | 1 |
| `=== KERNEL PANIC ===` | FAIL | 1 |
| QEMU dies, the launcher never starts, or the overall timeout (6 min KVM / 15 min TCG) passes | FAIL | 1 |
| Missing ISO/disk/QEMU/`testfiles/ffauto`, or a boot without a framebuffer | ERROR | 2 |

Every run writes `build/ff-smoke/<timestamp>-<accel>-smpN/` (gitignored):

- `serial.log` — the complete serial console, verbatim.
- `screen.png` — the last VGA screendump (`screen.ppm` if PNG is not
  possible); `screen-paint.png` right after the paint line on a PASS.
- `summary.txt` — verdict, timeline (graphical session, `ff` start, first
  `firefox-bin` exec, first Firefox X window, first paint), one line per
  concluded attempt (stalled / exited status=N / crash-reporter paint), the
  last maeroX trace line with its `putimg=` counter, every
  `[SIG] pid=N killed by signal S` line, the last 40 kernel trace lines and
  the `moz.log` tail that `ff` dumps on a stall.
- `qemu-cmdline.txt` — the exact QEMU command.

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
  The toolchain tarball is vendored at `ports/i686-linux-musl-cross.tgz`.
- Most ports build `-static -no-pie`; **as of Phase 30 the kernel also runs
  PT_INTERP (dynamic) binaries** via musl ld.so (`ports/build-dynprobe.sh`
  builds the `-fpie -pie` probe and exports the linker).
- The ABI surface real binaries exercise is documented by what busybox
  and DOOM needed (Phase 19).
