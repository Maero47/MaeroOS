CC  := i686-elf-gcc
AS  := nasm
AR  := i686-elf-ar

# Silenced so a missing cross compiler fails at compile time, not at parse time.
GCC_INCLUDE := $(shell $(CC) -print-file-name=include 2>/dev/null)

CFLAGS := -std=gnu99 -O2 -g \
          -nostdlib -nostdinc -static \
          -ffreestanding -fno-builtin \
          -fno-pic -fno-pie \
          -mno-sse -mno-mmx -mno-sse2 \
          -fno-omit-frame-pointer \
          -Wall -Wextra \
          -I../userspace/include \
          -isystem $(GCC_INCLUDE)

LDFLAGS := -nostdlib -static -T ../userspace/user.ld

TESTFILES := ../testfiles

LIBC_SRCS := libc/syscalls.c libc/stdio.c libc/stdlib.c libc/pthread.c \
             libc/string.c libc/dirent.c libc/termios.c libc/socket.c \
             libc/resolve.c libc/toybox_compat.c
LIBC_OBJS := $(LIBC_SRCS:.c=.o)
CRT0_OBJ  := libc/crt0.o

.PHONY: all clean

all: $(TESTFILES)/init $(TESTFILES)/shell $(TESTFILES)/ls \
     $(TESTFILES)/cat $(TESTFILES)/cp $(TESTFILES)/rm \
     $(TESTFILES)/touch $(TESTFILES)/mkdir $(TESTFILES)/head \
     $(TESTFILES)/grep $(TESTFILES)/wc $(TESTFILES)/sleep \
     $(TESTFILES)/kill $(TESTFILES)/test \
     $(TESTFILES)/sort $(TESTFILES)/uniq $(TESTFILES)/cut \
     $(TESTFILES)/printf \
     $(TESTFILES)/env $(TESTFILES)/tee $(TESTFILES)/tr \
	     $(TESTFILES)/false $(TESTFILES)/true \
	     $(TESTFILES)/basename $(TESTFILES)/dirname \
	     $(TESTFILES)/sed $(TESTFILES)/awk $(TESTFILES)/find \
	     $(TESTFILES)/date $(TESTFILES)/id $(TESTFILES)/ps $(TESTFILES)/xargs \
	     $(TESTFILES)/svc $(TESTFILES)/session $(TESTFILES)/login $(TESTFILES)/passwd $(TESTFILES)/doas $(TESTFILES)/fwctl $(TESTFILES)/getty \
	     $(TESTFILES)/diff $(TESTFILES)/tar \
	     $(TESTFILES)/mkfifo $(TESTFILES)/ln $(TESTFILES)/readlink \
	     $(TESTFILES)/sysprobe $(TESTFILES)/fcprobe $(TESTFILES)/lspci $(TESTFILES)/ifconfig \
	     $(TESTFILES)/diskprobe $(TESTFILES)/respawnprobe \
	     $(TESTFILES)/sockprobe $(TESTFILES)/netprobe $(TESTFILES)/httpget \
	     $(TESTFILES)/randprobe \
	     $(TESTFILES)/fbtest $(TESTFILES)/evtest $(TESTFILES)/mousetest \
	     $(TESTFILES)/polltest $(TESTFILES)/ptytest $(TESTFILES)/cttytest \
	     $(TESTFILES)/desktop $(TESTFILES)/wmctl $(TESTFILES)/gui_demo \
	     $(TESTFILES)/wmevtest $(TESTFILES)/uidemo $(TESTFILES)/files \
	     $(TESTFILES)/dmesg $(TESTFILES)/shmprobe $(TESTFILES)/threadprobe $(TESTFILES)/term $(TESTFILES)/browse \
     $(TESTFILES)/raceprobe $(TESTFILES)/extprobe $(TESTFILES)/store $(TESTFILES)/pkg $(TESTFILES)/wavplay $(TESTFILES)/statprobe $(TESTFILES)/edit $(TESTFILES)/calc $(TESTFILES)/taskmgr $(TESTFILES)/view $(TESTFILES)/settings \
     $(TESTFILES)/reboot $(TESTFILES)/shutdown $(TESTFILES)/poweroff $(TESTFILES)/halt \
     $(TESTFILES)/sudo $(TESTFILES)/whoami $(TESTFILES)/uname $(TESTFILES)/clear $(TESTFILES)/hostname \
     $(TESTFILES)/which $(TESTFILES)/free $(TESTFILES)/uptime $(TESTFILES)/du $(TESTFILES)/df $(TESTFILES)/top $(TESTFILES)/wget $(TESTFILES)/bbwrap \
     $(TESTFILES)/maerox $(TESTFILES)/ff

# Compile C → .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

libc/clone.o: libc/clone.asm
	nasm -f elf32 $< -o $@

# Assemble crt0
libc/crt0.o: libc/crt0.asm
	$(AS) -f elf32 $< -o $@

# Static library
libc/libc.a: $(LIBC_OBJS) libc/clone.o
	$(AR) rcs $@ $^

# init binary
init/init.o: init/init.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/init: $(CRT0_OBJ) init/init.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) init/init.o libc/libc.a

# shell binary
shell/shell.o: shell/shell.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/shell: $(CRT0_OBJ) shell/shell.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) shell/shell.o libc/libc.a

# ls binary
ls/ls.o: ls/ls.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/ls: $(CRT0_OBJ) ls/ls.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) ls/ls.o libc/libc.a

# cat
cat/cat.o: cat/cat.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/cat: $(CRT0_OBJ) cat/cat.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) cat/cat.o libc/libc.a

# cp
cp/cp.o: cp/cp.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/cp: $(CRT0_OBJ) cp/cp.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) cp/cp.o libc/libc.a

# rm
rm/rm.o: rm/rm.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/rm: $(CRT0_OBJ) rm/rm.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) rm/rm.o libc/libc.a

# touch
touch/touch.o: touch/touch.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/touch: $(CRT0_OBJ) touch/touch.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) touch/touch.o libc/libc.a

# mkdir
mkdir_cmd/mkdir.o: mkdir_cmd/mkdir.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/mkdir: $(CRT0_OBJ) mkdir_cmd/mkdir.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) mkdir_cmd/mkdir.o libc/libc.a

# head
head/head.o: head/head.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/head: $(CRT0_OBJ) head/head.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) head/head.o libc/libc.a

# grep
grep/grep.o: grep/grep.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/grep: $(CRT0_OBJ) grep/grep.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) grep/grep.o libc/libc.a

# wc
wc/wc.o: wc/wc.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/wc: $(CRT0_OBJ) wc/wc.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) wc/wc.o libc/libc.a

# sleep
sleep/sleep.o: sleep/sleep.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/sleep: $(CRT0_OBJ) sleep/sleep.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) sleep/sleep.o libc/libc.a

# kill
kill_cmd/kill.o: kill_cmd/kill.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/kill: $(CRT0_OBJ) kill_cmd/kill.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) kill_cmd/kill.o libc/libc.a

# test / [
test_cmd/test.o: test_cmd/test.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/test: $(CRT0_OBJ) test_cmd/test.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) test_cmd/test.o libc/libc.a

# sort
sort/sort.o: sort/sort.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/sort: $(CRT0_OBJ) sort/sort.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) sort/sort.o libc/libc.a

# uniq
uniq/uniq.o: uniq/uniq.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/uniq: $(CRT0_OBJ) uniq/uniq.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) uniq/uniq.o libc/libc.a

# cut
cut/cut.o: cut/cut.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/cut: $(CRT0_OBJ) cut/cut.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) cut/cut.o libc/libc.a

# printf
printf_cmd/printf.o: printf_cmd/printf.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/printf: $(CRT0_OBJ) printf_cmd/printf.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) printf_cmd/printf.o libc/libc.a

# env
env/env.o: env/env.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/env: $(CRT0_OBJ) env/env.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) env/env.o libc/libc.a

# tee
tee/tee.o: tee/tee.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/tee: $(CRT0_OBJ) tee/tee.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) tee/tee.o libc/libc.a

# tr
tr/tr.o: tr/tr.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/tr: $(CRT0_OBJ) tr/tr.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) tr/tr.o libc/libc.a

# false
false_cmd/false.o: false_cmd/false.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/false: $(CRT0_OBJ) false_cmd/false.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) false_cmd/false.o libc/libc.a

# true
true_cmd/true.o: true_cmd/true.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/true: $(CRT0_OBJ) true_cmd/true.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) true_cmd/true.o libc/libc.a

# basename
basename_cmd/basename.o: basename_cmd/basename.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/basename: $(CRT0_OBJ) basename_cmd/basename.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) basename_cmd/basename.o libc/libc.a

# dirname
dirname_cmd/dirname.o: dirname_cmd/dirname.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/dirname: $(CRT0_OBJ) dirname_cmd/dirname.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) dirname_cmd/dirname.o libc/libc.a

# sed
sed/sed.o: sed/sed.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/sed: $(CRT0_OBJ) sed/sed.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) sed/sed.o libc/libc.a

# awk
awk/awk.o: awk/awk.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/awk: $(CRT0_OBJ) awk/awk.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) awk/awk.o libc/libc.a

# find
find_cmd/find.o: find_cmd/find.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/find: $(CRT0_OBJ) find_cmd/find.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) find_cmd/find.o libc/libc.a

# date
date_cmd/date.o: date_cmd/date.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/date: $(CRT0_OBJ) date_cmd/date.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) date_cmd/date.o libc/libc.a

# id
id_cmd/id.o: id_cmd/id.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/id: $(CRT0_OBJ) id_cmd/id.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) id_cmd/id.o libc/libc.a

# ps
ps/ps.o: ps/ps.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/ps: $(CRT0_OBJ) ps/ps.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) ps/ps.o libc/libc.a

# xargs
xargs_cmd/xargs.o: xargs_cmd/xargs.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/xargs: $(CRT0_OBJ) xargs_cmd/xargs.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) xargs_cmd/xargs.o libc/libc.a

# diff
diff/diff.o: diff/diff.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/diff: $(CRT0_OBJ) diff/diff.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) diff/diff.o libc/libc.a

# tar
tar_cmd/tar.o: tar_cmd/tar.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/tar: $(CRT0_OBJ) tar_cmd/tar.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) tar_cmd/tar.o libc/libc.a

# mkfifo
mkfifo_cmd/mkfifo.o: mkfifo_cmd/mkfifo.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/mkfifo: $(CRT0_OBJ) mkfifo_cmd/mkfifo.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) mkfifo_cmd/mkfifo.o libc/libc.a

# ln
ln_cmd/ln.o: ln_cmd/ln.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/ln: $(CRT0_OBJ) ln_cmd/ln.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) ln_cmd/ln.o libc/libc.a

# readlink
readlink_cmd/readlink.o: readlink_cmd/readlink.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/readlink: $(CRT0_OBJ) readlink_cmd/readlink.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) readlink_cmd/readlink.o libc/libc.a

# sysprobe
sysprobe/sysprobe.o: sysprobe/sysprobe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/sysprobe: $(CRT0_OBJ) sysprobe/sysprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) sysprobe/sysprobe.o libc/libc.a

# fcprobe (fontconfig writability diagnostic)
fcprobe/fcprobe.o: fcprobe/fcprobe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/fcprobe: $(CRT0_OBJ) fcprobe/fcprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) fcprobe/fcprobe.o libc/libc.a

# lspci
lspci/lspci.o: lspci/lspci.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/lspci: $(CRT0_OBJ) lspci/lspci.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) lspci/lspci.o libc/libc.a

# ifconfig
ifconfig/ifconfig.o: ifconfig/ifconfig.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/ifconfig: $(CRT0_OBJ) ifconfig/ifconfig.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) ifconfig/ifconfig.o libc/libc.a

# diskprobe
diskprobe/diskprobe.o: diskprobe/diskprobe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/diskprobe: $(CRT0_OBJ) diskprobe/diskprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) diskprobe/diskprobe.o libc/libc.a

# respawnprobe
respawnprobe/respawnprobe.o: respawnprobe/respawnprobe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/respawnprobe: $(CRT0_OBJ) respawnprobe/respawnprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) respawnprobe/respawnprobe.o libc/libc.a

# svc
svc/svc.o: svc/svc.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/svc: $(CRT0_OBJ) svc/svc.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) svc/svc.o libc/libc.a

# session
session/session.o: session/session.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/session: $(CRT0_OBJ) session/session.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) session/session.o libc/libc.a

# auth helper
auth/auth.o: auth/auth.c auth/auth.h
	$(CC) $(CFLAGS) -c auth/auth.c -o $@

# getty
getty/getty.o: getty/getty.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/getty: $(CRT0_OBJ) getty/getty.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) getty/getty.o libc/libc.a

# login
login/login.o: login/login.c auth/auth.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/login: $(CRT0_OBJ) login/login.o auth/auth.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) login/login.o auth/auth.o libc/libc.a

# passwd
passwd/passwd.o: passwd/passwd.c auth/auth.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/passwd: $(CRT0_OBJ) passwd/passwd.o auth/auth.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) passwd/passwd.o auth/auth.o libc/libc.a

# doas (set-uid root privilege tool)
doas/doas.o: doas/doas.c auth/auth.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/doas: $(CRT0_OBJ) doas/doas.o auth/auth.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) doas/doas.o auth/auth.o libc/libc.a

# fwctl (firewall control)
fwctl/fwctl.o: fwctl/fwctl.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/fwctl: $(CRT0_OBJ) fwctl/fwctl.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) fwctl/fwctl.o libc/libc.a

# sockprobe
sockprobe/sockprobe.o: sockprobe/sockprobe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/sockprobe: $(CRT0_OBJ) sockprobe/sockprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) sockprobe/sockprobe.o libc/libc.a

# netprobe
netprobe/netprobe.o: netprobe/netprobe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/netprobe: $(CRT0_OBJ) netprobe/netprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) netprobe/netprobe.o libc/libc.a

# httpget
httpget/httpget.o: httpget/httpget.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/httpget: $(CRT0_OBJ) httpget/httpget.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) httpget/httpget.o libc/libc.a

# randprobe
randprobe/randprobe.o: randprobe/randprobe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/randprobe: $(CRT0_OBJ) randprobe/randprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) randprobe/randprobe.o libc/libc.a

# fbtest
fbtest/fbtest.o: fbtest/fbtest.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/fbtest: $(CRT0_OBJ) fbtest/fbtest.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) fbtest/fbtest.o libc/libc.a

# evtest
evtest/evtest.o: evtest/evtest.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/evtest: $(CRT0_OBJ) evtest/evtest.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) evtest/evtest.o libc/libc.a

# mousetest
mousetest/mousetest.o: mousetest/mousetest.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/mousetest: $(CRT0_OBJ) mousetest/mousetest.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) mousetest/mousetest.o libc/libc.a

# polltest
polltest/polltest.o: polltest/polltest.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/polltest: $(CRT0_OBJ) polltest/polltest.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) polltest/polltest.o libc/libc.a

# ptytest
ptytest/ptytest.o: ptytest/ptytest.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/ptytest: $(CRT0_OBJ) ptytest/ptytest.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) ptytest/ptytest.o libc/libc.a

# cttytest
cttytest/cttytest.o: cttytest/cttytest.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/cttytest: $(CRT0_OBJ) cttytest/cttytest.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) cttytest/cttytest.o libc/libc.a

# desktop
desktop/desktop.o: desktop/desktop.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/desktop: $(CRT0_OBJ) desktop/desktop.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) desktop/desktop.o libc/libc.a

# wmctl
wmctl/wmctl.o: wmctl/wmctl.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/wmctl: $(CRT0_OBJ) wmctl/wmctl.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) wmctl/wmctl.o libc/libc.a

# gui_demo
gui_demo/gui_demo.o: gui_demo/gui_demo.c
	$(CC) $(CFLAGS) -c $< -o $@

libwm/wm.o: libwm/wm.c include/wm.h
	$(CC) $(CFLAGS) -c $< -o $@

libdraw/draw.o: libdraw/draw.c include/draw.h include/font8x16.h
	$(CC) $(CFLAGS) -c $< -o $@

libdraw/fonts.o: libdraw/fonts.c include/draw.h include/font_ui16.h include/font_ui20.h
	$(CC) $(CFLAGS) -c $< -o $@

libdraw/image.o: libdraw/image.c include/draw.h
	$(CC) $(CFLAGS) -c $< -o $@

libgui/gui.o: libgui/gui.c include/gui.h include/wm.h include/draw.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/gui_demo: $(CRT0_OBJ) gui_demo/gui_demo.o libwm/wm.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) gui_demo/gui_demo.o libwm/wm.o libc/libc.a

# wmevtest
wmevtest/wmevtest.o: wmevtest/wmevtest.c include/wm.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/wmevtest: $(CRT0_OBJ) wmevtest/wmevtest.o libwm/wm.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) wmevtest/wmevtest.o libwm/wm.o libc/libc.a

# uidemo
uidemo/uidemo.o: uidemo/uidemo.c include/gui.h include/wm.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/uidemo: $(CRT0_OBJ) uidemo/uidemo.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) uidemo/uidemo.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

# files
files/files.o: files/files.c include/gui.h include/wm.h include/dirent.h include/sys/stat.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/files: $(CRT0_OBJ) files/files.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) files/files.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

# dmesg
dmesg/dmesg.o: dmesg/dmesg.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/dmesg: $(CRT0_OBJ) dmesg/dmesg.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) dmesg/dmesg.o libc/libc.a


# Phase 14 apps (libgui + libdraw surface apps)
GUIAPP_DEPS := $(CRT0_OBJ) libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

edit/edit.o: edit/edit.c include/gui.h include/draw.h
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/edit: edit/edit.o $(GUIAPP_DEPS)
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) edit/edit.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

calc/calc.o: calc/calc.c include/gui.h include/draw.h
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/calc: calc/calc.o $(GUIAPP_DEPS)
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) calc/calc.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

taskmgr/taskmgr.o: taskmgr/taskmgr.c include/gui.h include/draw.h
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/taskmgr: taskmgr/taskmgr.o $(GUIAPP_DEPS)
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) taskmgr/taskmgr.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

view/view.o: view/view.c include/gui.h include/draw.h
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/view: view/view.o $(GUIAPP_DEPS)
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) view/view.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

maerox/maerox.o: maerox/maerox.c include/gui.h include/draw.h
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/maerox: maerox/maerox.o $(GUIAPP_DEPS)
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) maerox/maerox.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

settings/settings.o: settings/settings.c include/gui.h include/draw.h
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/settings: settings/settings.o $(GUIAPP_DEPS)
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) settings/settings.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

raceprobe/raceprobe.o: raceprobe/raceprobe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/raceprobe: $(CRT0_OBJ) raceprobe/raceprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) raceprobe/raceprobe.o libc/libc.a

extprobe/extprobe.o: extprobe/extprobe.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/extprobe: $(CRT0_OBJ) extprobe/extprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) extprobe/extprobe.o libc/libc.a

store/store.o: store/store.c include/gui.h include/draw.h
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/store: store/store.o $(GUIAPP_DEPS)
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) store/store.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

pkg/pkg.o: pkg/pkg.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/pkg: $(CRT0_OBJ) pkg/pkg.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) pkg/pkg.o libc/libc.a

wavplay/wavplay.o: wavplay/wavplay.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/wavplay: $(CRT0_OBJ) wavplay/wavplay.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) wavplay/wavplay.o libc/libc.a

statprobe/statprobe.o: statprobe/statprobe.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/statprobe: $(CRT0_OBJ) statprobe/statprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) statprobe/statprobe.o libc/libc.a

# threadprobe
threadprobe/threadprobe.o: threadprobe/threadprobe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/threadprobe: $(CRT0_OBJ) threadprobe/threadprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) threadprobe/threadprobe.o libc/libc.a

# shmprobe
shmprobe/shmprobe.o: shmprobe/shmprobe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/shmprobe: $(CRT0_OBJ) shmprobe/shmprobe.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) shmprobe/shmprobe.o libc/libc.a

# term (GUI terminal)
term/term.o: term/term.c include/gui.h include/draw.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/term: $(CRT0_OBJ) term/term.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) term/term.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

# browse (HTTP viewer)
browse/browse.o: browse/browse.c include/gui.h include/draw.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTFILES)/browse: $(CRT0_OBJ) browse/browse.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) browse/browse.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o libwm/wm.o libc/libc.a

clean:
	rm -f libc/*.o libc/libc.a \
	      init/init.o shell/shell.o ls/ls.o \
	      cat/cat.o cp/cp.o rm/rm.o touch/touch.o mkdir_cmd/mkdir.o head/head.o \
	      grep/grep.o wc/wc.o sleep/sleep.o kill_cmd/kill.o test_cmd/test.o \
	      sort/sort.o uniq/uniq.o cut/cut.o printf_cmd/printf.o \
	      env/env.o tee/tee.o tr/tr.o false_cmd/false.o true_cmd/true.o \
	      basename_cmd/basename.o dirname_cmd/dirname.o \
	      sed/sed.o awk/awk.o find_cmd/find.o date_cmd/date.o id_cmd/id.o \
	      ps/ps.o svc/svc.o session/session.o auth/auth.o login/login.o passwd/passwd.o getty/getty.o xargs_cmd/xargs.o diff/diff.o tar_cmd/tar.o \
	      mkfifo_cmd/mkfifo.o ln_cmd/ln.o readlink_cmd/readlink.o \
	      sysprobe/sysprobe.o lspci/lspci.o ifconfig/ifconfig.o \
	      diskprobe/diskprobe.o respawnprobe/respawnprobe.o \
	      sockprobe/sockprobe.o netprobe/netprobe.o httpget/httpget.o randprobe/randprobe.o \
	      fbtest/fbtest.o evtest/evtest.o mousetest/mousetest.o \
	      polltest/polltest.o ptytest/ptytest.o cttytest/cttytest.o \
	      desktop/desktop.o wmctl/wmctl.o gui_demo/gui_demo.o wmevtest/wmevtest.o uidemo/uidemo.o files/files.o \
	      dmesg/dmesg.o \
	      libwm/wm.o libgui/gui.o libdraw/draw.o libdraw/fonts.o libdraw/image.o \
	      $(TESTFILES)/init $(TESTFILES)/shell $(TESTFILES)/ls \
	      $(TESTFILES)/cat $(TESTFILES)/cp $(TESTFILES)/rm \
	      $(TESTFILES)/touch $(TESTFILES)/mkdir $(TESTFILES)/head \
	      $(TESTFILES)/grep $(TESTFILES)/wc $(TESTFILES)/sleep \
	      $(TESTFILES)/kill $(TESTFILES)/test \
	      $(TESTFILES)/sort $(TESTFILES)/uniq $(TESTFILES)/cut $(TESTFILES)/printf \
	      $(TESTFILES)/env $(TESTFILES)/tee $(TESTFILES)/tr \
	      $(TESTFILES)/false $(TESTFILES)/true \
	      $(TESTFILES)/basename $(TESTFILES)/dirname \
	      $(TESTFILES)/sed $(TESTFILES)/awk $(TESTFILES)/find \
	      $(TESTFILES)/date $(TESTFILES)/id $(TESTFILES)/ps $(TESTFILES)/xargs \
	      $(TESTFILES)/svc $(TESTFILES)/session $(TESTFILES)/login $(TESTFILES)/passwd $(TESTFILES)/doas $(TESTFILES)/fwctl $(TESTFILES)/getty \
	      $(TESTFILES)/diff $(TESTFILES)/tar \
	      $(TESTFILES)/mkfifo $(TESTFILES)/ln $(TESTFILES)/readlink \
	      $(TESTFILES)/sysprobe $(TESTFILES)/lspci $(TESTFILES)/ifconfig \
	      $(TESTFILES)/diskprobe $(TESTFILES)/respawnprobe \
	      $(TESTFILES)/sockprobe $(TESTFILES)/netprobe $(TESTFILES)/httpget $(TESTFILES)/randprobe \
	      $(TESTFILES)/fbtest $(TESTFILES)/evtest $(TESTFILES)/mousetest \
	      $(TESTFILES)/polltest $(TESTFILES)/ptytest $(TESTFILES)/cttytest \
	      $(TESTFILES)/desktop $(TESTFILES)/wmctl $(TESTFILES)/gui_demo \
	      $(TESTFILES)/wmevtest $(TESTFILES)/uidemo $(TESTFILES)/files \
	      $(TESTFILES)/dmesg


# ── Phase 28: Linux command parity ──────────────────────────────────────────
reboot_cmd/reboot.o: reboot_cmd/reboot.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/reboot: $(CRT0_OBJ) reboot_cmd/reboot.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) reboot_cmd/reboot.o libc/libc.a
# shutdown/poweroff/halt are the same binary (dispatch on argv[0]).
$(TESTFILES)/shutdown: $(TESTFILES)/reboot
	cp $< $@
$(TESTFILES)/poweroff: $(TESTFILES)/reboot
	cp $< $@
$(TESTFILES)/halt: $(TESTFILES)/reboot
	cp $< $@

sudo_cmd/sudo.o: sudo_cmd/sudo.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/sudo: $(CRT0_OBJ) sudo_cmd/sudo.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) sudo_cmd/sudo.o libc/libc.a

whoami_cmd/whoami.o: whoami_cmd/whoami.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/whoami: $(CRT0_OBJ) whoami_cmd/whoami.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) whoami_cmd/whoami.o libc/libc.a

uname_cmd/uname.o: uname_cmd/uname.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/uname: $(CRT0_OBJ) uname_cmd/uname.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) uname_cmd/uname.o libc/libc.a

clear_cmd/clear.o: clear_cmd/clear.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/clear: $(CRT0_OBJ) clear_cmd/clear.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) clear_cmd/clear.o libc/libc.a

hostname_cmd/hostname.o: hostname_cmd/hostname.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/hostname: $(CRT0_OBJ) hostname_cmd/hostname.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) hostname_cmd/hostname.o libc/libc.a

which_cmd/which.o: which_cmd/which.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/which: $(CRT0_OBJ) which_cmd/which.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) which_cmd/which.o libc/libc.a

free_cmd/free.o: free_cmd/free.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/free: $(CRT0_OBJ) free_cmd/free.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) free_cmd/free.o libc/libc.a

uptime_cmd/uptime.o: uptime_cmd/uptime.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/uptime: $(CRT0_OBJ) uptime_cmd/uptime.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) uptime_cmd/uptime.o libc/libc.a

du_cmd/du.o: du_cmd/du.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/du: $(CRT0_OBJ) du_cmd/du.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) du_cmd/du.o libc/libc.a

df_cmd/df.o: df_cmd/df.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/df: $(CRT0_OBJ) df_cmd/df.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) df_cmd/df.o libc/libc.a

top_cmd/top.o: top_cmd/top.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/top: $(CRT0_OBJ) top_cmd/top.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) top_cmd/top.o libc/libc.a

wget_cmd/wget.o: wget_cmd/wget.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/wget: $(CRT0_OBJ) wget_cmd/wget.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) wget_cmd/wget.o libc/libc.a


# BusyBox applet wrappers: one tiny launcher copied to each applet name in
# /disk/bin so `vi`, `less`, `ping`, etc. resolve on PATH and run busybox.
BB_APPLETS := vi vim more less ping mount umount nano seq yes expr cmp \
              gzip gunzip md5sum tar wget2 watch
bbwrap_cmd/bbwrap.o: bbwrap_cmd/bbwrap.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/bbwrap: $(CRT0_OBJ) bbwrap_cmd/bbwrap.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) bbwrap_cmd/bbwrap.o libc/libc.a
	@mkdir -p $(TESTFILES)/bin
	@for a in $(BB_APPLETS); do cp $@ $(TESTFILES)/bin/$$a; done
	@echo "bbwrap: installed applet wrappers ($(BB_APPLETS))"

# ff — one-command Firefox launcher (starts maeroX + Firefox)
ff/ff.o: ff/ff.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/ff: $(CRT0_OBJ) ff/ff.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) ff/ff.o libc/libc.a

uidtest/uidtest.o: uidtest/uidtest.c
	$(CC) $(CFLAGS) -c $< -o $@
$(TESTFILES)/uidtest: $(CRT0_OBJ) uidtest/uidtest.o libc/libc.a
	$(CC) $(LDFLAGS) -o $@ $(CRT0_OBJ) uidtest/uidtest.o libc/libc.a
