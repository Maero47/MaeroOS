#!/usr/bin/env python3
"""
mkrepo.py — build the MaeroOS package repository.

Reads package recipes from ports/packages/<name>/ (a `pkg.conf` plus the
files to ship), produces repo/ with index.txt + <name>-<ver>.tar (ustar).

pkg.conf format (key=value):
    version=1.0
    caption=One-line description shown in the Store
    exec=/disk/apps/<name>/<binary>     (what the launcher runs)
    fullscreen=1                        (optional: app owns the screen)
    args=-iwad /disk/apps/doom/doom1.wad  (optional launch args)

index.txt line format (| separated):
    name|version|size|tarfile|caption|exec|fullscreen|args
"""
import os
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKGS = os.path.join(ROOT, "ports", "packages")
REPO = os.path.join(ROOT, "repo")


def read_conf(path):
    conf = {"version": "1.0", "caption": "", "exec": "", "fullscreen": "0",
            "args": "", "rawinput": "0"}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                conf[k.strip()] = v.strip()
    return conf


def main():
    os.makedirs(REPO, exist_ok=True)
    index = []
    for name in sorted(os.listdir(PKGS)):
        pdir = os.path.join(PKGS, name)
        confp = os.path.join(pdir, "pkg.conf")
        if not os.path.isdir(pdir) or not os.path.exists(confp):
            continue
        conf = read_conf(confp)
        tar_name = f"{name}-{conf['version']}.tar"
        tar_path = os.path.join(REPO, tar_name)
        with tarfile.open(tar_path, "w", format=tarfile.USTAR_FORMAT) as tf:
            for fn in sorted(os.listdir(pdir)):
                if fn == "pkg.conf":
                    continue
                tf.add(os.path.join(pdir, fn), arcname=fn)
        size = os.path.getsize(tar_path)
        index.append("|".join([
            name, conf["version"], str(size), tar_name, conf["caption"],
            conf["exec"], conf["fullscreen"], conf["args"],
            conf["rawinput"]]))
        print(f"  {name} {conf['version']}: {size} bytes")
    with open(os.path.join(REPO, "index.txt"), "w") as f:
        f.write("\n".join(index) + "\n")
    print(f"repo: {len(index)} package(s) → {REPO}")


if __name__ == "__main__":
    sys.exit(main())
