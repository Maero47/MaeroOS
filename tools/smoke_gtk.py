#!/usr/bin/env python3
"""smoke-gtk — verify the GTK stack as it is built up, bottom-first.

Phase 34a: GLib (the foundation of GTK/Pango/GdkPixbuf) runs on MaeroOS.
glibprobe exercises GHashTable/GString/GList/g_ascii_strup → GLIB_OK.
This suite grows a layer at a time (Cairo, Pango, … then GTK).
"""
import os
import selectors
import subprocess
import sys
import time


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "


def wait_for(proc, sel, needle, log, timeout=30, start=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for key, _ in sel.select(0.2):
            chunk = os.read(key.fd, 4096).decode("latin1", "replace")
            if not chunk:
                continue
            log.append(chunk)
            sys.stdout.write(chunk)
            sys.stdout.flush()
            if needle in "".join(log)[start:]:
                return
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {proc.returncode}")
    raise TimeoutError(f"timed out waiting for {needle!r}")


def send(proc, text):
    proc.stdin.write(text.encode("latin1"))
    proc.stdin.flush()


# Probes this suite runs, the port script(s) whose static libraries each one is
# linked against (the source is ports/gtk/<name>.c).  glibprobe is committed; the other three are
# gitignored (tens of MB each) and no script in the tree links them — they are
# built by hand inside the maeros-gtkbuild container (ports/gtk/Dockerfile.gtk*)
# against /sysroot after the build-*.sh scripts below have filled it.
PROBES = [
    ("glibprobe", "ports/gtk/build-glib.sh"),
    ("cairoprobe", "ports/gtk/build-glib.sh + build-cairo.sh"),
    ("pangoprobe", "ports/gtk/build-glib.sh + build-cairo.sh + build-pango.sh"),
    ("gtkprobe", "ports/gtk/build-glib.sh + build-cairo.sh + build-pango.sh + build-gtk.sh"),
]


def check_probes():
    missing = [(name, deps) for name, deps in PROBES
               if not os.path.isfile(os.path.join(ROOT, "testfiles", name))]
    if not missing:
        return
    lines = ["missing GTK-stack probe binaries (not in git; nothing was run):"]
    for name, deps in missing:
        lines.append(f"  testfiles/{name}: link ports/gtk/{name}.c against the "
                     f"libraries {deps} installs into /sysroot")
    lines.append("  build them inside the maeros-gtkbuild container "
                 "(ports/gtk/Dockerfile.gtk*), copy them into testfiles/, "
                 "then rerun make smoke-gtk")
    raise FileNotFoundError("\n".join(lines))


def main():
    check_probes()
    # The GTK-stack probes are large and live on the ext2 DISK (not the initrd,
    # which sits in RAM).  Boot with the disk attached and run them from /disk.
    subprocess.run(["make", "initrd", "disk"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc = subprocess.Popen(
        ["qemu-system-i386", "-kernel", "kernel.elf", "-initrd", "initrd.tar",
         "-drive", "file=disk.img,format=raw,if=ide", "-serial", "stdio",
         "-m", "512M", "-no-reboot", "-no-shutdown"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0,
    )
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []
    try:
        wait_for(proc, sel, PROMPT, log, timeout=75)

        before = len("".join(log))
        send(proc, "/disk/glibprobe\n")
        wait_for(proc, sel, PROMPT, log, timeout=30, start=before)
        body = "".join(log)[before:]
        if "GLIB_OK" not in body:
            raise AssertionError("GLib (glibprobe) did not run")
        if "v2.78" not in body:
            raise AssertionError("GLib did not report its version")

        # Phase 34b: Cairo (2D drawing, what GTK paints with) + pixman/FreeType.
        before = len("".join(log))
        send(proc, "/disk/cairoprobe\n")
        wait_for(proc, sel, PROMPT, log, timeout=30, start=before)
        body = "".join(log)[before:]
        if "CAIRO_OK" not in body:
            raise AssertionError("Cairo (cairoprobe) did not run")
        if "rect_px=0xe69919" not in body:
            raise AssertionError("Cairo did not render the expected pixel")

        # Phase 34c: Pango (text layout via HarfBuzz + FreeType + fontconfig).
        before = len("".join(log))
        send(proc, "/disk/pangoprobe\n")
        wait_for(proc, sel, PROMPT, log, timeout=120, start=before)
        body = "".join(log)[before:]
        if "PANGO_OK" not in body:
            raise AssertionError("Pango (pangoprobe) did not lay out text")

        # Phase 34d: GTK3 itself — init, build a real window+widgets, connect to
        # maeroX over X11, run a full frame-clock draw cycle (Cairo → X11), and
        # composite the result onto the desktop.
        before = len("".join(log))
        send(proc, "export GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX,-AVX2,-AVX_Fast_Unaligned_Load,-SSE2,-SSSE3,-SSE4_1,-SSE4_2,-Fast_Rep_String,-ERMS\n")
        send(proc, "/disk/gtkprobe\n")
        wait_for(proc, sel, "GTK_PUMPED", log, timeout=150, start=before)
        body = "".join(log)[before:]
        if "GTK_OK init" not in body:
            raise AssertionError("GTK3 did not initialize")
        if "GTK_WINDOW_SHOWN" not in body:
            raise AssertionError("GTK3 did not create+show a window")
        if "GTK_DRAWN" not in body:
            raise AssertionError("GTK3 frame-clock draw cycle did not run")
        wait_for(proc, sel, PROMPT, log, timeout=20, start=before)

        print("\n[SMOKE-GTK] passed")
        return 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[SMOKE-GTK] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
