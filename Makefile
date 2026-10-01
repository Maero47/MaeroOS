# Kernel Makefile — uses relative paths to handle spaces in directory name

CC      := i686-elf-gcc
AS      := nasm
LD      := i686-elf-gcc

CFLAGS  := -std=gnu99 -ffreestanding -nostdlib -fno-builtin \
           -Wall -Wextra -Werror -O2 -g \
           -fno-pic -fno-pie -fno-exceptions \
           -mno-sse -mno-mmx -mno-sse2 -mno-3dnow \
           -fno-omit-frame-pointer \
           -fstack-protector-strong \
           -I./include \
           -I./arch/i686/include \
           -I./net/lwip_port \
           -I./third_party/lwip/src/include

# Hot-path debug tracing (per-exec/per-signal printks, kprof probe spans and
# periodic dump): `make KTRACE=1`.  Off by default.  The stamp file records the
# last setting so flipping it rebuilds the kernel objects.
KTRACE ?= 0
ifeq ($(KTRACE),1)
CFLAGS  += -DKTRACE=1
endif
# Kernel-stack guard self-test (debug builds only, never in a normal build): a
# kernel thread started at boot overflows its own stack on purpose, and the
# serial log must show the overflow diagnosis instead of a reset.
#   make KSTACK_TEST=1   runaway recursion  -> double fault on the #DF task
#   make KSTACK_TEST=2   store into the guard -> page-fault overflow report
#   make KSTACK_TEST=3   mode 1 on an AP (boot with -smp 2+) -> that CPU's #DF task
#   make KSTACK_TEST=4   runaway recursion on the BSP scheduler stack -> #DF task
KSTACK_TEST ?= 0
ifneq ($(KSTACK_TEST),0)
CFLAGS  += -DKSTACK_TEST=$(KSTACK_TEST)
endif
# Kernel heap self-test (debug builds only): `make KHEAP_TEST=1` runs a
# randomised alloc/free/realloc stress with content, invariant and leak checks
# right after heap_init, and poisons free heap memory to catch use-after-free.
# The serial log shows "[HEAP-TEST] PASS" or a FAIL line and a panic.
# KHEAP_TEST=2 then writes through a freed pointer on purpose: the log must
# end in "use after free" and a heap-corruption panic.
KHEAP_TEST ?= 0
ifneq ($(KHEAP_TEST),0)
CFLAGS  += -DKHEAP_TEST=$(KHEAP_TEST)
endif
KTRACE_STAMP := .ktrace-stamp
$(shell [ "$$(cat $(KTRACE_STAMP) 2>/dev/null)" = "$(KTRACE) $(KSTACK_TEST) $(KHEAP_TEST)" ] || echo "$(KTRACE) $(KSTACK_TEST) $(KHEAP_TEST)" > $(KTRACE_STAMP))

ASFLAGS := -f elf32 -g

LDFLAGS := -ffreestanding -nostdlib -lgcc \
           -T./linker.ld \
           -Wl,-z,max-page-size=0x1000

DEPFLAGS = -MT $@ -MMD -MP -MF $(@:.o=.d)

# Collect all kernel C sources (not userspace)
C_SRCS  := $(shell find kernel arch mm fs drivers proc lib net -name "*.c" 2>/dev/null)

# Collect all kernel ASM sources (not userspace)
ASM_SRCS := $(shell find arch -name "*.asm" 2>/dev/null)

C_OBJS   := $(C_SRCS:.c=.o)
ASM_OBJS := $(ASM_SRCS:.asm=.o)

LWIP_SRCS := \
	third_party/lwip/src/core/init.c \
	third_party/lwip/src/core/def.c \
	third_party/lwip/src/core/dns.c \
	third_party/lwip/src/core/inet_chksum.c \
	third_party/lwip/src/core/ip.c \
	third_party/lwip/src/core/mem.c \
	third_party/lwip/src/core/memp.c \
	third_party/lwip/src/core/netif.c \
	third_party/lwip/src/core/pbuf.c \
	third_party/lwip/src/core/raw.c \
	third_party/lwip/src/core/stats.c \
	third_party/lwip/src/core/sys.c \
	third_party/lwip/src/core/altcp.c \
	third_party/lwip/src/core/altcp_alloc.c \
	third_party/lwip/src/core/altcp_tcp.c \
	third_party/lwip/src/core/tcp.c \
	third_party/lwip/src/core/tcp_in.c \
	third_party/lwip/src/core/tcp_out.c \
	third_party/lwip/src/core/timeouts.c \
	third_party/lwip/src/core/udp.c \
	third_party/lwip/src/core/ipv4/acd.c \
	third_party/lwip/src/core/ipv4/dhcp.c \
	third_party/lwip/src/core/ipv4/etharp.c \
	third_party/lwip/src/core/ipv4/icmp.c \
	third_party/lwip/src/core/ipv4/ip4.c \
	third_party/lwip/src/core/ipv4/ip4_addr.c \
	third_party/lwip/src/core/ipv4/ip4_frag.c \
	third_party/lwip/src/netif/ethernet.c

LWIP_OBJS := $(LWIP_SRCS:.c=.o)
LWIP_CFLAGS := $(filter-out -Werror,$(CFLAGS)) -Wno-unused-parameter
ALL_OBJS := $(C_OBJS) $(LWIP_OBJS) $(ASM_OBJS)

$(LWIP_OBJS): CFLAGS := $(LWIP_CFLAGS)

TARGET   := kernel.elf
QEMU_ISO_PID := .qemu-iso.pid

# Host tool discovery.  Everything here is deferred (plain `=`) or silenced so
# that a missing tool fails when the recipe that needs it runs, not while Make
# parses this file.  macOS/Homebrew paths are the fallback, Linux is the default.
# find_tool: first of the given names on PATH, else the first name looked up in
# the usual sbin/Homebrew directories, else the bare name (so the error message
# at use time names the missing tool).
TOOL_DIRS := /usr/sbin /sbin /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin
find_tool = $(shell for t in $(1); do command -v "$$t" 2>/dev/null && exit 0; done; \
	for d in $(TOOL_DIRS); do [ -x "$$d/$(firstword $(1))" ] && echo "$$d/$(firstword $(1))" && exit 0; done; \
	echo $(firstword $(1)))
MKE2FS  = $(call find_tool,mke2fs mkfs.ext2)
DEBUGFS = $(call find_tool,debugfs)
# toybox's build scripts need GNU sed: `gsed` on macOS, plain `sed` on Linux.
SED     = $(shell command -v gsed 2>/dev/null || echo sed)
GCC_INCLUDE := $(shell $(CC) -print-file-name=include 2>/dev/null)
TOYBOX_DIR := third_party/toybox
GRUB_MKRESCUE := $(shell command -v grub-mkrescue 2>/dev/null || command -v i686-elf-grub-mkrescue 2>/dev/null)
GRUB_BIOS_MODULES := $(HOME)/opt/hostpkgs/usr/lib/grub/i386-pc
TOYBOX_CFLAGS := -D__linux__ -std=gnu99 -O2 -g \
	-nostdlib -nostdinc -static -ffreestanding -fno-builtin \
	-fno-pic -fno-pie -mno-sse -mno-mmx -mno-sse2 \
	-fno-omit-frame-pointer -Wall -Wextra \
	-Wno-unused-parameter -Wno-implicit-function-declaration \
	-I../../userspace/include -isystem $(GCC_INCLUDE)
TOYBOX_LDFLAGS := -nostdlib -static -T ../../userspace/user.ld \
	../../userspace/libc/crt0.o ../../userspace/libc/libc.a -lgcc

.PHONY: all run run-net run-disk run-iso restart-iso stop-iso debug gdb clean iso initrd userspace toybox disk disk-ff run-firefox smoke smoke-net smoke-net-e1000 smoke-tcpsrv smoke-fw smoke-disk smoke-pkg smoke-toybox smoke-cmds smoke-dyn smoke-dynlib smoke-x smoke-gtk smoke-gui check abiprobes smoke-abi smoke-firefox smoke-firefox-web repo repo-serve start resolutions icons

all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

$(C_OBJS): $(KTRACE_STAMP)

# C compilation with automatic dependency generation
%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

third_party/lwip/%.o: third_party/lwip/%.c
	$(CC) $(LWIP_CFLAGS) $(DEPFLAGS) -c $< -o $@

# NASM assembly
%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

# Build userspace binaries (libc, init, shell)
userspace:
	$(MAKE) -C userspace

toybox: userspace
	rm -f $(TOYBOX_DIR)/toybox $(TOYBOX_DIR)/generated/unstripped/toybox
	# Fresh checkout: generated/ (gitignored) is created here, which would make
	# the committed .config.maeros look stale and trigger silentoldconfig, whose
	# kconfig/conf sources are not in the tree.  Generate first, then re-date the
	# config so the main build never tries to reconfigure.  Only ask for
	# generated/Config.in: scripts/genconfig.sh writes Config.probed and
	# unstripped/kconfig as side effects, and toybox has no rule for kconfig, so
	# naming it as a goal fails under -j before genconfig has run.
	$(MAKE) -C $(TOYBOX_DIR) generated/Config.in SED=$(SED) HOSTCC=cc && touch $(TOYBOX_DIR)/.config.maeros
	$(MAKE) -C $(TOYBOX_DIR) toybox \
		KCONFIG_CONFIG=.config.maeros \
		SED=$(SED) \
		LDOPTIMIZE='-Wl,--gc-sections' \
		CC=$(CC) \
		HOSTCC=cc \
		STRIP=i686-elf-strip \
		CFLAGS="$(TOYBOX_CFLAGS)" \
		LDFLAGS="$(TOYBOX_LDFLAGS)"
	chmod u+w testfiles/toybox 2>/dev/null || true
	cp $(TOYBOX_DIR)/toybox testfiles/toybox

# Regenerate themed .mic icons from the Reversal-blue SVG theme (needs
# rsvg-convert + the theme in ~/Downloads; .mic files are committed so this
# is a convenience target, not a hard build dep).
icons:
	python3 tools/mkicons.py

# Build the initrd ustar tar from the testfiles directory.
# The large GTK-stack probes (tens of MB each) are kept on the ext2 DISK only —
# the initrd lives permanently in RAM, so bundling them there exhausts memory.
# They run from /disk; smoke-gtk boots with the disk attached.
INITRD_EXCLUDE := --exclude=./cairoprobe --exclude=./pangoprobe \
	--exclude=./firefox --exclude=./fflib \
	--exclude=./usr/share/icons \
	--exclude=./gtkprobe
# COPYFILE_DISABLE=1 stops macOS tar from adding ._* AppleDouble entries; it is
# an ordinary ignored environment variable for GNU tar on Linux.
initrd: userspace toybox
	COPYFILE_DISABLE=1 tar --format=ustar $(INITRD_EXCLUDE) -cf initrd.tar -C testfiles .
	@echo "initrd.tar created."

# Create an ext2 disk image populated from testfiles/.
# Requires e2fsprogs (apt install e2fsprogs / brew install e2fsprogs).
#
# DISK_SIZE_MB / DISK_PRUNE / DISK_IMG are overridable so the same recipe builds
# both the lean default disk (used by smoke-disk / run-disk) and the large
# Firefox disk (disk-ff).  The default disk EXCLUDES the multi-hundred-MB
# firefox/ and fflib/ trees: bundling them makes the image slow to build and
# boot, which destabilises the timing-sensitive service tests in smoke-disk.
DISK_SIZE_MB ?= 384
DISK_IMG     ?= disk.img
DISK_PRUNE   ?= -path 'testfiles/firefox' -o -path 'testfiles/fflib'

# toybox too: testfiles/toybox goes on the disk, and under -j debugfs must not
# read it while the toybox recipe is still copying it.
disk: userspace toybox
	@echo "[DISK]  Building ext2 disk image ($(DISK_SIZE_MB) MiB) -> $(DISK_IMG)..."
	dd if=/dev/zero bs=1M count=$(DISK_SIZE_MB) 2>/dev/null | tr '\000' '\000' > $(DISK_IMG)
	$(MKE2FS) -t ext2 -b 1024 -F $(DISK_IMG) 2>/dev/null
	@find testfiles \( $(DISK_PRUNE) \) -prune -o -type d -print | while read d; do \
	    if [ "$$d" != "testfiles" ]; then \
	        rel=$${d#testfiles/}; \
	        echo "  [DISK]  mkdir /$$rel"; \
	        $(DEBUGFS) -w -R "mkdir /$$rel" $(DISK_IMG) || true; \
	    fi; \
	done
	@find testfiles \( $(DISK_PRUNE) \) -prune -o -type f -print | while read f; do \
	    rel=$${f#testfiles/}; \
	    echo "  [DISK]  $$f -> /$$rel"; \
	    $(DEBUGFS) -w \
	        -R "write $$f /$$rel" $(DISK_IMG) || true; \
	    magic=$$(head -c4 "$$f" | od -An -tx1 | tr -d ' \n'); \
	    if [ "$$magic" = "7f454c46" ]; then \
	        $(DEBUGFS) -w -R "sif /$$rel mode 0100755" $(DISK_IMG) \
	            2>/dev/null || true; \
	    fi; \
	done
	@if [ -f tools/diskperms.txt ]; then \
	    echo "[DISK]  Applying ownership/permission manifest..."; \
	    grep -v '^#' tools/diskperms.txt | while read p u g m; do \
	        [ -z "$$p" ] && continue; \
	        $(DEBUGFS) -w -R "sif /$$p uid $$u" $(DISK_IMG) 2>/dev/null || true; \
	        $(DEBUGFS) -w -R "sif /$$p gid $$g" $(DISK_IMG) 2>/dev/null || true; \
	        $(DEBUGFS) -w -R "sif /$$p mode $$m" $(DISK_IMG) 2>/dev/null || true; \
	    done; \
	fi
	@echo "[DISK]  Done: $(DISK_IMG)"

# Large disk WITH the Firefox + glibc library trees (175 MiB libxul etc.).
# Used to run the prebuilt Firefox ESR; see `make run-firefox`.
# The Firefox runtime tree is gitignored; rebuild it from public sources when
# it is missing (ports/firefox/fetch-runtime.sh, cached under ports/firefox/prebuilt).
testfiles/firefox/firefox-bin:
	sh ports/firefox/fetch-runtime.sh

# userspace/toybox are built here, before the recursive make, so under -j the
# sub-make's own `disk: userspace toybox` finds them done instead of racing the
# iso -> initrd branch for the same objects.
disk-ff: testfiles/firefox/firefox-bin userspace toybox
	$(MAKE) disk DISK_SIZE_MB=1024 DISK_IMG=disk-ff.img DISK_PRUNE="-path testfiles/nonexistent"

# -accel kvm when this user can open /dev/kvm (native speed, real CPU), else TCG.
QEMU_ACCEL := $(shell test -r /dev/kvm -a -w /dev/kvm && echo "-accel kvm" || echo "-accel tcg")

# QEMU_DISPLAY adds display flags to run/run-net/run-disk; the smoke
# suites pass QEMU_DISPLAY="-display none" to boot headless.
QEMU_DISPLAY ?=

# Run in QEMU — uses built-in multiboot loader (no ISO required)
run: $(TARGET) initrd
	qemu-system-i386 $(QEMU_DISPLAY) \
		-kernel $(TARGET) \
		-initrd initrd.tar \
		-serial stdio \
		-m 512M \
		-no-reboot \
		-no-shutdown

# NIC=e1000 for the Intel e1000 instead (drivers/e1000.c).
NIC ?= rtl8139
run-net: $(TARGET) initrd
	qemu-system-i386 $(QEMU_DISPLAY) \
		-kernel $(TARGET) \
		-initrd initrd.tar \
		-serial stdio \
		-m 128M \
		-no-reboot \
		-no-shutdown \
		-netdev user,id=n0 \
		-device $(NIC),netdev=n0

# Run with initrd + ATA disk image
run-disk: $(TARGET) initrd disk
	qemu-system-i386 $(QEMU_DISPLAY) \
		-kernel $(TARGET) \
		-initrd initrd.tar \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-serial stdio \
		-m 128M \
		-no-reboot \
		-no-shutdown

# Boot with the Firefox disk attached (2 GiB RAM).  At the shell, run:
#   /disk/firefox/firefox-bin --version      (proven: prints "Mozilla Firefox 115.15.0esr")
# LD_LIBRARY_PATH and DISPLAY are pre-set by login for the GTK/X stack.
# Firefox needs the desktop, and the desktop needs a framebuffer, which only
# the GRUB ISO path provides (gfxpayload).  The -kernel path boots without
# /dev/fb0, so init never starts the graphical session and ff cannot run.
# With /disk/ffauto on the disk the desktop launches ff by itself.
run-firefox: $(TARGET) iso disk-ff
	qemu-system-i386 \
		-cdrom maeros.iso \
		-drive file=disk-ff.img,format=raw,if=ide \
		$(QEMU_ACCEL) \
		-vga std \
		-serial stdio \
		-m 2048M \
		-no-reboot \
		-no-shutdown

smoke: $(TARGET) initrd
	python3 tools/smoke.py

smoke-net: $(TARGET) initrd
	python3 tools/smoke_net.py

# The same on an Intel e1000, with the disk (snapshot) for a writable /etc:
# DHCP's resolv.conf and DNS lookups against a responder in the harness.
smoke-net-e1000: $(TARGET) initrd disk
	python3 tools/smoke_net_e1000.py

# AF_INET listen/accept and blocking UDP, reached through QEMU hostfwd
# (e1000; SMOKE_TCPSRV_ARGS="--nic rtl8139" for the other NIC).
smoke-tcpsrv: $(TARGET) initrd
	python3 tools/smoke_tcpsrv.py $(SMOKE_TCPSRV_ARGS)

smoke-fw: $(TARGET) initrd
	python3 tools/smoke_fw.py

smoke-disk: $(TARGET) initrd disk
	python3 tools/smoke_disk.py

# pkg against a host HTTP repo: signed-index checks, install, rollback.
smoke-pkg: $(TARGET) initrd disk repo
	python3 tools/smoke_pkg.py

smoke-toybox: $(TARGET) initrd
	python3 tools/smoke_toybox.py

smoke-cmds: $(TARGET) initrd
	python3 tools/smoke_cmds.py

smoke-dyn: $(TARGET) initrd
	python3 tools/smoke_dyn.py

smoke-dynlib: $(TARGET) initrd
	python3 tools/smoke_dynlib.py

smoke-x: $(TARGET) initrd
	python3 tools/smoke_x.py

smoke-gtk: $(TARGET) initrd
	python3 tools/smoke_gtk.py

# The desktop and its apps, driven through QMP mouse/keyboard input on the
# ISO + a copy of disk.img (~30 s).  Screendumps and the serial log land in
# build/smoke-gui/; see tools/smoke_gui.py.
smoke-gui: $(TARGET) iso disk
	python3 tools/smoke_gui.py

# The headless suites CI runs (.github/workflows/ci.yml), in one command.
# Each suite's console goes to $(CHECK_LOG_DIR)/<suite>.log; a failure prints
# the tail of its log and the rest still run, then check exits non-zero.
# Suites run one at a time: each boots QEMU under TCG and their prompt
# timeouts assume the guest has a host core to itself.  The suites start QEMU
# with -display none (tools/smokelib.py), so no display is needed.
# Pick a subset with CHECK_SUITES="smoke smoke-x".
CHECK_SUITES  ?= smoke smoke-cmds smoke-toybox smoke-disk smoke-net smoke-net-e1000 smoke-tcpsrv smoke-fw \
                 smoke-dyn smoke-dynlib smoke-x smoke-pkg smoke-gui
CHECK_LOG_DIR ?= build/check

# repo: smoke-pkg serves packages from repo/ (see the smoke-pkg target).
# iso: smoke-gui boots the desktop, which needs the ISO's framebuffer.
check: $(TARGET) initrd disk repo iso
	@mkdir -p $(CHECK_LOG_DIR); rm -f $(CHECK_LOG_DIR)/*.log; failed=""; \
	for s in $(CHECK_SUITES); do \
	    log=$(CHECK_LOG_DIR)/$$s.log; t0=$$(date +%s); \
	    if python3 tools/$$(echo $$s | tr - _).py >$$log 2>&1; then \
	        echo "[CHECK] $$s: pass ($$(( $$(date +%s) - t0 ))s)"; \
	    else \
	        echo "[CHECK] $$s: FAIL ($$(( $$(date +%s) - t0 ))s), last lines of $$log:"; \
	        tail -n 30 $$log | sed 's/^/    /'; \
	        failed="$$failed $$s"; \
	    fi; \
	done; \
	if [ -n "$$failed" ]; then \
	    echo "[CHECK] failed:$$failed"; \
	    exit 1; \
	fi; \
	echo "[CHECK] all passed: $(CHECK_SUITES)"

# Linux-ABI probes (docs/audit/firefox-first-paint.md section 8).  The static
# musl probes are built into testfiles/abiprobes/ so the initrd picks them up;
# smoke_abi.py boots QEMU itself (it needs -m 1024M for P18) and runs each one.
# Needs i686-linux-musl-gcc (ports/abiprobes/README.md).
abiprobes:
	$(MAKE) -C ports/abiprobes

# The disk is a dependency because p26_unlink_frees_space measures real ext2
# free space; every other probe runs out of the initrd alone.
smoke-abi: $(TARGET) abiprobes initrd disk
	python3 tools/smoke_abi.py

# initrd and disk pack testfiles/abiprobes/, so under -j they must wait for it.
ifneq ($(filter smoke-abi,$(MAKECMDGOALS)),)
initrd disk: | abiprobes
endif

# Does Firefox 115 paint a window on the desktop?  Boots the ISO with the
# Firefox disk headless (KVM when available), lets the desktop launch ff, and
# judges PASS/FAIL from the serial console.  Artifacts (serial log, screendump,
# summary) land in build/ff-smoke/<timestamp>-<accel>-smpN/.  Options pass
# through SMOKE_FF_ARGS, e.g. make smoke-firefox SMOKE_FF_ARGS="--smp 2 --accel tcg".
smoke-firefox: $(TARGET) iso disk-ff
	python3 tools/smoke_firefox.py $(SMOKE_FF_ARGS)

# The same boot, then a page load over the network: a page served from the host
# (HTML, a stylesheet rule, a PNG) must be fetched and its image reach the screen.
smoke-firefox-web: $(TARGET) iso disk-ff
	python3 tools/smoke_firefox.py --web $(SMOKE_FF_ARGS)

# Run with full interrupt + CPU-reset logging
debug: $(TARGET)
	qemu-system-i386 \
		-kernel $(TARGET) \
		-serial stdio \
		-m 128M \
		-no-reboot \
		-no-shutdown \
		-d int,cpu_reset \
		-D /tmp/qemu.log

# Start QEMU frozen, waiting for GDB on port 1234
gdb: $(TARGET)
	qemu-system-i386 \
		-kernel $(TARGET) \
		-serial stdio \
		-m 128M \
		-no-reboot \
		-no-shutdown \
		-s -S

# Run the GRUB ISO path. This is the framebuffer-capable boot path because
# QEMU's direct -kernel Multiboot loader does not provide VBE framebuffer info.
# ── The one-command interactive experience ──────────────────────────────
# `make start` = build disk+iso, serve the app repo on :8000, open QEMU
# with display + sound + network.  The repo server is stopped when the
# QEMU window closes.

repo: $(wildcard ports/packages/*/*) tools/mkrepo.py tools/ed25519.py
	python3 tools/mkrepo.py

repo-serve: repo
	@sh tools/run-maeros.sh --free-port 8000
	@echo "Serving app repo at http://localhost:8000 (guest: 10.0.2.2:8000)"
	cd repo && python3 -m http.server 8000

# `make start` auto-fits the guest resolution to the host screen (osascript on
# macOS, xrandr/xdpyinfo on Linux) and serves the app repo.  Override the
# resolution with `make start RES=1280x720`.
start: disk iso repo
	SERVE_REPO=1 sh tools/run-maeros.sh $(RES)

# Just list the supported resolutions + the auto-pick for this screen.
resolutions:
	@sh tools/run-maeros.sh --list

run-iso: iso
	@if [ -f "$(QEMU_ISO_PID)" ]; then \
		pid=$$(cat "$(QEMU_ISO_PID)"); \
		if ps -p "$$pid" -o comm= 2>/dev/null | grep -q qemu-system-i386; then \
			echo "Stopping previous MaeroOS QEMU pid $$pid"; \
			kill "$$pid" 2>/dev/null || true; \
			sleep 1; \
		fi; \
		rm -f "$(QEMU_ISO_PID)"; \
	fi
	@qemu-system-i386 \
		-cdrom maeros.iso \
		-serial stdio \
		-m 128M \
		-no-reboot \
		-no-shutdown & \
	pid=$$!; \
	echo $$pid > "$(QEMU_ISO_PID)"; \
	wait $$pid; \
	status=$$?; \
	rm -f "$(QEMU_ISO_PID)"; \
	exit $$status

restart-iso: run-iso

stop-iso:
	@if [ -f "$(QEMU_ISO_PID)" ]; then \
		pid=$$(cat "$(QEMU_ISO_PID)"); \
		if ps -p "$$pid" -o comm= 2>/dev/null | grep -q qemu-system-i386; then \
			echo "Stopping MaeroOS QEMU pid $$pid"; \
			kill "$$pid" 2>/dev/null || true; \
		else \
			echo "No running MaeroOS QEMU for pid $$pid"; \
		fi; \
		rm -f "$(QEMU_ISO_PID)"; \
	else \
		echo "No MaeroOS QEMU PID file"; \
	fi

# Generate a bootable ISO (requires grub-mkrescue or i686-elf-grub-mkrescue + xorriso)
iso: $(TARGET) initrd
	@test -n "$(GRUB_MKRESCUE)" || { echo "iso: need grub-mkrescue (grub-common grub-pc-bin xorriso mtools)"; exit 1; }
	mkdir -p isodir/boot/grub
	cp $(TARGET) isodir/boot/
	cp initrd.tar isodir/boot/
	printf 'serial --unit=0 --speed=115200\nterminal_input serial console\nterminal_output serial console\nset timeout=1\nset gfxpayload=1920x1080x32\nmenuentry "MaeroOS" {\n    multiboot /boot/kernel.elf\n    module /boot/initrd.tar\n    boot\n}\n' \
		> isodir/boot/grub/grub.cfg
	@# A distro grub-mkrescue without grub-pc-bin makes an EFI-only ISO that
	@# SeaBIOS cannot boot.  Point it at tools/setup-linux.sh --no-sudo's
	@# relocated BIOS modules, or stop.  Scripts (its wrappers) and Homebrew's
	@# i686-elf-grub-mkrescue already know where their modules are.
	@g="$(GRUB_MKRESCUE)"; d=""; \
	if [ "$${g##*/}" = grub-mkrescue ] && [ "$$(head -c 2 "$$g")" != '#!' ]; then \
		r=$$(readlink -f "$$g" 2>/dev/null || echo "$$g"); \
		if [ ! -d "$${r%/bin/*}/lib/grub/i386-pc" ]; then \
			if [ -d "$(GRUB_BIOS_MODULES)" ]; then d="$(GRUB_BIOS_MODULES)"; else \
				echo "iso: $$g has no BIOS modules ($${r%/bin/*}/lib/grub/i386-pc); the ISO would not boot in QEMU."; \
				echo "iso: fix with 'sudo apt install grub-pc-bin' or 'tools/setup-linux.sh --no-sudo'"; \
				exit 1; \
			fi; \
		fi; \
	fi; \
	echo "$$g $${d:+-d $$d }-o maeros.iso isodir"; \
	"$$g" $${d:+-d "$$d"} -o maeros.iso isodir

clean:
	find kernel arch/i686 mm fs drivers proc lib net third_party/lwip/src \
		\( -name "*.o" -o -name "*.d" \) -delete 2>/dev/null || true
	rm -f $(TARGET) maeros.iso initrd.tar disk.img disk-ff.img $(QEMU_ISO_PID) $(KTRACE_STAMP)
	rm -rf isodir repo
	rm -rf $(TOYBOX_DIR)/generated
	rm -f $(TOYBOX_DIR)/toybox $(TOYBOX_DIR)/.singlemake testfiles/toybox
	$(MAKE) -C userspace clean

# Pull in auto-generated dependency files (silence errors if none exist yet)
-include $(C_OBJS:.o=.d) $(LWIP_OBJS:.o=.d)
