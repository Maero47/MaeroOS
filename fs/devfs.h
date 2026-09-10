#pragma once
#include "vfs.h"

/*
 * devfs — virtual device filesystem.
 *
 * Provides three character-device nodes:
 *   /dev/null  — reads return 0, writes are discarded
 *   /dev/zero  — reads return 0x00 bytes, writes are discarded
 *   /dev/tty   — reads/writes via COM1 serial port
 *
 * Returns a directory vfs_node_t representing the /dev root.
 */
vfs_node_t *devfs_mount(void);

/* One byte from the serial console, sleeping (not spinning) while the line is
 * idle — see the comment on the definition in devfs.c. */
char console_serial_getc(void);

/* Read/write the TTY's struct termios (36 bytes, Linux i386 layout) */
void tty_get_termios(void *buf);
void tty_set_termios(const void *buf);

/* Terminal foreground process group (for TIOCGPGRP / TIOCSPGRP) */
extern int tty_fg_pgrp;

/* Hang up and detach the controlling terminal for a session. */
void devfs_session_tty_hangup(int sid, vfs_node_t *tty);
