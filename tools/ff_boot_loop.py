#!/usr/bin/env python3
"""ff_boot_loop: boot the Firefox smoke test over and over and count the wedges.

A one-in-twenty hang cannot be studied one boot at a time.  This runs
tools/smoke_firefox.py back to back — strictly one QEMU at a time, because the
host does not have the memory for two — and keeps a per-run record of the
verdict, the first-paint time and where the artifacts landed, so a campaign
produces a rate with a count behind it instead of an anecdote.

Failing runs keep their whole artifact directory (serial log, screendumps and
the QEMU-monitor wedge capture).  Passing runs keep their summary and serial
log; their screendumps are deleted, which is what makes a sixty-boot campaign
fit on disk.

Usage:
  python3 tools/ff_boot_loop.py -n 60 [--stop-after-fails N] [--keep-pass]
                                [-- <args passed to smoke_firefox.py>]

Results go to build/ff-loop/<timestamp>/{loop.csv,loop.txt}; the tally is also
printed after every run so a long campaign can be watched from the terminal.
"""
import argparse
import datetime
import os
import re
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ART = re.compile(r"artifacts -> (\S+)")
PAINT = re.compile(r"first paint \(ff verdict\)\s*:\s*([0-9.]+)s")
REASON = re.compile(r"^reason\s*:\s*(.*)$", re.M)
RESULT = re.compile(r"^result\s*:\s*(\S+)$", re.M)


def prune_pass(outdir):
    """Drop a passing run's images; the serial log and summary stay."""
    for name in os.listdir(outdir):
        if name.endswith((".png", ".ppm")):
            try:
                os.unlink(os.path.join(outdir, name))
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--runs", type=int, default=20)
    ap.add_argument("--stop-after-fails", type=int, default=0,
                    help="stop once this many runs have failed (0 = never)")
    ap.add_argument("--keep-pass", action="store_true",
                    help="keep the screendumps of passing runs too")
    ap.add_argument("--out", default=os.path.join("build", "ff-loop"))
    ap.add_argument("rest", nargs=argparse.REMAINDER,
                    help="arguments after -- go to smoke_firefox.py")
    args = ap.parse_args()
    extra = [a for a in args.rest if a != "--"]

    os.chdir(ROOT)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    outdir = os.path.abspath(os.path.join(args.out, stamp))
    os.makedirs(outdir, exist_ok=True)
    csv = open(os.path.join(outdir, "loop.csv"), "w", buffering=1)
    csv.write("run,result,paint_s,elapsed_s,artifacts,reason\n")
    log = open(os.path.join(outdir, "loop.txt"), "w", buffering=1)

    def say(line):
        print(line)
        sys.stdout.flush()
        log.write(line + "\n")

    say("ff_boot_loop: %d runs, extra args %s" % (args.runs, extra or "(none)"))
    say("ff_boot_loop: results -> %s" % outdir)

    npass = nfail = 0
    paints = []
    fails = []
    t_start = time.time()
    for i in range(1, args.runs + 1):
        t0 = time.time()
        p = subprocess.run([sys.executable, "tools/smoke_firefox.py"] + extra,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = p.stdout.decode("utf-8", "replace")
        elapsed = time.time() - t0
        m = ART.search(out)
        art = m.group(1) if m else "?"
        m = RESULT.search(out)
        result = m.group(1) if m else ("PASS" if p.returncode == 0 else "FAIL")
        m = REASON.search(out)
        reason = m.group(1).strip() if m else ""
        m = PAINT.search(out)
        paint = float(m.group(1)) if m else None

        if result == "PASS":
            npass += 1
            if paint is not None:
                paints.append(paint)
            if not args.keep_pass and os.path.isdir(art):
                prune_pass(art)
        else:
            nfail += 1
            fails.append((i, art, reason))
            # Keep the harness's own stdout next to the artifacts: it holds the
            # echoed serial lines in the order the harness saw them.
            if os.path.isdir(art):
                with open(os.path.join(art, "harness-stdout.txt"), "w") as f:
                    f.write(out)

        csv.write("%d,%s,%s,%.0f,%s,%s\n" % (
            i, result, ("%.1f" % paint) if paint is not None else "",
            elapsed, art, reason.replace(",", ";")))
        mean = (sum(paints) / len(paints)) if paints else float("nan")
        say("[%2d/%d] %-4s  paint=%-6s elapsed=%3.0fs   pass=%d fail=%d "
            "(%.1f%% wedge)  paint mean=%.1fs  %s"
            % (i, args.runs, result,
               ("%.1fs" % paint) if paint is not None else "-",
               elapsed, npass, nfail, 100.0 * nfail / i, mean, reason[:60]))

        if args.stop_after_fails and nfail >= args.stop_after_fails:
            say("ff_boot_loop: stopping after %d failure(s)" % nfail)
            break

    say("")
    say("ff_boot_loop: %d run(s) in %.0f min: %d pass, %d fail (%.1f%%)"
        % (npass + nfail, (time.time() - t_start) / 60.0, npass, nfail,
           100.0 * nfail / max(1, npass + nfail)))
    if paints:
        paints_sorted = sorted(paints)
        say("ff_boot_loop: first paint mean=%.1fs min=%.1fs max=%.1fs median=%.1fs (n=%d)"
            % (sum(paints) / len(paints), paints_sorted[0], paints_sorted[-1],
               paints_sorted[len(paints_sorted) // 2], len(paints)))
    for i, art, reason in fails:
        say("ff_boot_loop: FAIL run %d -> %s  (%s)" % (i, art, reason))
    csv.close()
    log.close()
    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
