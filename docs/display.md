# Display modes

The desktop draws into one 32 bpp framebuffer, `/dev/fb0`.  At boot that is the
mode the boot loader set (GRUB `gfxpayload`, Limine, the UEFI GOP).  On three
kinds of display hardware a kernel driver takes the framebuffer over and the
mode can then change at run time:

| Hardware | PCI ID | Driver | How |
|---|---|---|---|
| Bochs / QEMU `-vga std` | 1234:1111 | `drivers/bochs_vga.c` | VBE DISPI registers (ports 0x1CE/0x1CF), linear framebuffer at BAR0 |
| VirtualBox VGA | 80ee:beef | `drivers/bochs_vga.c` | the same DISPI ports |
| virtio-gpu (`-device virtio-gpu-pci`, `virtio-vga`) | 1af4:1050 | `drivers/virtio_gpu.c` | virtio 1.x over PCI, control queue, 2D resources |

Anything else (a real PC's GOP or VBE framebuffer, QEMU `ramfb`, Cirrus) keeps
the boot loader's mode: the mode list holds that one mode and Settings says it
is fixed.  `nomodeset` on the kernel command line keeps both drivers out.

## Kernel

`drivers/framebuffer.c` owns `/dev/fb0` and the framebuffer window
(`FB_WINDOW_START`..`FB_WINDOW_END`, 256 MiB at 0xE0000000, see
`include/kernel/config.h`).  A driver calls `framebuffer_attach()` during boot,
after `pci_init` and before devfs and the first process, with all the memory any
of its modes can use; that memory is mapped at the window once, so no page
table of the window changes later and a mode switch only reprograms the device:

- **Bochs/VirtualBox:** all of BAR0's VRAM (16 MiB on QEMU by default),
  uncached.  The driver takes over only if the boot loader's framebuffer is at
  BAR0 (so `make run`'s text-mode `-kernel` boot stays text).  Modes: the common
  sizes (640x480 ... 2560x1600, widths a multiple of 8, which DISPI requires)
  that fit in VRAM and under the card's GETCAPS maximum, plus the boot mode.
  The scanout reads VRAM, nothing to flush.
- **virtio-gpu:** 9 MiB of guest RAM frames (1920x1200 at 32 bpp), cached, each
  holding a reference of the driver's.  A mode is a host resource of that size
  (`RESOURCE_CREATE_2D`, B8G8R8X8) backed by the first w*h*4 bytes of the frames
  (`RESOURCE_ATTACH_BACKING`, adjacent frames merged) and set on scanout 0
  (`SET_SCANOUT`); the old resource is detached and unreferenced.  The queue is
  polled (INTx and MSI-X off, `VIRTQ_AVAIL_F_NO_INTERRUPT`).  It starts in the
  display's preferred size (`GET_DISPLAY_INFO`; QEMU's default is 1280x800).
  What is written reaches the screen when flushed (`TRANSFER_TO_HOST_2D` +
  `RESOURCE_FLUSH`, submitted together).  A virtio-vga's VGA side carries the
  boot loader's framebuffer until the driver replaces it; a virtio-gpu-pci with
  no boot framebuffer (SeaBIOS has no VBE for it) gains a desktop it did not
  have.  `VIRTIO_GPU_EVENT_DISPLAY` (the QEMU window resized) refreshes the
  preferred mode in the list.

`fbflushd` (a kernel thread, 25 Hz, only with a flush driver) flushes for the
clients that do not: the scanlines written with `write(2)` since the last
flush, or the whole screen while a client has `/dev/fb0` mapped (fbDOOM).  An
explicit flush from the desktop ends the mapped mode.

### ioctls on /dev/fb0

| Request | Argument | |
|---|---|---|
| `FBIOGET_VSCREENINFO` 0x4600 | `struct fb_var_screeninfo` (out) | Linux layout |
| `FBIOPUT_VSCREENINFO` 0x4601 | `struct fb_var_screeninfo` (in/out) | `xres`x`yres` at 32 bpp from the mode list, no panning (virtual = visible); `activate & FB_ACTIVATE_TEST` only checks; the current mode is always accepted; the resulting var is written back |
| `FBIOGET_FSCREENINFO` 0x4602 | `struct fb_fix_screeninfo` (out) | `line_length` changes with the mode |
| `FBIO_MAEROS_MODES` 0x46E0 | `struct fb_modelist` (out) | the modes, the current one, `FB_MODES_SETTABLE`/`FB_MODES_FLUSH`, driver name, the display's preferred size, a generation that changes with the list |
| `FBIO_MAEROS_FLUSH` 0x46E1 | `struct fb_flush` (in) | up to 16 changed rectangles; a no-op unless the list says `FB_MODES_FLUSH` |

Linux lists modes in sysfs and has no fbdev flush (DRM's `DIRTYFB` is the
closest), hence the two MaeroOS requests; the layouts are in
`drivers/framebuffer.h` and `userspace/include/sys/ioctl.h`.  `mmap` of
`/dev/fb0` maps the framebuffer page by page (a virtio-gpu framebuffer is not
contiguous).

## Desktop and Settings

Settings -> Display lists the modes (up to the desktop's 1920-pixel width) and
sends the desktop `setmode W H` on Apply.  The desktop switches, re-allocates
its back buffer, shadow and damage spans, rescales the wallpaper and its blur,
and re-lays the windows out: maximized and snapped windows take the new full or
half screen, the rest are kept inside it, clients whose window changed get a
`geom` event (maeroX resizes its root window and sends the root a
ConfigureNotify).  Settings then shows "Keep this resolution?" with a 15 s
countdown: Keep sends `modekeep` and writes `mode=WxH` to
`/disk/etc/desktop.conf`; Revert sends `moderevert`; if nothing comes, the
desktop goes back by itself (it owns the timer, so a crashed or closed Settings
cannot leave a broken mode up).  At login the desktop applies `mode=` before it
allocates anything.

On a flush display the desktop's `present()` turns the spans it wrote into
rectangles (rows with the same span merge; past 16 the last one grows) and
flushes them with one ioctl per frame, so a pointer move flushes a few hundred
pixels, not the screen.  `echo stats > /tmp/wmctl` prints `flushes=`,
`flush_rects=` and `flush_kpx=` with the other compositor counters.

## Test

`make smoke-gfxmode` (in `make check`, ~5 min with KVM): see
`tools/smoke_gfxmode.py`.

## References

- Bochs `vbe.h` and QEMU `hw/display/vga.c`, `hw/display/bochs-display.c`
  (DISPI register semantics; reference only), OSDev wiki "Bochs VBE Extensions".
- Virtual I/O Device (VIRTIO) Version 1.2, OASIS: 2.7 split virtqueues, 4.1
  virtio over PCI, 5.7 GPU device.  QEMU `hw/display/virtio-gpu.c` for what
  the host does with each command (reference only).
- Linux `include/uapi/linux/fb.h` for the fbdev structures and request numbers.
