#pragma once

/* Package-safety helpers for pkg, kept free of MaeroOS specifics so the same
 * code can be built and tested on the host (userspace/pkg/test_tarx.c). */

/* A package name is 1-31 of [A-Za-z0-9._-] and starts with a letter or digit,
 * so it is always a single, harmless path component ("..", "a/b" and "" are
 * refused).  Repo tar file names follow the same rule (up to 47 chars). */
int pkg_name_ok(const char *name);
int pkg_file_ok(const char *name);

/* A tar member must be one plain file name: packages are flat, so anything
 * with a '/' (absolute paths, "../x", "a/b") or named "." / ".." is refused. */
int tar_member_ok(const char *name);

/* Extract the ustar archive in data[0..len) into dest_dir.  Every header is
 * checked before anything is written: bad checksum, non-regular entry,
 * unsafe name or truncated data rejects the archive.  Returns the number of
 * files written, or -1 with a message in *why. */
int untar_mem(const char *data, int len, const char *dest_dir, const char **why);
