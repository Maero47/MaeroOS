#pragma once
#include <stdint.h>

/*
 * The repo index (tools/mkrepo.py) is signed with Ed25519:
 *
 *   #maeros-index v1 serial=<N>
 *   name|version|...            (one line per package)
 *   #sig ed25519 <128 hex chars>
 *
 * The signature covers every byte before the "#sig" line, header included.
 * The serial only grows (mkrepo uses the build time), so pkg can refuse an
 * index older than the one it already has.
 *
 * index_verify returns 0 and sets *serial and *signed_len (bytes covered by
 * the signature, i.e. where the "#sig" line starts) if buf[0..n) is a
 * well-formed index signed by pk; otherwise -1 with *why set.
 */
int index_verify(const char *buf, int n, const uint8_t pk[32],
                 unsigned long *serial, int *signed_len, const char **why);
