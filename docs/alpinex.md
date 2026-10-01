# Alpine X11 applications on maeroX

MaeroOS runs unmodified Alpine Linux x86 X11 programs (xterm, xeyes, xclock,
GTK 3 apps such as Mousepad and Galculator) as windows of maeroX, its X
server, and installs them from the desktop.

    make disk-alpinex      # disk-alpinex.img: Alpine root + offline X repo + /disk/xapp
    make smoke-alpinex     # opt-in: install, run, type into and close xterm, xeyes, mousepad

Boot the ISO with `disk-alpinex.img` as the disk.  The Start menu then has
**Linux Apps**, which lists the curated applications with Install/Remove and
Open buttons.  Installed apps also get their own Start-menu entry.

## Pieces

| Piece | Where | What it does |
|---|---|---|
| maeroX | `userspace/maerox/` (initrd `/maerox`) | the X server, one desktop window whose body is the X screen |
| `xapp` | `userspace/xapp/xapp.c`, set-uid root at `/disk/xapp` | `list`, `install`, `remove`, `run`, `<slot> <name>` |
| Linux Apps | `userspace/xapp/linuxapps.c` (initrd `/linuxapps`) | the front end; runs `xapp install/remove`, launches entries |
| curated list | `userspace/xapp/xapps.h` | name, packages, command of each app |
| image | `ports/alpine/prepare.py` with `ALPINE_X=1` | adds `apk fetch -R` of the apps to `/repo/main` and `/repo/community`, pinned in `ports/alpine/alpine-x.lock` |

`xapp install <name>` runs `apk add` for the app's packages inside
`chroot /disk/alpine` (apk verifies the repo's signatures) and writes
`/disk/apps/<name>/manifest` with `exec=/disk/xapp args=<name>`, then tells
the desktop `apps-changed`.  Launching the entry runs `/disk/xapp <slot> <name>`:
if no X server answers, that process drops to the caller's uid and becomes
maeroX in the slot, and a child enters the chroot, drops privileges for good
and executes the app with `DISPLAY=:0`.  If maeroX already runs, the app
just joins it.  The app's output goes to `/disk/alpine/tmp/xapp-<name>.log`.

Privilege: `chroot` and `apk` need root, so `xapp` is set-uid (the initrd
cannot carry set-uid files, so it lives on the image).  It accepts only the
names in `xapps.h` — no package names, paths or commands from the caller — and
apps always run as the caller.

The chroot's `/tmp` is its own, so the filesystem socket
`/tmp/.X11-unix/X0` is invisible inside it.  maeroX also listens on the
abstract socket `@/tmp/.X11-unix/X0`, which libxcb tries first; abstract
names are not part of any filesystem, so they cross the chroot.

## maeroX

maeroX was a Firefox-only server (absolute window coordinates, every event
sent whether selected or not, empty properties, no core text).  It now
implements the core protocol the way X11R7.7 specifies it, minus the parts
noted below:

- **windows**: a real tree (relative coordinates, borders, stacking,
  Configure/Reparent/Circulate, Map/Unmap/MapSubwindows, Destroy with
  DestroyNotify for the subtree), backgrounds (pixel, pixmap tile,
  ParentRelative), every window with its own backing buffer, composited
  bottom-up — so Expose is only needed on map and resize;
- **events**: per-client event masks, propagation and do-not-propagate,
  implicit and active pointer grabs (owner-events), keyboard grabs,
  Enter/Leave with the Ancestor/Inferior/Nonlinear/Virtual details,
  FocusIn/FocusOut, PropertyNotify, Selection*, ClientMessage, SendEvent,
  NoExpose after CopyArea/CopyPlane;
- **properties, atoms, selections**: all requests, predefined atoms 1-68;
- **drawing**: every core graphics request through one span rasteriser with
  the GC's function, plane mask, fill style (solid, tiled, stippled,
  opaque-stippled), clip rectangles or clip mask; wide lines, arcs, polygons
  (even-odd and winding); PutImage/GetImage in bitmap/XY/Z formats at depth
  1, 8, 24 and 32 (GetImage of a window includes its children);
- **fonts**: one built-in 8x16 face under XLFD names (medium, bold,
  iso8859-1, iso10646-1, "fixed", "8x16", "cursor"); OpenFont gives it for
  any name; QueryFont, ListFonts(WithInfo), QueryTextExtents, Poly/ImageText;
- **colours**: TrueColor; AllocColor, AllocNamedColor/LookupColor with the
  X.Org rgb.txt names, QueryColors;
- **RENDER 0.11**: formats a8r8g8b8, x8r8g8b8, a8, a1; Composite with every
  Porter-Duff operator, masks (and component alpha), transforms with nearest
  or bilinear filtering, repeat none/normal/pad/reflect, picture clips;
  solid, linear, radial and conical gradients; anti-aliased Trapezoids,
  Triangles, TriStrip, TriFan and AddTraps; glyph sets in A1/A8/ARGB32;
- **window manager** (in the server): toplevels get a frame with a title
  bar and a close box (WM_DELETE_WINDOW, else KillClient), are placed in a
  cascade or centred on their WM_TRANSIENT_FOR, can be dragged by the title,
  raise and take the focus on a click (WM_TAKE_FOCUS honoured);
  _NET_ACTIVE_WINDOW, _NET_CLOSE_WINDOW and _NET_WM_STATE maximise requests
  sent to the root are acted on.  `-k` (kiosk, used by Firefox's `ff`
  launcher) keeps the old behaviour: a large toplevel fills the screen,
  no frames.

Input: the desktop sends maeroX the raw pointer stream it asks for with
`rawptr` (every motion, all three buttons, leaving the window; libgui
`gui_set_ptr_handler`), the uncooked key stream, and wheel notches (buttons
4/5).  Unknown requests get BadRequest and are logged once to
`/tmp/maerox.log`, as are the extensions clients query.

Not implemented (clients fall back): XKEYBOARD, XInputExtension, RANDR,
SHAPE, MIT-SHM, BIG-REQUESTS, XFIXES, DAMAGE, Composite, SYNC, GLX.  Window
gravity on parent resize, dashed lines and cursor images are ignored (the
desktop draws its own pointer).

## Applications

See `.yonet/report.md` of the alpinex branch for the per-application
status measured with `make smoke-alpinex`.
