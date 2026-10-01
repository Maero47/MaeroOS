# Limine (bootloader for `make limine-iso`)

Upstream: https://github.com/limine-bootloader/limine. Licence: BSD-2-Clause
(see `LICENSE`, copied verbatim from the release).

The binaries are not committed. `tools/fetch-limine.sh` fetches the
`v11.4.1-binary` tag tarball, verifies it against a pinned sha256
(`82c0653d97b02af122a385028b4a5f334db51f440c0b94a1e7d29ee6532bf202`), and
unpacks `limine-bios.sys`, `limine-bios-cd.bin`, `limine-uefi-cd.bin`,
`BOOTX64.EFI`, `BOOTIA32.EFI` and `limine.c` into `bin/` (gitignored). It then
builds the `limine` host tool from `limine.c`. See `docs/boot.md`.
