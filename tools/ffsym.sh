#!/bin/bash
# ffsym.sh — symbolize Firefox/glibc fault addresses from the kernel's [bt] dump.
#
# The kernel prints user-space return addresses as raw VAs.  Library load bases
# are fixed (no ASLR), so we map each address to <lib>+<offset> and run
# i686-elf-addr2line against the actual .so on disk.
#
# Usage:
#   tools/ffsym.sh 0x46f5e962 0x40418870 ...
#   grep '\[bt\]' /tmp/ff.log | grep -oE '0x[0-9a-f]+' | xargs tools/ffsym.sh
#
# Bases below are the deterministic load addresses observed in the [MAP] trace.
# Re-capture them (temporarily re-enable the [MAP] printk in sys_mmap2) if the
# library set changes.

FFDIR="testfiles/firefox"
A2L="i686-elf-addr2line"
command -v "$A2L" >/dev/null 2>&1 || A2L="$(ls /opt/homebrew/opt/i686-elf-binutils/bin/i686-elf-addr2line 2>/dev/null)"

# lib base(hex)  (sorted by base; first base <= addr wins)
read -r -d '' MAP <<'EOF'
libpthread.so.0 40037000
libdl.so.2 4003c000
libstdc++.so.6 40041000
libm.so.6 40261000
libgcc_s.so.1 40366000
libc.so.6 4038d000
libnspr4.so 413bd000
libgtk-3.so.0 4170b000
libgdk-3.so.0 42051000
libglib-2.0.so.0 42175000
libgobject-2.0.so.0 422cc000
libgio-2.0.so.0 42332000
libcairo.so.2 42703000
libX11.so.6 42a69000
libxcb.so.1 43012000
libxul.so 435b8000
EOF

sym() {
  local addr=$((16#${1#0x}))
  local best_name="" best_base=0
  while read -r name base; do
    [ -z "$name" ] && continue
    local b=$((16#$base))
    if [ "$addr" -ge "$b" ] && [ "$b" -gt "$best_base" ]; then
      best_name="$name"; best_base="$b"
    fi
  done <<< "$MAP"
  if [ -z "$best_name" ]; then
    printf '%#x  (below all known bases)\n' "$addr"; return
  fi
  local off=$((addr - best_base))
  local sofile="$FFDIR/$best_name"
  printf '%#x  %s+%#x  ' "$addr" "$best_name" "$off"
  if [ -n "$A2L" ] && [ -f "$sofile" ]; then
    "$A2L" -f -C -e "$sofile" "$off" 2>/dev/null | tr '\n' ' '
  fi
  echo
}

for a in "$@"; do
  case "$a" in 0x*|[0-9a-fA-F]*) sym "$a" ;; esac
done
