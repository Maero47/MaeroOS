# Linux-ABI probe programs (docs/audit/firefox-first-paint.md, section 8).
#
# Each pNN_*.c is one self-checking static musl binary that prints a single
# final PASS/FAIL/SKIP line.  The same binary is the Linux reference when run
# on an x86_64 host that executes i386 ELF files, and the regression test when
# run on MaeroOS by tools/smoke_abi.py.  See README.md for the mapping.
#
# Toolchain: musl.cc i686-linux-musl-cross, on PATH or under $MUSL_PREFIX
# (the ports/ Docker image installs it in /opt; a host install goes to
# $HOME/opt).  No glibc is required.

MUSL_PREFIX ?= $(firstword $(wildcard /opt/i686-linux-musl-cross ~/opt/i686-linux-musl-cross))
CC := $(or $(shell command -v i686-linux-musl-gcc 2>/dev/null),$(MUSL_PREFIX)/bin/i686-linux-musl-gcc)

# -no-pie: plain ET_EXEC like testfiles/glibc-static, the simplest loader path.
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?= -static -no-pie
LDLIBS  ?= -lpthread

OUT  ?= ../../testfiles/abiprobes
SRCS := $(sort $(wildcard p[0-9][0-9]_*.c))
BINS := $(patsubst %.c,$(OUT)/%,$(SRCS))

.PHONY: all clean host-check list

all: $(BINS)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/%: %.c probe.h | $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

list:
	@for b in $(BINS); do echo $$b; done

# Run every probe on the build host and summarise.  This is the Linux
# reference run: every line must be PASS (or SKIP with a reason).
host-check: all
	@pass=0; fail=0; skip=0; \
	for b in $(BINS); do \
	    out=$$($$b 2>&1 | tail -n 1); \
	    echo "$$out"; \
	    case "$$out" in \
	        PASS*) pass=$$((pass+1));; \
	        SKIP*) skip=$$((skip+1));; \
	        *)     fail=$$((fail+1));; \
	    esac; \
	done; \
	echo "host-check: $$pass pass, $$skip skip, $$fail fail"; \
	test $$fail -eq 0

clean:
	rm -f $(BINS)
