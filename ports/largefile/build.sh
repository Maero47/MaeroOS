#!/bin/sh
# Build testfiles/lfprobe (tools/smoke_largefile.py) with the musl.cc
# i686-linux-musl-cross toolchain (ports/abiprobes/README.md).
set -e
cd "$(dirname "$0")"
CC=${CC:-i686-linux-musl-gcc}
$CC -O2 -Wall -Wextra -static -no-pie -s lfprobe.c -o ../../testfiles/lfprobe
