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
TOYBOX_CFLAGS := -D__linux__ -std=gnu99 -O2 -g \
	-nostdlib -nostdinc -static -ffreestanding -fno-builtin \
	-fno-pic -fno-pie -mno-sse -mno-mmx -mno-sse2 \
	-fno-omit-frame-pointer -Wall -Wextra \
	-Wno-unused-parameter -Wno-implicit-function-declaration \
	-I../../userspace/include -isystem $(GCC_INCLUDE)
TOYBOX_LDFLAGS := -nostdlib -static -T ../../userspace/user.ld \
	../../userspace/libc/crt0.o ../../userspace/libc/libc.a -lgcc

.PHONY: all run run-net run-disk run-iso restart-iso stop-iso debug gdb clean iso initrd userspace toybox disk disk-ff run-firefox smoke smoke-net smoke-fw smoke-disk smoke-toybox smoke-cmds smoke-dyn smoke-dynlib smoke-x smoke-gtk abiprobes smoke-abi repo repo-serve start resolutions icons

all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

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

disk: userspace
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
disk-ff:
	$(MAKE) disk DISK_SIZE_MB=1024 DISK_IMG=disk-ff.img DISK_PRUNE="-path testfiles/nonexistent"

# Run in QEMU — uses built-in multiboot loader (no ISO required)
run: $(TARGET) initrd
	qemu-system-i386 \
		-kernel $(TARGET) \
		-initrd initrd.tar \
		-serial stdio \
		-m 512M \
		-no-reboot \
		-no-shutdown

run-net: $(TARGET) initrd
	qemu-system-i386 \
		-kernel $(TARGET) \
		-initrd initrd.tar \
		-serial stdio \
		-m 128M \
		-no-reboot \
		-no-shutdown \
		-netdev user,id=n0 \
		-device rtl8139,netdev=n0

# Run with initrd + ATA disk image
run-disk: $(TARGET) initrd disk
	qemu-system-i386 \
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
run-firefox: $(TARGET) initrd disk-ff
	qemu-system-i386 \
		-kernel $(TARGET) \
		-initrd initrd.tar \
		-drive file=disk-ff.img,format=raw,if=ide \
		-serial stdio \
		-m 2048M \
		-no-reboot \
		-no-shutdown

smoke: $(TARGET) initrd
	python3 tools/smoke.py

smoke-net: $(TARGET) initrd
	python3 tools/smoke_net.py

smoke-fw: $(TARGET) initrd
	python3 tools/smoke_fw.py

smoke-disk: $(TARGET) initrd disk
	python3 tools/smoke_disk.py

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

# Linux-ABI probes (docs/audit/firefox-first-paint.md section 8).  The static
# musl probes are built into testfiles/abiprobes/ so the initrd picks them up;
# smoke_abi.py boots QEMU itself (it needs -m 1024M for P18) and runs each one.
# Needs i686-linux-musl-gcc (ports/abiprobes/README.md).
abiprobes:
	$(MAKE) -C ports/abiprobes

smoke-abi: $(TARGET) abiprobes initrd
	python3 tools/smoke_abi.py

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

repo: $(wildcard ports/packages/*/*)
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
	mkdir -p isodir/boot/grub
	cp $(TARGET) isodir/boot/
	cp initrd.tar isodir/boot/
	printf 'serial --unit=0 --speed=115200\nterminal_input serial console\nterminal_output serial console\nset timeout=1\nset gfxpayload=1920x1080x32\nmenuentry "MaeroOS" {\n    multiboot /boot/kernel.elf\n    module /boot/initrd.tar\n    boot\n}\n' \
		> isodir/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o maeros.iso isodir

clean:
	find kernel arch/i686 mm fs drivers proc lib net third_party/lwip/src \
		\( -name "*.o" -o -name "*.d" \) -delete 2>/dev/null || true
	rm -f $(TARGET) maeros.iso initrd.tar disk.img $(QEMU_ISO_PID)
	rm -rf isodir
	$(MAKE) -C userspace clean

# Pull in auto-generated dependency files (silence errors if none exist yet)
-include $(C_OBJS:.o=.d)
