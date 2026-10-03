# The Firefox content sandbox

Firefox 115's child processes (web content, the file:// process, extensions)
run with Firefox's Linux content sandbox at **level 4**. The kernel provides
what that sandbox needs: seccomp-bpf filters, `SECCOMP_FILTER_FLAG_TSYNC`,
and `no_new_privs`. Firefox's file broker (`SandboxBroker`) hands the children
the files they are allowed to open.

What about:support reports on MaeroOS (recorded by `make smoke-firefox`;
see "Evidence" below):

| about:support field            | MaeroOS | Linux host (same build, same profile) |
|--------------------------------|---------|----------------------------------------|
| hasSeccompBPF                  | true    | true  |
| hasSeccompTSync                | true    | true  |
| hasUserNamespaces              | false   | true  |
| hasPrivilegedUserNamespaces    | false   | true  |
| canSandboxContent              | true    | true  |
| canSandboxMedia                | true    | true  |
| contentSandboxLevel            | 4       | 4     |
| effectiveContentSandboxLevel   | 4       | 4     |
| syscallLog (rejected calls)    | empty   | empty |

## How it works

**In the kernel** (`proc/seccomp.c`, `proc/seccomp.h`):

- `prctl(PR_SET_NO_NEW_PRIVS)` and `PR_GET_NO_NEW_PRIVS`. The flag is one-way
  and is kept across fork, clone and execve. While it is set, execve ignores
  set-user-ID and set-group-ID bits.
- `seccomp(2)` (i386 number 354) supports `SECCOMP_SET_MODE_STRICT`,
  `SECCOMP_SET_MODE_FILTER` with the flags `TSYNC`, `TSYNC_ESRCH`, `LOG` and
  `SPEC_ALLOW`, and `SECCOMP_GET_ACTION_AVAIL`.
  `prctl(PR_SET_SECCOMP)` and `PR_GET_SECCOMP` are supported too.
  `NEW_LISTENER` is not supported (no user-space notification), so a
  `USER_NOTIF` result fails the call with `ENOSYS`, as Linux does when no
  listener exists.
- **Validation** happens before a program is accepted and follows Linux's
  rules:
  - 1 to 4096 instructions;
  - only the classic-BPF opcodes that Linux allows for seccomp (no MOD, no
    byte, halfword or indirect loads, no `RET X`);
  - every load is an aligned 32-bit word inside the 64-byte `seccomp_data`;
  - scratch cells are written on every path before they are read;
  - no division by a constant zero and no shift by 32 or more;
  - every jump is forward and lands inside the program;
  - the last instruction is a RET.
  
  Because the program counter only moves forward, a run takes at most `len`
  steps. A division by `X == 0` at run time returns 0 (`KILL_THREAD`), as
  classic BPF does.
- **Evaluation** happens on every `int 0x80` (in `syscall_dispatch`) before
  the call runs. The filter sees a `seccomp_data` with `nr`,
  `arch = AUDIT_ARCH_I386`, the address after `int 0x80`, and the six argument
  registers zero-extended to 64 bits. Every filter in the chain runs, and the
  most restrictive action wins. Between equal actions, the newest filter's
  data wins. The actions are:
  - `ALLOW`, and `LOG` (allow and print `[SECCOMP] ...`);
  - `ERRNO(n)`: n is clamped to 4095;
  - `TRAP(n)`: forced SIGSYS with `si_code = SYS_SECCOMP`, `si_errno = n`,
    `si_call_addr`, `si_syscall` and `si_arch`. If SIGSYS is blocked or
    ignored, the block is lifted and the disposition reset, so the process
    dies. The trap is delivered before any other pending signal. eax still
    holds the syscall number, and whatever the handler leaves in
    `uc_mcontext` eax is the result;
  - `KILL_THREAD`: the thread dies; if it is the last thread, the process dies
    of SIGSYS;
  - `KILL_PROCESS`: the process dies of SIGSYS, and so does any unknown action;
  - `TRACE` and `USER_NOTIF`: the call fails with `ENOSYS`.
- **Filters are permanent.** They are reference-counted chains (`->prev`).
  Fork and clone share the chain, execve keeps it, and nothing removes or
  replaces a filter. TSYNC moves every other thread of the process onto the
  caller's chain only when that thread's chain is an ancestor of it.
  Otherwise TSYNC fails with the thread's tid (or `ESRCH` with
  `TSYNC_ESRCH`), and nothing is installed. Installing a filter without
  `no_new_privs` needs root, else `EACCES`. The path limit is 32768
  instructions counted Linux's way (each filter costs its length plus 4).
- `/proc/<pid>/status` shows `NoNewPrivs:`, `Seccomp:` and
  `Seccomp_filters:`.
- `prctl` now implements `PR_SET_NAME`/`PR_GET_NAME`, `PR_SET_DUMPABLE`/
  `PR_GET_DUMPABLE`, `PR_SET_PDEATHSIG`/`PR_GET_PDEATHSIG` (recorded only),
  `PR_CAPBSET_READ`, and no-ops for the tuning options. Unknown options are
  `EINVAL`. Before this change every option returned 0, which told Firefox's
  `SandboxInfo` that seccomp existed and then did nothing.

**In Firefox** (`userspace/ff/ff.c`, `testfiles/ffprofile/user.js`):

- The `MOZ_DISABLE_{CONTENT,GMP,RDD,SOCKET_PROCESS,UTILITY}_SANDBOX` variables
  are gone. `MOZ_SANDBOX_LOGGING=1` (with `security.sandbox.logging.enabled`)
  sends the broker's policy and denials and any filter violation to the
  serial console as `Sandbox: ...` lines.
- `security.sandbox.content.level` is 4 and `media.cubeb.sandbox` is true.
  Firefox lowers the content level to 3 unless audio is remoted to the
  parent, which is what `media.cubeb.sandbox` does (cubeb runs in the parent
  over audioipc). The RDD and socket processes stay disabled by prefs
  (`media.rdd-process.enabled`, `network.process.enabled`). Their sandboxes
  are no longer disabled, so turning those processes on gets them sandboxed.
- **Level 4 without namespaces.** Firefox's level 4 adds a chroot and
  separate network, PID and IPC namespaces on top of level 3, but only if
  `CLONE_NEWUSER` works (`SandboxLaunch.cpp`: no user namespaces means no
  chroot, no `CLONE_NEW*`). This kernel has no namespaces, so
  `/proc/self/ns/*` is absent and `hasUserNamespaces` is false. Firefox then
  launches the child normally and still applies the level-4 seccomp policy
  and the level-4 broker file policy. What is missing compared with a Linux
  desktop is the chroot and the separate network, PID and IPC namespaces.
- The broker's denials in a normal run are the expected ones:
  `/proc/<pid>/cgroup`, `/proc/stat`, `/sys/devices/system/cpu/{present,
  possible}`. The child falls back (glibc uses `sched_getaffinity` for the
  CPU count), as on Linux.

## Evidence

- `make smoke-abi` runs probe `p45_seccomp` (`ports/abiprobes/p45_seccomp.c`).
  It is checked against the Linux host with `make -C ports/abiprobes
  host-check`. On a non-root host the set-uid part is reported as skipped.
  It covers:
  - Firefox's feature probes (`EFAULT` for a NULL program);
  - the validator's rejections;
  - ALLOW/ERRNO/LOG, irremovability (an ALLOW filter on top does not lift an
    ERRNO), and ERRNO clamping;
  - the TRAP siginfo and ucontext contract, with the handler's result
    returned;
  - TRAP beating ERRNO, and TRAP with SIGSYS blocked;
  - KILL_PROCESS (also from a second thread), KILL_THREAD with one and with
    two threads, and STRICT mode;
  - TSYNC, and TSYNC failing with the tid of a thread that holds another
    filter;
  - inheritance across fork and execve, including `/proc/self/status`;
  - `no_new_privs` ignoring a set-uid bit on execve.
- `ports/firefox/autoconfig/maeros.cfg` is installed into the Firefox tree by
  `fetch-runtime.sh`. It prints about:support's sandbox section as one
  `maeros-sandbox: {...}` line about 8 s after start. `tools/smoke_firefox.py`
  waits for that line and records it in `summary.txt`. The run FAILs if
  Firefox reports seccomp/TSYNC missing, `canSandboxContent` false, an
  effective level of 0, or any rejected syscall.

## Notes

- After changing `testfiles/ffprofile/user.js` or anything under
  `testfiles/firefox`, delete `disk-ff.img`. The `disk-ff` target only
  depends on `firefox-bin`, so the old image would be reused.
- Do not run the runtime's `firefox-bin` on the host with network access.
  The host has no `DisableAppUpdate` policy, so Firefox downloads an update
  into `testfiles/firefox/updates/`, and the next host start tries to apply
  it. Running under `unshare -rn` keeps it offline.

## Audio and page-load cost

Level 4 needs `media.cubeb.sandbox=true`. cubeb then runs in the parent, and
the content process sends it decoded audio over audioipc. A level-3 content
process with cubeb inside it gets no sound at all, because the sandbox keeps
it away from `/dev/snd`. The 3 s tone of `make smoke-firefox
SMOKE_FF_ARGS=--audio` measured on one CPU (KVM):

| configuration                                   | silent 10 ms blocks inside the tone |
|-------------------------------------------------|-------------------------------------|
| sandbox off, cubeb in content (the old setup)   | 0 |
| sandbox off, cubeb remoted                      | 94 |
| level 4, remoted, default latency               | 37, 118 |
| level 4, remoted, `media.cubeb_latency_playback_ms=250`, `audiosink.threshold_ms=500` (what `user.js` sets) | 9 (PASS), 17 (FAIL, limit 10) |
| level 4, remoted, latency 500                   | 9-10 gaps, but only 2.5 s of tone (FAIL) |
| level 4, remoted, default latency, `--smp 2`    | 8 (PASS) |

The gaps come from the audioipc hop, not from seccomp: remoting with the
sandbox off is just as choppy. On one CPU the opt-in audio check is marginal.
With two CPUs it passes.

Page loads do not get noticeably slower (`make smoke-firefox-web`, one CPU,
two runs each):

| | first paint after the launcher started | test page image on screen after Enter |
|---|---|---|
| sandbox off | 11.7 s, 9.8 s | 1.0 s, 1.0 s |
| level 4     | 11.0 s, 10.6 s | 1.1 s, 1.2 s |
