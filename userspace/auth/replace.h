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
 * The umask is set to 077 first, so every file this creates is 0600 from
 * the moment it exists; it is then chowned/chmodded to root:MODE before any
 * data is written, so no other user can ever open it.  The kernel's
 * rename() replaces PATH atomically and keeps TMP's inode, so PATH is
 * root:MODE from the instant it holds the new contents; the post-rename
 * re-lock and compare remain as a check (init also re-locks /etc/shadow at
 * boot).
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

/* Set the process umask to 077 via the raw syscall (libc umask() is a
 * stub).  Call before creating anything that must stay private. */
void maero_strict_umask(void);

/* Force root:root MODE on an open file.  Returns 1 on success. */
int maero_lock_down(int fd, int mode);
