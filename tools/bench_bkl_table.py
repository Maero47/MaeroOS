#!/usr/bin/env python3
"""Median table from build/bench-bkl/results.txt (tools/bench_bkl.py runs).

  python3 tools/bench_bkl_table.py RESULTS [RESULTS...] -- TAGPREFIX:SMP ...

Each column is every run whose tag starts with TAGPREFIX at that -smp; the
cell is the median and, in brackets, the individual runs.  "idle spin" is the
largest per-CPU spin of the CPUs that held the lock <5% in a single-process
workload (stat x1): what an idle CPU pays for the lock.
"""
import re
import statistics
import sys

ROWS = [("time forks (ms)", "forks", "time"), ("time forks4 (ms)", "forks4", "time"),
        ("time targz4 (ms)", "targz4", "time"), ("time par4 (ms)", "par4", "time"),
        ("getpid x1 (k ops/s)", "scale-getpid-1", "ops"), ("getpid x4", "scale-getpid-4", "ops"),
        ("pipe x1", "scale-pipe-1", "ops"), ("pipe x4", "scale-pipe-4", "ops"),
        ("stat x1", "scale-stat-1", "ops"), ("stat x4", "scale-stat-4", "ops"),
        ("mmap x1", "scale-mmap-1", "ops"), ("mmap x4", "scale-mmap-4", "ops"),
        ("idle-CPU spin, stat x1 (%)", "scale-stat-1", "idle"),
        ("idle-CPU spin, forks (%)", "forks", "idle")]


def parse(paths):
    runs = []
    for p in paths:
        cur = None
        wl = None
        for line in open(p):
            m = re.match(r"== (\S+) smp=(\d+)", line)
            if m:
                cur = {"tag": m.group(1), "smp": int(m.group(2)), "w": {}}
                runs.append(cur)
                continue
            m = re.match(r"-- (\S+)", line)
            if m and cur is not None:
                wl = cur["w"].setdefault(m.group(1), {"cpus": []})
                continue
            if wl is None:
                continue
            m = re.search(r"schedlat time \S+: ([\d.]+) ms", line)
            if m:
                wl["time"] = float(m.group(1))
            m = re.search(r"ops_per_s=(\d+)", line)
            if m:
                wl["ops"] = int(m.group(1)) / 1000.0
            m = re.search(r"cpu\d+ .*hold_us=\d+ \(([\d.]+)%\) spin_us=\d+ \(([\d.]+)%\)", line)
            if m:
                wl["cpus"].append((float(m.group(1)), float(m.group(2))))
    return runs


def value(w, kind):
    if kind in ("time", "ops"):
        return w.get(kind)
    idle = [s for h, s in w["cpus"] if h < 5.0]
    return max(idle) if idle else None


def main():
    a = sys.argv[1:]
    sep = a.index("--")
    runs = parse(a[:sep])
    cols = []
    for spec in a[sep + 1:]:
        pre, smp = spec.rsplit(":", 1)
        cols.append((spec, [r for r in runs if r["tag"].startswith(pre) and r["smp"] == int(smp)]))
    print("| Workload | " + " | ".join(c for c, _ in cols) + " |")
    print("|---|" + "---|" * len(cols))
    for label, wl, kind in ROWS:
        cells = []
        for _, rs in cols:
            vs = [value(r["w"][wl], kind) for r in rs if wl in r["w"]]
            vs = [v for v in vs if v is not None]
            if not vs:
                cells.append("–")
                continue
            fmt = (lambda v: "%.0f" % v) if kind != "idle" else (lambda v: "%.1f" % v)
            med = statistics.median(vs)
            cells.append(fmt(med) + (" [" + ",".join(fmt(v) for v in vs) + "]" if len(vs) > 1 else ""))
        print("| %s | %s |" % (label, " | ".join(cells)))


if __name__ == "__main__":
    main()
