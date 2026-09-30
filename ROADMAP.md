# MaeroOS Roadmap

The goal is not to clone Linux internals; it is to provide enough Linux/POSIX
behaviour that ordinary Unix software runs unmodified. What already works, and
the test that proves it, is in the status table of [README.md](README.md).

## Standing rules

- Keep `make`, `make initrd` and every `make smoke*` target passing.
- Add a regression check to the relevant `tools/smoke*.py` (or a `*probe`
  binary it runs) with every bug fix.
- Touch user memory only through `copy_from_user` / `copy_to_user`.
- Import third-party code only when it matches the kernel ABI and its license
  is recorded in the README's third-party table.

## Open work

- **Firefox first paint.** Firefox 115 ESR starts but stalls before rendering
  (`make smoke-firefox`, [README-BROWSER.md](README-BROWSER.md)); then remove
  the Firefox-specific workarounds listed in `docs/audit/firefox-first-paint.md`.
- **Credentials.** Apply the `open()` mode in the kernel instead of only the
  umask; supplementary groups (`initgroups` is a stub); a separate fsuid.
- **Sockets.** Pass `O_NONBLOCK` and `send` flags (`MSG_NOSIGNAL`) to the
  network layer and raise SIGPIPE on a closed TCP socket.
- **Packages.** Sign the `pkg` index; today only each tarball's SHA-256 is
  checked against it.
- **Console.** Offer a getty on the console while the desktop session runs.
- **Memory protection.** Map ELF segments with their own permissions (W^X).
- **SMP scaling.** Replace the single Big Kernel Lock with finer locking.
- **Distribution.** A deterministic release artifact: kernel ELF, initrd,
  disk image and a documented QEMU command.
