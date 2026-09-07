# Linux-ABI probes

Twenty small C programs, one per probe of `docs/audit/firefox-first-paint.md`
section 8.  Each proves or disproves one kernel-semantics gap found by the
audit and prints exactly one final line:

    PASS <name>
    FAIL <name>: <what was observed>
    SKIP <name>: <why it cannot run here>

The binaries are static musl i386 executables.  Run on an x86_64 Linux host
that executes i386 ELF files they print `PASS`: that is the reference
behaviour.  Run on MaeroOS under QEMU (`make smoke-abi`) they are the
regression tests for the fixes in audit section 5.

## Probe to file mapping

| Probe | File | Audit findings | Section 5 item |
|---|---|---|---|
| P1  | `p01_fatal_signal_scope.c`  | RC1, S2 | 1 |
| P2  | `p02_spawn_exit_group.c`    | RC2, C1, S12 | 2 |
| P3  | `p03_sigchld_thread.c`      | RC3, S3, S4, S11, C2 | 2, 3 |
| P4  | `p04_spurious_futex.c`      | RC4, F1, F3, F11, S1, S8 | 4, 5, 6 |
| P5  | `p05_stale_wake_tick.c`     | F4 (T4/E1 as triggers) | 4 |
| P6  | `p06_mmap_prot_madvise.c`   | M1, M6, M4 | 9 |
| P7  | `p07_ftruncate64.c`         | syscalls 194, 193, 297, 40 | 8 |
| P8  | `p08_select_timeout.c`      | E2 | 7 |
| P9  | `p09_poll_eintr_restart.c`  | E1, S5 | 7, 14 |
| P10 | `p10_unix_socket.c`         | U2, U3, U4, U5 | 7, 11 |
| P11 | `p11_addr_space_reuse.c`    | M3 | 9 |
| P12 | `p12_clocks.c`              | T1, T2, T4 | 12 |
| P13 | `p13_futex_timeout.c`       | F2, T3 | 4 |
| P14 | `p14_exec_arg_size.c`       | C4 | 10 |
| P15 | `p15_socket_cloexec.c`      | C5 | 10 |
| P16 | `p16_memfd_cloexec_size.c`  | C5, M5 | 10, 13 |
| P17 | `p17_signal_busy_thread.c`  | S2, S4 | 1 |
| P18 | `p18_high_memory.c`         | M10 (M8 timing printed) | 13 |
| P19 | `p19_siginfo.c`             | S6 | 14 |
| P20 | `p20_shared_futex.c`        | F5, F6 | 14 |
| P21 | `p21_sigsuspend.c`          | review round 1, finding 1 | - |
| P22 | `p22_mprotect_cow.c`        | review round 1, finding 3 (M1) | 9 |

Every source starts with a comment that names the findings, states the Linux
behaviour it asserts with a kernel/libc source reference, and quotes the
MaeroOS behaviour the audit describes.

P21 and P22 do not come from the audit: they were added with the fixes for
the review of the merged kernel (`.yonet/fix1.md`), so that the sigsuspend
livelock and the `mprotect(PROT_READ)` hole cannot come back unnoticed.  P12
gained the `SA_RESTART` sleep cases for the same reason (round 2).

Two deviations from the audit text, both explained in the sources:

- P9: Linux never restarts `poll()` after a handled signal, even with
  `SA_RESTART` (man 7 signal).  The probe asserts `EINTR` for `poll` in both
  cases and uses `read()` on a pipe for the `SA_RESTART` restart check (S5).
- P17 additionally checks that `kill(getpid(), sig)` reaches a thread that
  does not block the signal (S4), since it already has a spinning thread.

## Building

Toolchain: musl.cc `i686-linux-musl-cross` (sha512 in
<https://musl.cc/SHA512SUMS>), either on `PATH`, under `/opt` (the ports
Docker image) or under `~/opt`.  Override with `MUSL_PREFIX=...`.

    make abiprobes                 # from the repo root, or
    make -C ports/abiprobes        # same thing

Outputs go to `testfiles/abiprobes/` (gitignored), which the initrd picks up
as `/abiprobes/`.  The build flags are `-O2 -Wall -Wextra -static -no-pie`
and the sources compile warning free.

musl differences worth knowing when comparing with a glibc build:

- `posix_spawn` uses `clone(CLONE_VM|CLONE_VFORK|SIGCHLD)`, glibc 2.36 uses
  `clone3` with the same flags.  Both exit the child with `exit_group`, so P2
  exercises the same kernel path either way (C1).
- musl 1.2 has a 64-bit `time_t`; raw `SYS_futex` calls therefore build the
  kernel's old 32-bit `timespec` by hand (`struct kernel_old_timespec` in
  `probe.h`).
- glibc's condvar internals (BZ#25847) are not modelled; P4 exercises the raw
  futex behaviour underneath them.

## Running

On the host (Linux reference; every line must be PASS):

    make -C ports/abiprobes host-check

On MaeroOS under QEMU:

    make smoke-abi                          # boots with -m 1024M
    python3 tools/smoke_abi.py --mem 2048M  # the audit's second P18 case
    python3 tools/smoke_abi.py --only p05,p13

P18 sizes its allocation from the QEMU memory (`mem - 324 MiB`, clamped to
64..1400), so `-m 1024M` runs the audit's 700 MiB case and `-m 2048M` a
genuinely larger 1400 MiB one.

`tools/smoke_abi.py` runs each probe from the shell over the serial console,
reads the verdict line and prints a summary `N pass, M xfail, K unexpected
fail, X xpass, S skip, H hang, R not run, V no verdict`.  Probes that the
audit expects to fail on the current kernel are listed in `XFAIL` inside the
script, so the target is green today; when a kernel fix flips one to PASS it
is reported as XPASS and its entry must be removed to make it required
(`--strict` turns XPASS into a failure).  P18 and P20 pass on the current
kernel and are required; the other eighteen are listed in `XFAIL`.

An `XFAIL` entry is satisfied only by a probe that ran and printed
`FAIL <name>: ...`.  A probe that printed no verdict line, that wedged the
guest shell, or that never ran is a hard failure whether or not it is listed:
those mean the harness itself is broken (binaries missing from the initrd,
exec failure, a crash before any output, a kernel wedge) and would otherwise
be indistinguishable from the expected ABI failures.  After a wedge the driver
kills QEMU, reboots and continues with the remaining probes.

Each probe arms an internal watchdog thread that prints a `FAIL` line and
exits, so a hung syscall still returns the shell prompt instead of wedging
the guest:

| Probes | Watchdog | Driver timeout |
|---|---|---|
| P1-P10, P12-P15, P17, P19, P20 | 60 s | 90 s |
| P11, P16 | 120 s | 150 s |
| P18 | `120 + MiB/2` s (470 s at 700 MiB) | watchdog + 30 s |

The driver derives its per-probe timeout as watchdog + 30 s, so the watchdog
always fires first, and it verifies the watchdog values in this table against
the `probe_watchdog()` calls in the sources before every run (a mismatch
aborts the run). P16 and P18 accept a size in MiB as `argv[1]`.
