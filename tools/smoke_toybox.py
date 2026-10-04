#!/usr/bin/env python3
import os
import selectors
import subprocess
import sys
import time

import smokelib


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROMPT = "MaeroOS$ "
TIMEOUT = 25.0
DISP_DFL = "disp: INT=D QUIT=D PIPE=D TSTP=D TTIN=D TTOU=D"


def wait_for(proc, sel, needle, log, timeout=TIMEOUT, start=0):
    return smokelib.wait_for(proc, sel, needle, log, timeout, start)


def send(proc, text):
    smokelib.send(proc, text)


def main():
    # pkg's tar/name and index-signature checks, the libc regex engine, the
    # e1000 TX ring bookkeeping, the r8169 driver core (against a simulated
    # chip), the virtio ring logic (against a simulated device) and the USB
    # HID descriptor parser (against hostile descriptors) and the USB stick
    # transport (against a simulated stick with short transfers) are plain
    # C: exercise them on the host first.
    for test in ("test_pkg_tarx.py", "test_pkg_sign.py", "test_regex.py",
                 "test_e1000_tx.py", "test_r8169.py", "test_virtqueue.py",
                 "test_usb_hid.py", "test_usb_msc.py"):
        if subprocess.run([sys.executable,
                           os.path.join(ROOT, "tools", test)]).returncode:
            raise AssertionError(f"tools/{test} failed")

    proc = subprocess.Popen(
        ["make", "run"] + smokelib.MAKE_DISPLAY,
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
        smokelib.login(proc, sel, log)
        checks = [
            ("toybox echo TOYBOX_OK\n", "TOYBOX_OK"),
            ("toybox cat hello.txt\n", "Hello from MaeroOS initrd!"),
            ("toybox pwd\n", "/"),
            ("toybox basename /tmp/example.txt\n", "example.txt"),
            ("toybox dirname /tmp/example.txt\n", "/tmp"),
            ("toybox printf %d 90\n", "90"),
            ("toybox head -n 1 hello.txt\n", "Hello from MaeroOS initrd!"),
            ("toybox wc hello.txt\n", "hello.txt"),
            ("toybox ls\n", "hello.txt"),
            ("toybox cut -c 1-5 hello.txt\n", "Hello"),
            ("toybox sort hello.txt\n", "Hello from MaeroOS initrd!"),
            ("toybox uniq hello.txt\n", "Hello from MaeroOS initrd!"),
            ("toybox date -u -d @0 +%Y-%m-%dT%H\n", "1970-01-01T00"),
            # One line per applet enabled by the libc additions (regex, libm,
            # float printf, sscanf, time, *at() calls, rlimits, statfs...).
            ("toybox echo hello | toybox sed s/l/L/g\n", "heLLo"),
            ("toybox grep -c Hello hello.txt\n", "1"),
            # 4 KB lines through sed (it always asks regexec for submatches,
            # and takes any error as "no match").  4095 spaces, x, a space:
            # the only match of ' *$' is the last space, after 4 K failing
            # start positions, which used to exhaust the step cap.
            ("toybox printf \"%4096s \\n\" x > /tmp/sxs; toybox sed 's/ *$/_OK/;s/x/SED_TRIM/' /tmp/sxs | toybox tr -d ' '\n",
             "SED_TRIM_OK"),
            ("toybox printf \"%4096s\\n\" x > /tmp/sx; toybox sed 's/ *$//;s/.*foo/Y/;s/x$/SED_LONG_/;s/_$/_OK/' /tmp/sx | toybox tr -d ' '\n",
             "SED_LONG_OK"),
            ("toybox printf \"%4096sfoo!\\n\" x | toybox sed 's/.*foo/SED_/;s/!/FOO_OK/'\n", "SED_FOO_OK"),
            ("toybox egrep -o \"M[a-z]+OS\" hello.txt\n", "MaeroOS"),
            ("toybox fgrep -x \"Hello from MaeroOS initrd!\" hello.txt\n", "Hello from MaeroOS"),
            ("toybox find /etc -name \"pass*\" -type f\n", "/etc/passwd"),
            ("toybox echo a b | toybox xargs toybox echo X\n", "X a b"),
            ("toybox echo hello | toybox tr a-z A-Z\n", "HELLO"),
            ("toybox echo 3 4 | toybox awk '{print $1*$2}'\n", "12"),
            # Two output pipes open at once: closing the first must not wait on
            # the second child, which (before popen closed the parent's other
            # pipe ends in the child) held the first pipe's write end open.
            ("toybox awk 'BEGIN{print \"P1\" | \"toybox cat\"; print \"P2\" | \"toybox cat\"; "
             "print \"b\" | \"toybox sort\"; close(\"toybox cat\"); close(\"toybox sort\"); "
             "print \"AWK_PIPES_OK\"}'\n", "AWK_PIPES_OK"),
            ("toybox expr 6 \\* 7\n", "42"),
            ("toybox echo 2^10 | toybox bc -q\n", "1024"),
            ("toybox factor 360\n", "360: 2 2 2 3 3 5"),
            ("toybox printf \"%.2f\\n\" 3.14159\n", "3.14"),
            ("toybox echo xa xb xb xc | toybox tsort | toybox paste -s -d ,\n", "xa,xb,xc"),
            ("toybox shuf -e SHUF_OK\n", "SHUF_OK"),
            ("toybox md5sum hello.txt\n", "847bdd77800a12e3"),
            ("toybox sha1sum hello.txt\n", "ea27489bfc036952"),
            ("toybox sha224sum hello.txt\n", "c6d229b93cd131e0"),
            ("toybox sha256sum hello.txt\n", "a97a93676d7d2b09"),
            ("toybox sha384sum hello.txt\n", "14c0be10db8655a8"),
            ("toybox sha512sum hello.txt\n", "9155bafb08440e00"),
            ("toybox stat -c %s hello.txt\n", "27"),
            ("toybox echo hello > /tmp/t1; toybox cp /tmp/t1 /tmp/t2; toybox mv /tmp/t2 /tmp/t3; toybox cat /tmp/t3\n", "hello"),
            ("toybox chown 1000:1000 /tmp/t3; toybox chgrp 0 /tmp/t3; toybox stat -c %u:%g /tmp/t3\n", "1000:0"),
            ("toybox install -m 600 /tmp/t1 /tmp/t4; toybox stat -c %a /tmp/t4\n", "600"),
            ("toybox dd if=/tmp/t1 of=/tmp/t5 bs=1 count=3; toybox cat /tmp/t5\n", "hel"),
            ("toybox diff /tmp/t1 /tmp/t5\n", "-hello"),
            ("toybox fallocate -l 4096 /tmp/fa; toybox stat -c %s /tmp/fa\n", "4096"),
            ("toybox fsync /tmp/t1; echo rc=$?\n", "rc=0"),
            ("toybox sync; echo rc=$?\n", "rc=0"),
            ("toybox flock 0 < /tmp/t1; echo rc=$?\n", "rc=0"),
            ("toybox mktemp /tmp/mt.XXXXXX\n", "/tmp/mt."),
            ("cd /tmp; toybox echo t1 | toybox cpio -o -H newc > a.cpio; toybox cpio -t < a.cpio; cd /\n", "t1"),
            ("toybox tar -czf /tmp/b.tgz -C /tmp t1; toybox tar -tzf /tmp/b.tgz\n", "t1"),
            ("toybox gzip -c /tmp/t1 > /tmp/t1.gz; toybox zcat /tmp/t1.gz\n", "hello"),
            ("toybox printf \"\\xfd\\x37\\x7a\\x58\\x5a\\x00\\x00\\x01\\x69\\x22\\xde\\x36\\x04\\xc0\\x0a\\x06\\x21\\x01\\x1c\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x63\\xa0\\xac\\xb1\\x01\\x00\\x05\\x58\\x5a\\x5f\\x4f\\x4b\\x0a\\x00\\x00\\x00\\x8a\\x56\\x4c\\x38\\x00\\x01\\x22\\x06\\x3e\\x56\\x57\\x6e\\x90\\x42\\x99\\x0d\\x01\\x00\\x00\\x00\\x00\\x01\\x59\\x5a\" | toybox xzcat\n", "XZ_OK"),
            ("toybox printf \"\\x42\\x5a\\x68\\x39\\x31\\x41\\x59\\x26\\x53\\x59\\x5d\\x5e\\xf0\\x60\\x00\\x00\\x00\\xc6\\x00\\x00\\x10\\x10\\x08\\x80\\x10\\xa0\\x00\\x21\\x80\\x0c\\x01\\xa9\\xae\\xe2\\xee\\x48\\xa7\\x0a\\x12\\x0b\\xab\\xde\\x0c\\x00\" | toybox bzcat\n", "BZ_OK"),
            ("toybox printf \"\\x42\\x5a\\x68\\x39\\x31\\x41\\x59\\x26\\x53\\x59\\x5d\\x5e\\xf0\\x60\\x00\\x00\\x00\\xc6\\x00\\x00\\x10\\x10\\x08\\x80\\x10\\xa0\\x00\\x21\\x80\\x0c\\x01\\xa9\\xae\\xe2\\xee\\x48\\xa7\\x0a\\x12\\x0b\\xab\\xde\\x0c\\x00\" > /tmp/z.bz2; toybox bunzip2 -c /tmp/z.bz2\n", "BZ_OK"),
            ("toybox seq 1 6 > /tmp/s6; cd /tmp; toybox csplit -f cs s6 4; toybox paste -s -d , cs01; cd /\n", "4,5,6"),
            ("toybox echo ABC > /tmp/abc; toybox hexdump -C /tmp/abc\n", "|ABC.|"),
            ("toybox readelf -h /toybox\n", "ELF32"),
            ("toybox echo MORE_OK | toybox more\n", "MORE_OK"),
            ("toybox logger LOGGER_OK\n", "LOGGER_OK"),
            ("toybox kill -l 15\n", "TERM"),
            ("toybox nice -n 5 toybox echo NICE_OK\n", "NICE_OK"),
            ("toybox renice -n 1 1; echo rc=$?\n", "rc=0"),
            ("toybox ps -o pid,comm -p 1\n", "init"),
            ("toybox pgrep -l init\n", "1 init"),
            ("toybox top -b -n 1 -m 3\n", "Tasks:"),
            ("toybox free -m\n", "Mem:"),
            ("toybox uptime\n", "load average"),
            ("toybox nproc > /tmp/np; toybox grep -c \"^[1-9]\" /tmp/np\n", "1"),
            ("toybox taskset -p 1\n", "affinity mask"),
            ("toybox ulimit -n\n", "512"),
            ("toybox hostid\n", "007f0100"),
            ("toybox tty\n", "/dev/"),
            ("toybox time toybox true\n", "real"),
            ("toybox dmesg | toybox grep -o \"Launching /init\"\n", "Launching /init"),
            ("toybox timeout 1 toybox sleep 5; echo rc=$?\n", "rc=124"),
            ("toybox timeout 1 toybox yes YES_OK | toybox tail -n 1\n", "YES_OK"),
            ("toybox timeout 4 toybox watch -x -e -n 1 toybox false\n", "Exit status 1"),
            ("sleep 50 &\n", PROMPT),
            ("toybox pkill sleep; echo rc=$?\n", "rc=0"),
            ("toybox sleep 0\n", PROMPT),
            ("toybox sleep 1\n", PROMPT),
            ("toybox uname\n", "Linux"),
            ("toybox whoami\n", "root"),
            ("toybox id\n", "uid=0"),
            # Syscall-layer regressions: long relative paths, symlink loops,
            # offsets, getdents layouts, waitpid(WUNTRACED).
            ("sysmiscprobe\n", "sysmiscprobe ok"),
            # Creation modes, O_APPEND, supplementary groups, access(),
            # TIOCSPGRP, unlink(dir) (userspace/abi2probe).
            ("abi2probe\n", "abi2probe ok"),
            # Signal state across fork/clone/execve: SIG_IGN, the blocked mask
            # and pending signals survive exec, handlers reset
            # (userspace/sigexecprobe).
            ("sigexecprobe\n", "sigexecprobe ok"),
            # SA_RESETHAND/SA_NODEFER, process-wide pending signals across
            # threads, a leader's exit and a non-leader's execve
            # (userspace/sigshareprobe).
            ("sigshareprobe\n", "sigshareprobe ok"),
            # The shell ignores INT/QUIT/PIPE/TSTP/TTIN/TTOU for itself;
            # SIG_IGN survives exec, so every kind of child must get SIG_DFL
            # back or ^C/^Z could not reach a job.
            ("sigshareprobe disp\n", DISP_DFL),
            ("sigshareprobe disp | cat\n", DISP_DFL),
            ("sigshareprobe disp & sleep 1\n", DISP_DFL),
            ("echo $(sigshareprobe disp)\n", DISP_DFL),
            # alarm/setitimer/POSIX timers deliver signals that interrupt
            # blocking calls (EINTR / SA_RESTART); fork clears, exec keeps
            # (userspace/timerprobe).
            ("timerprobe\n", "timerprobe ok"),
            ("whoami\n", "root"),
            # libc regression checks (userspace/libctest)
            ("libctest\n", "LIBCTEST PASS"),
            # a path instead of a package name must be refused outright
            ("pkg remove ../etc\n", "invalid package name '../etc'"),
        ]
        for command, expected in checks:
            before = len("".join(log))
            send(proc, command)
            wait_for(proc, sel, PROMPT, log, start=before)
            recent = "".join(log)[before:]
            body = recent.split("\n", 1)[1] if "\n" in recent else recent
            if "error" in body.lower():
                raise AssertionError(f"command {command.strip()!r} reported an error")
            if "FAIL:" in body:
                raise AssertionError(f"command {command.strip()!r} reported a failure")
            if expected not in body:
                raise AssertionError(
                    f"command {command.strip()!r} did not produce {expected!r}"
                )

        # ^Z stops a foreground job and gives the prompt back; `jobs` shows it
        # stopped; `fg` resumes it (its interrupted read restarts) and ^D ends
        # it.  cat is used because the serial console only turns ^Z into
        # SIGTSTP for a reader.
        before = len("".join(log))
        send(proc, "cat\n")
        wait_for(proc, sel, "cat' pid=", log, start=before)
        time.sleep(1.0)
        before = len("".join(log))
        send(proc, "\x1a")
        wait_for(proc, sel, PROMPT, log, start=before)
        before = len("".join(log))
        send(proc, "jobs\n")
        wait_for(proc, sel, PROMPT, log, start=before)
        if "Stopped" not in "".join(log)[before:]:
            raise AssertionError("^Z did not stop the foreground job")
        before = len("".join(log))
        send(proc, "fg\n")
        time.sleep(1.0)
        send(proc, "\x04")
        wait_for(proc, sel, PROMPT, log, start=before)
        before = len("".join(log))
        send(proc, "jobs\n")
        wait_for(proc, sel, PROMPT, log, start=before)
        if "Stopped" in "".join(log)[before:] or "Running" in "".join(log)[before:]:
            raise AssertionError("the resumed job did not finish")

        # The same for a pipeline: ^Z must stop every stage (the console
        # signals its foreground process group), the shell must record the
        # job as soon as one stage reports stopped, and `fg` must resume and
        # wait for the whole group — wc then counts what cat read.
        before = len("".join(log))
        send(proc, "cat | wc -c\n")
        wait_for(proc, sel, "wc' pid=", log, start=before)
        time.sleep(1.0)
        before = len("".join(log))
        send(proc, "\x1a")
        wait_for(proc, sel, PROMPT, log, start=before)
        before = len("".join(log))
        send(proc, "jobs\n")
        wait_for(proc, sel, PROMPT, log, start=before)
        if "Stopped" not in "".join(log)[before:]:
            raise AssertionError("^Z did not stop the foreground pipeline")
        before = len("".join(log))
        send(proc, "fg\n")
        time.sleep(1.0)
        send(proc, "abcdefghij\n")
        time.sleep(0.5)
        send(proc, "\x04")
        wait_for(proc, sel, PROMPT, log, start=before)
        if "11" not in "".join(log)[before:]:
            raise AssertionError("the resumed pipeline did not finish with wc's count")

        # Log out of the console shell while a background job of its session
        # lives on; init starts a new getty, and the next login's shell runs
        # in a new session.  The old leader's exit must free the console so
        # the new shell gets it (job control, the foreground group) instead
        # of the orphaned job.
        before = len("".join(log))
        send(proc, "sleep 60 &\n")
        wait_for(proc, sel, PROMPT, log, start=before)
        before = len("".join(log))
        send(proc, "exit\n")
        wait_for(proc, sel, "exited; restarting", log, start=before)
        smokelib.login(proc, sel, log, start=before)
        before = len("".join(log))
        send(proc, "abi2probe tty\n")
        wait_for(proc, sel, PROMPT, log, start=before)
        if "abi2probe tty ok" not in "".join(log)[before:]:
            raise AssertionError("console was not freed when its session leader exited")

        print("\n[SMOKE-TOYBOX] passed")
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
        print(f"\n[SMOKE-TOYBOX] failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
