#pragma once

#include "process.h"

/*
 * Advisory file locks, kept in one list (proc/flock.c):
 *
 *   flock(2)          whole-file, owned by the open file description (the
 *                     descriptor's fid, shared by every dup/fork/SCM copy);
 *   fcntl F_SETLK...  POSIX byte ranges, owned by the descriptor table (Linux
 *                     fl_owner = current->files), so threads share them and
 *                     fork does not inherit them;
 *   fcntl F_OFD_*     byte ranges owned by the open file description.
 *
 * flock locks never conflict with the fcntl kinds (Linux keeps them apart);
 * POSIX and OFD locks conflict with each other.  Locks are keyed by the
 * file (st_dev, st_ino and its filesystem), not by the vfs node a
 * descriptor happens to hold.
 */

/* flock(fd, op): LOCK_SH 1, LOCK_EX 2, LOCK_NB 4, LOCK_UN 8. */
int flock_bsd(proc_file_t *f, int op);

/* fcntl lock commands on `f`: F_GETLK/F_SETLK/F_SETLKW (5/6/7, struct
 * flock with a 32-bit off_t), their 64-bit forms (12/13/14) and
 * F_OFD_GETLK/SETLK/SETLKW (36/37/38, struct flock64).  `uarg` is the user
 * pointer to the structure. */
int flock_fcntl(proc_file_t *f, int cmd, void *uarg);

/* A descriptor is being closed (fd_release): drop the closing table's
 * POSIX locks on its file, and the flock/OFD locks of its open file
 * description once this was the description's last descriptor. */
void flock_fd_closed(proc_file_t *f);

/* The descriptor table `owner` is going away: drop its POSIX locks. */
void flock_owner_gone(void *owner);
