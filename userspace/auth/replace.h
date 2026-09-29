#pragma once

/*
 * Replace PATH (a root-owned config file) with NEW_DATA without ever losing
 * it, whether or not the kernel's rename() is atomic.
 *
 * OLD_DATA must be the file's current contents.  If BACKUP is set, OLD_DATA
 * is first written there (verified) so a crash at any point leaves a good
 * copy.  NEW_DATA goes to TMP (verified), TMP is renamed over PATH, and then
 * PATH itself is checked: owner/mode are forced to root:MODE and the
 * contents compared.  If PATH is missing or holds anything else, OLD_DATA
 * is written back.  TMP is only removed once PATH is known to be good.
 *
 * Every file is chowned/chmodded before any data is written to it, so
 * nothing this writes is readable beyond MODE -- except, on a kernel whose
 * rename() recreates the target with a default mode, for the moment between
 * rename() and the re-check.
 *
 * Returns MAERO_REPLACED, MAERO_UNCHANGED (failed, PATH holds OLD_DATA), or
 * MAERO_LOST (failed and PATH could not be restored; TMP and BACKUP kept).
 */
#define MAERO_REPLACED   0
#define MAERO_UNCHANGED  1
#define MAERO_LOST      -1

int maero_replace_file(const char *path, const char *tmp, const char *backup,
                       const char *old_data, int old_len,
                       const char *new_data, int new_len, int mode);

/* Force root:root MODE on an open file.  Returns 1 on success. */
int maero_lock_down(int fd, int mode);
