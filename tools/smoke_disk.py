#!/usr/bin/env python3
import os
import re
import selectors
import subprocess
import sys
import time


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "
TIMEOUT = 25.0


def wait_for(proc, sel, needle, log, timeout=TIMEOUT, start=0):
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


def main():
    proc = subprocess.Popen(
        [
            "qemu-system-i386",
            "-kernel",
            "kernel.elf",
            "-initrd",
            "initrd.tar",
            "-drive",
            "file=disk.img,format=raw,index=0,media=disk",
            "-serial",
            "stdio",
            "-m",
            "128M",
            "-no-reboot",
            "-no-shutdown",
        ],
        cwd=ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    log = []

    try:
        wait_for(proc, sel, PROMPT, log)
        boot_log = "".join(log)
        if "[BOOT] Launching /disk/init" not in boot_log:
            raise AssertionError("disk boot did not launch /disk/init")
        if "[BOOT] failed to launch /disk/init" in boot_log:
            raise AssertionError("disk init failed and boot fell back to initrd")
        if "exec '/disk/getty'" not in boot_log:
            raise AssertionError("disk init did not launch /disk/getty")
        if "MaeroOS getty on tty0" not in boot_log:
            raise AssertionError("disk getty banner did not appear")
        if "login: root" not in boot_log:
            raise AssertionError("disk getty login did not appear")
        if "exec '/disk/login'" not in boot_log:
            raise AssertionError("disk getty did not launch /disk/login")
        if "login: root accepted" not in boot_log:
            raise AssertionError("disk login did not accept root")
        if "exec '/disk/shell'" not in boot_log:
            raise AssertionError("disk login did not launch /disk/shell")
        if "exec '/shell'" in boot_log:
            raise AssertionError("disk init unexpectedly launched initrd /shell")
        if "[init] Running /etc/rc" not in boot_log:
            raise AssertionError("disk init did not run /etc/rc")
        if "[rc] early userspace" not in boot_log:
            raise AssertionError("/etc/rc did not execute")
        if "[init] Reading /etc/inittab" not in boot_log:
            raise AssertionError("disk init did not read /etc/inittab")
        if "[init] Console tty0 respawn /disk/getty tty0" not in boot_log:
            raise AssertionError("disk init did not configure tty0 from inittab")
        if "[init] Reading /etc/services" not in boot_log:
            raise AssertionError("disk init did not read /etc/services")
        if "[init] Starting service bootstamp" not in boot_log:
            raise AssertionError("bootstamp service did not start")
        if "[init] Service bootstamp exited" not in boot_log:
            raise AssertionError("bootstamp service did not exit cleanly")
        if "[init] Starting service heartbeat" not in boot_log:
            raise AssertionError("heartbeat respawn service did not start")
        if "[init] Service sleeper disabled" not in boot_log:
            raise AssertionError("disabled sleeper service did not stay stopped")
        if "[init] Respawning service heartbeat" not in boot_log:
            wait_for(
                proc,
                sel,
                "[init] Respawning service heartbeat",
                log,
                timeout=5.0,
                start=len(boot_log),
            )
        if "[init] Control FIFO ready at /tmp/initctl" not in boot_log:
            raise AssertionError("init control FIFO was not created")
        if "Welcome to MaeroOS" not in boot_log:
            raise AssertionError("/etc/profile was not sourced by the login shell")

        checks = [
            ("env\n", "PATH=/disk:/disk/bin:/:/bin"),
            ("env\n", "HOME=/home/root"),
            ("env\n", "USER=root"),
            ("env\n", "SHELL=/disk/shell"),
            ("cat /etc/passwd\n", "root:x:0:0:root:/home/root:/shell"),
            ("cat /etc/shadow\n", "root:$maero-pbkdf2-sha256$100000$626f6f74303030303030303030303030$8597f18637a4f16874d11a49d32b710d48a6764b6f341819f7eec4dd655b5e2b:0:0:99999:7:::"),
            # Bare `login` prompts on the tty (echo off) instead of wanting
            # the password in argv; an empty answer is rejected.
            ("login\n", "login: authentication failed", None, ("Password: ", "\n")),
            ("login --check root root\n", "login: root password ok"),
            ("login --check root wrong\n", "login: authentication failed"),
            ("login --check missing root\n", "login: unknown user missing"),
            ("passwd root wrong toor\n", "passwd: authentication failed"),
            ("passwd missing root toor\n", "passwd: unknown user missing"),
            ("passwd root root toor\n", "passwd: password updated for root"),
            ("login --check root toor\n", "login: root password ok"),
            ("login --check root root\n", "login: authentication failed"),
            ("cat /etc/shadow\n", "root:$maero-pbkdf2-sha256$100000$"),
            ("cat /etc/shadow\n", ":0:0:99999:7:::"),
            ("passwd root toor root\n", "passwd: password updated for root"),
            ("login --check root root\n", "login: root password ok"),
            ("cat /etc/shadow\n", "root:$maero-pbkdf2-sha256$100000$"),
            ("cat /etc/shadow\n", ":0:0:99999:7:::"),
            # passwd must leave shadow root:root 0600 (it used to become 0644).
            ("busybox ls -ln /etc/shadow\n", re.compile(r"-rw------- +1 +0 +0 ")),
            ("busybox ls -ln /etc/shadow.tmp\n", "No such file", "-rw"),
            # The previous shadow is kept as a root-only backup.
            ("busybox ls -ln /etc/shadow-\n", re.compile(r"-rw------- +1 +0 +0 ")),
            # A caller's lax umask must not loosen what passwd creates.
            ("busybox sh -c 'umask 0; /disk/passwd root root umask0'\n", "passwd: password updated for root"),
            ("busybox ls -ln /etc/shadow\n", re.compile(r"-rw------- +1 +0 +0 ")),
            ("busybox ls -ln /etc/shadow-\n", re.compile(r"-rw------- +1 +0 +0 ")),
            ("busybox sh -c 'umask 0; /disk/passwd root umask0 root'\n", "passwd: password updated for root"),
            ("busybox ls -ln /etc/shadow\n", re.compile(r"-rw------- +1 +0 +0 ")),
            ("login --check root root\n", "login: root password ok"),
            # The shipped default user password is hashed, and the documented
            # login (user/user) still works.
            ("cat /etc/shadow\n", "user:$maero-pbkdf2-sha256$100000$"),
            ("cat /etc/shadow\n", "MaeroOS$ ", "user:user:"),
            ("login --check user user\n", "login: user password ok"),
            # Locked ("!") and empty-hash accounts, one with an empty GECOS
            # field: neither the lock marker nor the next shadow field is
            # accepted as a plaintext password.
            ("printf 'locked:x:1001:100::/home/user:/disk/shell\\nempty:x:1002:100::/home/user:/disk/shell\\n' >> /etc/passwd\n", "MaeroOS$ "),
            ("printf 'locked:!:0:0:99999:7:::\\nempty::0:0:99999:7:::\\n' >> /etc/shadow\n", "MaeroOS$ "),
            ("login --check locked !\n", "login: authentication failed"),
            ("login --check locked x\n", "login: authentication failed"),
            ("login locked !\n", "login: authentication failed", "accepted"),
            ("login --check empty 0\n", "login: authentication failed"),
            # login drops to the target user before starting the shell.
            ("login user user\n", "login: user accepted"),
            ("id\n", "uid=1000 gid=100"),
            ("pwd\n", "/home/user"),
            ("cat /etc/shadow\n", "cat: cannot open file", "$maero"),
            ("cat /etc/shadow-\n", "cat: cannot open file", "$maero"),
            # /disk/etc is root-only: the user cannot add files to it.
            ("printf pwned > /etc/owned\n", "MaeroOS$ "),
            ("cat /etc/owned\n", "cat: cannot open file", "pwned"),
            # doas never runs a same-named program from the cwd: plant an
            # executable ./ls (a copy of id) and check doas runs the real ls.
            ("cp /disk/id /home/user/ls\n", "MaeroOS$ "),
            ("busybox chmod 755 /home/user/ls\n", "MaeroOS$ "),
            ("./ls\n", "uid=1000 gid=100"),
            ("doas ls\n", "README.txt", "uid=", ("password for user", "user\n")),
            # doas becomes root in both uid and gid.
            ("doas id\n", "uid=0 gid=0", None, ("password for user", "user\n")),
            ("doas id\n", "doas: authentication failed", "uid=0", ("password for user", "wrong\n")),
            ("rm /home/user/ls\n", "MaeroOS$ "),
            ("exit\n", "MaeroOS$ "),
            ("id\n", "uid=0 gid=0"),
            ("cat /etc/inittab\n", "tty0 respawn /disk/getty tty0"),
            ("cat /tmp/sessions.status\n", "ID ACTION PID STATE COMMAND"),
            ("cat /tmp/sessions.status\n", "tty0 respawn"),
            ("cat /tmp/sessions.status\n", "running /disk/getty tty0"),
            ("session\n", "ID ACTION PID STATE COMMAND"),
            ("session\n", "tty0 respawn"),
            ("session status\n", "running /disk/getty tty0"),
            ("session nope\n", "session: usage: session [status]"),
            ("ls /disk\n", "hello.txt"),
            ("cat /disk/etc/os-release\n", "NAME=MaeroOS"),
            ("cat /etc/os-release\n", "NAME=MaeroOS"),
            ("cat /home/root/boot-state\n", "booted"),
            ("cat /disk/home/root/boot-state\n", "booted"),
            ("cat /home/root/service-state\n", "serviced"),
            ("cat /disk/home/root/service-state\n", "serviced"),
            ("cat /home/root/respawn-first\n", "first"),
            ("cat /home/root/respawn-alive\n", "alive"),
            ("svc\n", "heartbeat respawn"),
            ("svc\n", "running /disk/respawnprobe"),
            ("svc\n", "sleeper respawn disabled - stopped /disk/sleep 60"),
            ("cat /tmp/services.status\n", "heartbeat respawn enabled"),
            ("cat /tmp/services.status\n", "sleeper respawn disabled -1 stopped /disk/sleep 60"),
            ("svc enable sleeper\n", "started sleeper"),
            ("svc\n", "sleeper respawn enabled"),
            ("svc\n", "running /disk/sleep 60"),
            ("cat /tmp/services.status\n", "sleeper respawn enabled"),
            ("svc disable sleeper\n", "stopped sleeper"),
            ("svc\n", "sleeper respawn disabled - stopped /disk/sleep 60"),
            ("cat /tmp/services.status\n", "sleeper respawn disabled -1 stopped /disk/sleep 60"),
            ("cat /etc/services\n", "respawn sleeper disabled /disk/sleep 60"),
            ("svc start missing\n", "svc: unknown respawn service missing"),
            ("svc restart heartbeat\n", "restarted heartbeat"),
            ("svc\n", "running /disk/respawnprobe"),
            ("svc stop heartbeat\n", "stopped heartbeat"),
            ("svc\n", "stopped /disk/respawnprobe"),
            ("svc start heartbeat\n", "started heartbeat"),
            ("svc\n", "running /disk/respawnprobe"),
            ("svc disable heartbeat\n", "stopped heartbeat"),
            ("svc\n", "heartbeat respawn disabled - stopped /disk/respawnprobe"),
            ("cat /etc/services\n", "respawn heartbeat disabled /disk/respawnprobe"),
            ("svc enable heartbeat\n", "started heartbeat"),
            ("svc\n", "heartbeat respawn enabled"),
            ("svc\n", "running /disk/respawnprobe"),
            ("cat /etc/services\n", "respawn heartbeat enabled /disk/respawnprobe"),
            # svc enable/disable must keep every line of a long /etc/services
            # (it used to drop everything after line 32).
            ("printf '" + "#\\n" * 40 + "# svc-tail-marker\\n' >> /etc/services\n", "MaeroOS$ "),
            ("svc disable heartbeat\n", "stopped heartbeat"),
            ("cat /etc/services\n", "# svc-tail-marker"),
            ("cat /etc/services\n", "respawn heartbeat disabled /disk/respawnprobe"),
            ("svc enable heartbeat\n", "started heartbeat"),
            ("cat /etc/services\n", "# svc-tail-marker"),
            ("busybox ls /etc/services.tmp\n", "No such file"),
            ("cat /etc/services\n", "respawn heartbeat enabled /disk/respawnprobe"),
            ("cat /disk/hello.txt\n", "Hello from MaeroOS initrd!"),
            ("diskprobe\n", "diskprobe ok"),
            ("randprobe\n", "randprobe getrandom ok"),
            ("randprobe\n", "randprobe urandom ok"),
            ("cat /disk/hello.txt\n", "DISK"),
            ("printf root > /overlay.txt\n", "MaeroOS$ "),
            ("cat /disk/overlay.txt\n", "root"),
            ("rm /overlay.txt\n", "MaeroOS$ "),
            ("cat /disk/overlay.txt\n", "cat: cannot open file"),
            ("touch /disk/new.txt\n", "MaeroOS$ "),
            ("printf saved > /disk/new.txt\n", "MaeroOS$ "),
            ("cat /disk/new.txt\n", "saved"),
            ("rm /home/root/boot-state\n", "MaeroOS$ "),
            ("rm /home/root/service-state\n", "MaeroOS$ "),
            ("rm /home/root/respawn-first\n", "MaeroOS$ "),
            ("rm /home/root/respawn-alive\n", "MaeroOS$ "),
            ("rm /home/root\n", "MaeroOS$ "),
            ("mkdir /home/root\n", "MaeroOS$ "),
            ("printf note > /home/note.txt\n", "MaeroOS$ "),
            ("cat /disk/home/note.txt\n", "note"),
            ("rm /home/note.txt\n", "MaeroOS$ "),
            ("cat /disk/home/note.txt\n", "cat: cannot open file"),
            ("mkdir /home/root\n", "MaeroOS$ "),
            ("mkdir /home/root/a\n", "MaeroOS$ "),
            ("cd /home/root/a\n", "MaeroOS$ "),
            ("pwd\n", "/home/root/a"),
            ("cd ..\n", "MaeroOS$ "),
            ("pwd\n", "/home/root"),
            ("cd /home/root/..\n", "MaeroOS$ "),
            ("pwd\n", "/home"),
            ("cd /\n", "MaeroOS$ "),
            ("cd ..\n", "MaeroOS$ "),
            ("pwd\n", "/"),
            ("cat /etc/os-release\n", "NAME=MaeroOS"),
            ("cat /dev/null\n", "MaeroOS$ "),
            ("cat /proc/processes\n", "PID"),
            ("cat /var/log/init.log\n", "=== MaeroOS init boot ==="),
            ("cat /var/log/init.log\n", "session tty0 started"),
            ("cat /disk/var/log/init.log\n", "=== MaeroOS init boot ==="),
            # Disk boot is far more verbose than the ring size, so the oldest
            # [BOOT] lines are evicted by the time dmesg runs (base smoke already
            # proves [BOOT] capture). Here just confirm dmesg reads recent log.
            ("dmesg\n", "[SYSCALL]"),
            ("rm /home/root/a\n", "MaeroOS$ "),
            # /home now persists on disk (holds the unprivileged user's home,
            # /home/user — Phase 24).  Confirm it survives and the user home
            # is present rather than forbidding "home".
            ("ls /disk\n", "home"),
            ("ls /disk/home\n", "user"),
        ]

        for check in checks:
            command, expected = check[0], check[1]
            forbidden = check[2] if len(check) > 2 else None
            reply = check[3] if len(check) > 3 else None
            before = len("".join(log))
            send(proc, command)
            if reply:
                # Interactive step: answer a prompt (e.g. a password).
                wait_for(proc, sel, reply[0], log, start=before)
                send(proc, reply[1])
            wait_for(proc, sel, PROMPT, log, start=before)
            recent = "".join(log)[before:]
            if isinstance(expected, re.Pattern):
                if not expected.search(recent):
                    raise AssertionError(
                        f"command {command.strip()!r} did not match {expected.pattern!r}"
                    )
            elif expected not in recent:
                raise AssertionError(
                    f"command {command.strip()!r} did not produce {expected!r}"
                )
            if forbidden and forbidden in recent:
                raise AssertionError(
                    f"command {command.strip()!r} unexpectedly produced {forbidden!r}"
                )
            if command == "svc restart heartbeat\n":
                if "[init] Restarting service heartbeat" not in recent:
                    wait_for(proc, sel, "[init] Restarting service heartbeat", log, timeout=5.0, start=before)
                after_restart = "".join(log)[before:]
                if "[init] Respawning service heartbeat" not in after_restart:
                    wait_for(proc, sel, "[init] Respawning service heartbeat", log, timeout=5.0, start=before)

        print("\n[SMOKE-DISK] passed")
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
        print(f"\n[SMOKE-DISK] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
