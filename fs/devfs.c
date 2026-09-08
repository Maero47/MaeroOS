#include "devfs.h"
#include "../drivers/ac97.h"
#include "vfs.h"
#include "tmpfs.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/serial.h"
#include "../kernel/random.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "../proc/signal.h"
#include <stddef.h>
#include <stdint.h>

/* ── /dev/null ─────────────────────────────────────────────────────────────── */

static uint32_t null_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    (void)n; (void)off; (void)len; (void)buf;
    return 0;   /* EOF immediately */
}

static int always_ready(vfs_node_t *n) {
    (void)n;
    return 1;
}

static uint32_t null_write(vfs_node_t *n, uint32_t off, uint32_t len,
                            const uint8_t *buf) {
    (void)n; (void)off; (void)buf;
    return len; /* discard, pretend all bytes consumed */
}

/* ── /dev/zero ─────────────────────────────────────────────────────────────── */

static uint32_t zero_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    (void)n; (void)off;
    for (uint32_t i = 0; i < len; i++) buf[i] = 0;
    return len;
}

/* write is same as /dev/null — discard */

/* ── TTY termios state ──────────────────────────────────────────────────────── */

/*
 * We store a minimal set of termios flags.
 * c_lflag bits we honour:
 *   ICANON  (0x02) — line-buffered mode; if clear, raw (one byte at a time)
 *   ECHO    (0x08) — echo input to output
 * Default: ICANON | ECHO (matches Linux defaults).
 */
#define TERMIOS_ICANON  0x00000002
#define TERMIOS_ECHO    0x00000008
#define TERMIOS_ISIG    0x00000001
#define TERMIOS_ECHOE   0x00000010
#define TERMIOS_TOSTOP  0x00000100
#define TERMIOS_ICRNL   0x00000100
#define TERMIOS_OPOST   0x00000001
#define TERMIOS_ONLCR   0x00000004
#define TCGETS          0x5401
#define TCSETS          0x5402
#define TCSETSW         0x5403
#define TCSETSF         0x5404
#define TIOCGWINSZ      0x5413
#define TIOCGPGRP       0x5414
#define TIOCSPGRP       0x5415
#define TIOCSCTTY       0x540E
#define VINTR_IDX       0
#define VERASE_IDX      2
#define VKILL_IDX       3
#define VEOF_IDX        4
#define VSUSP_IDX       10
#define PTY_LINE_SIZE   512

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
} tty_termios_t;

static tty_termios_t tty_termios = {
    .c_iflag = TERMIOS_ICRNL,
    .c_oflag = TERMIOS_OPOST | TERMIOS_ONLCR,
    .c_cflag = 0x00000030 | 0x00000080 | 0x00000800,
    .c_lflag = TERMIOS_ISIG | TERMIOS_ICANON | TERMIOS_ECHO | TERMIOS_ECHOE,
    /* Standard c_cc defaults (Posix) */
    .c_cc = {
        [0]  = 3,   /* VINTR  = ^C */
        [1]  = 28,  /* VQUIT  = ^\ */
        [2]  = 127, /* VERASE = DEL */
        [3]  = 21,  /* VKILL  = ^U */
        [4]  = 4,   /* VEOF   = ^D */
        [7]  = 0,   /* VSTART */
        [8]  = 0,   /* VSTOP  */
        [9]  = 26,  /* VSUSP  = ^Z */
        [10] = 26,  /* VSUSP (alternate) */
    },
};

/* Called by sys_ioctl for TCGETS */
void tty_get_termios(void *buf) {
    __builtin_memcpy(buf, &tty_termios, sizeof(tty_termios_t));
}

/* Called by sys_ioctl for TCSETS/TCSETSW/TCSETSF */
void tty_set_termios(const void *buf) {
    __builtin_memcpy(&tty_termios, buf, sizeof(tty_termios_t));
}

/* ── /dev/tty — serial console ─────────────────────────────────────────────── */

/* Terminal foreground process group (0 = unset, use shell's pgrp) */
int tty_fg_pgrp = 0;

static vfs_node_t *proc_ctty_node(void) {
    if (current_proc && current_proc->ctty)
        return current_proc->ctty;
    return NULL;
}

static int pgrp_in_current_session(int pgrp) {
    if (!current_proc || pgrp <= 0) return 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (ptable[i].state != PROC_UNUSED &&
            ptable[i].pgrp == pgrp &&
            ptable[i].sid == current_proc->sid)
            return 1;
    }
    return 0;
}

static uint32_t tty_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    (void)n; (void)off;
    vfs_node_t *ctty = proc_ctty_node();
    if (ctty && ctty->read_fn)
        return ctty->read_fn(ctty, off, len, buf);
    if (!len) return 0;

    int icanon = (tty_termios.c_lflag & TERMIOS_ICANON) != 0;
    int do_echo = (tty_termios.c_lflag & TERMIOS_ECHO)   != 0;

    uint32_t i = 0;
    while (i < len) {
        char c = serial_getc();
        if (c == '\r') c = '\n';

        /* ^C — SIGINT (always, regardless of termios) */
        if (c == 0x03) {
            if (current_proc) {
                if (do_echo) { serial_putc('^'); serial_putc('C'); serial_putc('\n'); }
                signal_send(current_proc, SIGINT);
            }
            return 0;
        }

        /* ^Z — SIGTSTP (always) */
        if (c == 0x1A) {
            if (current_proc) {
                if (do_echo) { serial_putc('^'); serial_putc('Z'); serial_putc('\n'); }
                signal_send(current_proc, SIGTSTP);
            }
            return 0;
        }

        /* ^D — EOF (canonical mode only) */
        if (icanon && c == 0x04)
            return i;

        /* Backspace (canonical mode only) */
        if (icanon && (c == '\b' || c == tty_termios.c_cc[2] /* VERASE */)) {
            if (i > 0) {
                i--;
                if (do_echo) { serial_putc('\b'); serial_putc(' '); serial_putc('\b'); }
            }
            continue;
        }

        /* ^U — kill line (canonical mode) */
        if (icanon && c == 0x15) {
            if (do_echo) { while (i-- > 0) { serial_putc('\b'); serial_putc(' '); serial_putc('\b'); } }
            i = 0;
            continue;
        }

        if (do_echo) serial_putc(c);
        buf[i++] = (uint8_t)c;

        /* In canonical mode, return on newline */
        if (icanon && c == '\n') break;

        /* In raw mode, return each byte immediately */
        if (!icanon) break;
    }
    return i;
}

static uint32_t tty_write(vfs_node_t *n, uint32_t off, uint32_t len,
                           const uint8_t *buf) {
    (void)n; (void)off;
    vfs_node_t *ctty = proc_ctty_node();
    if (ctty && ctty->write_fn)
        return ctty->write_fn(ctty, off, len, buf);
    for (uint32_t i = 0; i < len; i++)
        serial_putc((char)buf[i]);
    return len;
}


/* ── Linux virtual-terminal / console ioctls (links2, fbdev apps) ────────
 * MaeroOS has one console; we accept mode changes and report VT 0 active.
 * struct vt_mode  { char mode, waitv; short relsig, acqsig, frsig; }
 * struct vt_stat  { unsigned short v_active, v_signal, v_state; }
 */
int console_vt_ioctl(uint32_t req, void *arg) {
    switch (req) {
    case 0x5601: {                       /* VT_GETMODE */
        if (!arg) return -14;
        memset(arg, 0, 6);               /* mode = VT_AUTO */
        return 0;
    }
    case 0x5602:                         /* VT_SETMODE */
        return 0;
    case 0x5603: {                       /* VT_GETSTATE */
        uint16_t *st = (uint16_t *)arg;
        if (!st) return -14;
        st[0] = 0;                       /* v_active = console 0 */
        st[1] = 0;
        st[2] = 1;
        return 0;
    }
    case 0x5605:                         /* VT_RELDISP */
    case 0x5606:                         /* VT_ACTIVATE */
    case 0x5607:                         /* VT_WAITACTIVE */
        return 0;
    case 0x4B3A:                         /* KDSETMODE (TEXT/GRAPHICS) */
        return 0;
    case 0x4B3B: {                       /* KDGETMODE */
        int *m = (int *)arg;
        if (m) *m = 0;                   /* KD_TEXT */
        return 0;
    }
    case 0x4B44: {                       /* KDGKBMODE */
        int *m = (int *)arg;
        if (m) *m = 0x02;                /* K_XLATE */
        return 0;
    }
    case 0x4B45:                         /* KDSKBMODE */
        return 0;
    case 0x4B32:                         /* KDSETLED */
    case 0x4B46:                         /* KDGKBMETA */
        return 0;
    }
    return -25;                          /* ENOTTY: not a VT request */
}

static int tty_ioctl(vfs_node_t *n, uint32_t req, void *arg) {
    (void)n;
    {
        int vr = console_vt_ioctl(req, arg);
        if (vr != -25) return vr;
    }
    vfs_node_t *ctty = proc_ctty_node();
    if (ctty && ctty->ioctl_fn)
        return ctty->ioctl_fn(ctty, req, arg);
    if (req == TCGETS) {
        if (!arg) return -14;
        tty_get_termios(arg);
        return 0;
    }
    if (req == TCSETS || req == TCSETSW || req == TCSETSF) {
        if (!arg) return -14;
        tty_set_termios(arg);
        return 0;
    }
    if (req == TIOCGWINSZ) {
        uint16_t *ws = (uint16_t *)arg;
        if (!ws) return -14;
        ws[0] = 25;
        ws[1] = 80;
        ws[2] = 0;
        ws[3] = 0;
        return 0;
    }
    if (req == TIOCGPGRP) {
        if (!arg) return -14;
        *(int *)arg = tty_fg_pgrp ? tty_fg_pgrp :
                      (current_proc ? current_proc->pgrp : 1);
        return 0;
    }
    if (req == TIOCSPGRP) {
        if (!arg) return -14;
        int pgrp = *(int *)arg;
        if (!pgrp_in_current_session(pgrp)) return -3;
        tty_fg_pgrp = pgrp;
        return 0;
    }
    return -25;
}

/* ── /dev/dsp — AC97 PCM out (48kHz S16LE stereo), blocking writes ───────── */

static uint32_t dsp_write(vfs_node_t *n, uint32_t off, uint32_t len,
                          const uint8_t *buf) {
    (void)n;
    (void)off;
    int r = ac97_write(buf, len);
    return r < 0 ? 0 : (uint32_t)r;
}

static uint32_t dsp_read(vfs_node_t *n, uint32_t off, uint32_t len,
                         uint8_t *buf) {
    (void)n; (void)off; (void)len; (void)buf;
    return 0;   /* no capture */
}

static vfs_node_t dev_dsp;

/* ── /dev/urandom — kernel best-effort pseudo-random bytes ────────────────── */

static uint32_t urandom_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    (void)n; (void)off;
    random_get_bytes(buf, len);
    return len;
}

/* ── Static device nodes ───────────────────────────────────────────────────── */

static vfs_node_t dev_null;
static vfs_node_t dev_zero;
static vfs_node_t dev_tty;
static vfs_node_t dev_urandom;
static vfs_node_t dev_fb0;
static vfs_node_t dev_input_dir;
static vfs_node_t dev_input_event0;
static vfs_node_t dev_input_event1;
static vfs_node_t dev_dir;   /* the /dev directory itself */

/* ── PTY master/slave pairs ──────────────────────────────────────────────── */

#define MAX_PTYS 8
#define PTY_BUF_SIZE 1024
#define TIOCGPTN 0x80045430U
#define TIOCSPTLCK 0x40045431U

typedef struct {
    int used;
    int id;
    int master_refs;
    int slave_refs;
    int master_closed;
    int slave_closed;
    uint8_t m2s[PTY_BUF_SIZE];
    uint8_t s2m[PTY_BUF_SIZE];
    uint8_t canon[PTY_LINE_SIZE];
    uint32_t m2s_head, m2s_count;
    uint32_t s2m_head, s2m_count;
    uint32_t canon_count;
    int eof_pending;
    tty_termios_t termios;
    int sid;
    int fg_pgrp;
    vfs_node_t master;
    vfs_node_t slave;
} pty_pair_t;

static pty_pair_t ptys[MAX_PTYS];
static vfs_node_t dev_ptmx;
static vfs_node_t dev_pts_dir;
static vfs_node_t *dev_shm_root;   /* tmpfs mounted at /dev/shm (POSIX shm_open) */

static uint32_t pty_buf_read(pty_pair_t *p, int from_slave,
                             uint32_t len, uint8_t *buf) {
    uint8_t *ring = from_slave ? p->s2m : p->m2s;
    uint32_t *head = from_slave ? &p->s2m_head : &p->m2s_head;
    uint32_t *count = from_slave ? &p->s2m_count : &p->m2s_count;
    uint32_t n = 0;

    /* POSIX read semantics: wait until at least one byte is available (or
     * EOF/signal), then return whatever is buffered, up to len. */
    while (*count == 0) {
        if (!from_slave && p->eof_pending) {
            p->eof_pending = 0;
            return n;
        }
        if ((from_slave && p->slave_closed) ||
            (!from_slave && p->master_closed))
            return n;
        /* Abort the wait when a deliverable signal is pending so the
         * process can be killed/stopped instead of sleeping forever. */
        if (signal_interrupt_pending(current_proc))
            return n;
        sleep_on(p);
    }
    uint32_t take = len;
    if (take > *count) take = *count;
    for (uint32_t i = 0; i < take; i++) {
        buf[n++] = ring[*head];
        *head = (*head + 1) % PTY_BUF_SIZE;
    }
    *count -= take;
    wake_up(p);
    io_wake();
    return n;
}

static uint32_t pty_buf_write(pty_pair_t *p, int to_slave,
                              uint32_t len, const uint8_t *buf) {
    uint8_t *ring = to_slave ? p->m2s : p->s2m;
    uint32_t *head = to_slave ? &p->m2s_head : &p->s2m_head;
    uint32_t *count = to_slave ? &p->m2s_count : &p->s2m_count;
    uint32_t n = 0;

    while (n < len) {
        if ((to_slave && p->slave_closed) ||
            (!to_slave && p->master_closed))
            return n;
        while (*count == PTY_BUF_SIZE) {
            if ((to_slave && p->slave_closed) ||
                (!to_slave && p->master_closed))
                return n;
            if (signal_interrupt_pending(current_proc))
                return n;
            sleep_on(p);
        }
        uint32_t tail = (*head + *count) % PTY_BUF_SIZE;
        ring[tail] = buf[n++];
        (*count)++;
        wake_up(p);
        io_wake();
    }
    return n;
}

static void pty_send_pgrp_signal(pty_pair_t *p, int sig) {
    int pg = p->fg_pgrp;
    if (!pg && current_proc) pg = current_proc->pgrp;
    /* One signal per process in the group, delivered to a thread that does
     * not block it (Linux kill_pgrp), not one per thread. */
    signal_send_pgrp(pg, sig);
}

static int pty_background_current(pty_pair_t *p) {
    int fg = p->fg_pgrp;
    if (!fg || !current_proc) return 0;
    return current_proc->pgrp != fg;
}

static void pty_echo(pty_pair_t *p, const char *s, uint32_t len) {
    if (!(p->termios.c_lflag & TERMIOS_ECHO)) return;
    (void)pty_buf_write(p, 0, len, (const uint8_t *)s);
}

static void pty_commit_canon(pty_pair_t *p) {
    if (!p->canon_count) return;
    (void)pty_buf_write(p, 1, p->canon_count, p->canon);
    p->canon_count = 0;
}

static uint32_t pty_master_write_input(pty_pair_t *p,
                                       uint32_t len, const uint8_t *buf) {
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if ((p->termios.c_iflag & TERMIOS_ICRNL) && c == '\r')
            c = '\n';

        if ((p->termios.c_lflag & TERMIOS_ISIG) &&
            c == p->termios.c_cc[VINTR_IDX]) {
            p->canon_count = 0;
            pty_echo(p, "^C\n", 3);
            pty_send_pgrp_signal(p, SIGINT);
            continue;
        }
        if ((p->termios.c_lflag & TERMIOS_ISIG) &&
            c == p->termios.c_cc[VSUSP_IDX]) {
            p->canon_count = 0;
            pty_echo(p, "^Z\n", 3);
            pty_send_pgrp_signal(p, SIGTSTP);
            continue;
        }

        if (!(p->termios.c_lflag & TERMIOS_ICANON)) {
            (void)pty_buf_write(p, 1, 1, &c);
            pty_echo(p, (const char *)&c, 1);
            continue;
        }

        if (c == p->termios.c_cc[VEOF_IDX]) {
            if (p->canon_count)
                pty_commit_canon(p);
            else {
                p->eof_pending = 1;
                wake_up(p);
            }
            continue;
        }
        if (c == '\b' || c == 127 || c == p->termios.c_cc[VERASE_IDX]) {
            if (p->canon_count) {
                p->canon_count--;
                pty_echo(p, "\b \b", 3);
            }
            continue;
        }
        if (c == p->termios.c_cc[VKILL_IDX]) {
            while (p->canon_count) {
                p->canon_count--;
                pty_echo(p, "\b \b", 3);
            }
            continue;
        }

        if (p->canon_count + 1 >= PTY_LINE_SIZE)
            pty_commit_canon(p);
        p->canon[p->canon_count++] = c;
        pty_echo(p, (const char *)&c, 1);
        if (c == '\n')
            pty_commit_canon(p);
    }
    return len;
}

static uint32_t pty_slave_write_output(pty_pair_t *p,
                                       uint32_t len, const uint8_t *buf) {
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if ((p->termios.c_oflag & TERMIOS_OPOST) &&
            (p->termios.c_oflag & TERMIOS_ONLCR) && c == '\n') {
            uint8_t crlf[2] = { '\r', '\n' };
            (void)pty_buf_write(p, 0, 2, crlf);
        } else {
            (void)pty_buf_write(p, 0, 1, &c);
        }
    }
    return len;
}

static uint32_t pty_master_read(vfs_node_t *n, uint32_t off,
                                uint32_t len, uint8_t *buf) {
    (void)off;
    return pty_buf_read((pty_pair_t *)n->private, 1, len, buf);
}

static uint32_t pty_master_write(vfs_node_t *n, uint32_t off,
                                 uint32_t len, const uint8_t *buf) {
    (void)off;
    return pty_master_write_input((pty_pair_t *)n->private, len, buf);
}

static uint32_t pty_slave_read(vfs_node_t *n, uint32_t off,
                               uint32_t len, uint8_t *buf) {
    (void)off;
    pty_pair_t *p = (pty_pair_t *)n->private;
    if (pty_background_current(p)) {
        signal_send(current_proc, SIGTTIN);
        return (uint32_t)-4;
    }
    return pty_buf_read(p, 0, len, buf);
}

static uint32_t pty_slave_write(vfs_node_t *n, uint32_t off,
                                uint32_t len, const uint8_t *buf) {
    (void)off;
    pty_pair_t *p = (pty_pair_t *)n->private;
    if ((p->termios.c_lflag & TERMIOS_TOSTOP) && pty_background_current(p)) {
        signal_send(current_proc, SIGTTOU);
        return (uint32_t)-4;
    }
    return pty_slave_write_output(p, len, buf);
}

static int pty_master_ready(vfs_node_t *n) {
    pty_pair_t *p = (pty_pair_t *)n->private;
    return p && (p->s2m_count > 0 || p->slave_closed);
}

static int pty_slave_ready(vfs_node_t *n) {
    pty_pair_t *p = (pty_pair_t *)n->private;
    return p && (p->m2s_count > 0 || p->eof_pending || p->master_closed);
}

static int pty_write_ready(vfs_node_t *n) {
    pty_pair_t *p = (pty_pair_t *)n->private;
    if (!p) return 0;
    if (n == &p->master) return !p->slave_closed && p->m2s_count < PTY_BUF_SIZE;
    return !p->master_closed && p->s2m_count < PTY_BUF_SIZE;
}

int console_vt_ioctl(uint32_t req, void *arg);

static int pty_ioctl(vfs_node_t *n, uint32_t req, void *arg) {
    {
        int vr = console_vt_ioctl(req, arg);
        if (vr != -25) return vr;
    }
    pty_pair_t *p = (pty_pair_t *)n->private;
    if (!p) return -22;
    if (req == TIOCGPTN) {
        *(int *)arg = p->id;
        return 0;
    }
    if (req == TIOCSPTLCK)
        return 0;
    if (req == TCGETS) {
        if (!arg) return -14;
        __builtin_memcpy(arg, &p->termios, sizeof(p->termios));
        return 0;
    }
    if (req == TCSETS || req == TCSETSW || req == TCSETSF) {
        if (!arg) return -14;
        __builtin_memcpy(&p->termios, arg, sizeof(p->termios));
        return 0;
    }
    if (req == TIOCGWINSZ) {
        uint16_t *ws = (uint16_t *)arg;
        if (!ws) return -14;
        ws[0] = 25;
        ws[1] = 80;
        ws[2] = 0;
        ws[3] = 0;
        return 0;
    }
    if (req == TIOCGPGRP) {
        if (!arg) return -14;
        *(int *)arg = p->fg_pgrp ? p->fg_pgrp :
                      (current_proc ? current_proc->pgrp : 1);
        return 0;
    }
    if (req == TIOCSPGRP) {
        if (!arg) return -14;
        int pgrp = *(int *)arg;
        if (!current_proc || current_proc->ctty != &p->slave)
            return -25;
        if (p->sid && p->sid != current_proc->sid)
            return -1;
        if (!pgrp_in_current_session(pgrp))
            return -3;
        p->fg_pgrp = pgrp;
        return 0;
    }
    if (req == TIOCSCTTY) {
        if (!current_proc) return -25;
        if (n != &p->slave) return -25;
        if (current_proc->sid != current_proc->pid) return -1;
        if (current_proc->ctty && current_proc->ctty != &p->slave) return -1;
        if (p->sid && p->sid != current_proc->sid) return -1;
        if (current_proc->ctty != &p->slave) {
            current_proc->ctty = &p->slave;
            vfs_retain(current_proc->ctty);
        }
        p->sid = current_proc->sid;
        if (!p->fg_pgrp)
            p->fg_pgrp = current_proc->pgrp;
        return 0;
    }
    return -25;
}

static void pty_maybe_free(pty_pair_t *p) {
    if (!p || p->master_refs > 0 || p->slave_refs > 0) return;
    memset(p, 0, sizeof(*p));
}

void devfs_session_tty_hangup(int sid, vfs_node_t *tty) {
    if (sid <= 0 || !tty) return;

    pty_pair_t *p = (pty_pair_t *)tty->private;
    if (p && p->used && tty == &p->slave) {
        if (p->fg_pgrp) {
            pty_send_pgrp_signal(p, SIGHUP);
            pty_send_pgrp_signal(p, SIGCONT);
        }
        p->sid = 0;
        p->fg_pgrp = 0;
    }

    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *proc = &ptable[i];
        if (proc->state == PROC_UNUSED) continue;
        if (proc->sid != sid || proc->ctty != tty) continue;
        proc->ctty = NULL;
        vfs_close(tty);
    }
}

static void pty_master_retain(vfs_node_t *n) {
    pty_pair_t *p = (pty_pair_t *)n->private;
    if (p && p->used) p->master_refs++;
}

static void pty_slave_retain(vfs_node_t *n) {
    pty_pair_t *p = (pty_pair_t *)n->private;
    if (!p || !p->used) return;
    /* The first holder of the slave is what clears the hangup the master sees,
     * not the lookup that found the node: ptsdir_finddir() used to clear it,
     * so a bare stat("/dev/pts/N") on a slave nobody had open made the master's
     * read/poll wait instead of reporting hangup, with no holder left to set it
     * back.  pty_slave_close() sets it again when the last holder goes. */
    if (p->slave_refs == 0) p->slave_closed = 0;
    p->slave_refs++;
}

static void pty_master_close(vfs_node_t *n) {
    pty_pair_t *p = (pty_pair_t *)n->private;
    if (!p || !p->used) return;
    if (p->master_refs > 0) p->master_refs--;
    if (p->master_refs == 0) {
        if (p->fg_pgrp) {
            pty_send_pgrp_signal(p, SIGHUP);
            pty_send_pgrp_signal(p, SIGCONT);
        }
        p->master_closed = 1;
    }
    wake_up(p);
    pty_maybe_free(p);
}

static void pty_slave_close(vfs_node_t *n) {
    pty_pair_t *p = (pty_pair_t *)n->private;
    if (!p || !p->used) return;
    if (p->slave_refs > 0) p->slave_refs--;
    if (p->slave_refs == 0) p->slave_closed = 1;
    wake_up(p);
    pty_maybe_free(p);
}

/* Allocate a master/slave pair and return the master node.
 *
 * INVARIANT: this reserves one of the MAX_PTYS slots, so it must only ever run
 * on behalf of a descriptor that is definitely about to hold it.  It is
 * therefore reached only through dev_ptmx.open_fn (see vfs.h), never from
 * devdir_finddir(): a lookup that does not become an open — stat(), access(),
 * execve(), or an open() that fails its permission check or runs out of
 * descriptors — must reserve nothing.  It used to be called from the lookup,
 * and because pty_maybe_free() is only reachable from the close paths, eight
 * stat("/dev/ptmx") calls exhausted the table for the life of the boot. */
static vfs_node_t *pty_alloc_master(void) {
    for (int i = 0; i < MAX_PTYS; i++) {
        pty_pair_t *p = &ptys[i];
        if (p->used) continue;
        memset(p, 0, sizeof(*p));
        p->used = 1;
        p->id = i;
        /* The descriptor that this open is feeding takes the reference itself
         * (vfs_retain -> pty_master_retain), the same way every other
         * filesystem's nodes are referenced.  Pre-taking it here as well
         * double-counted once open() started retaining, and the pair was never
         * freed: ptytest ran out of PTYs at round 7. */
        p->master_refs = 0;
        p->slave_closed = 1;
        p->termios = tty_termios;

        memset(&p->master, 0, sizeof(p->master));
        strncpy(p->master.name, "ptmx", 255);
        p->master.flags = VFS_FLAG_CHARDEV;
        p->master.inode = 100 + (uint32_t)i * 2;
        p->master.read_fn = pty_master_read;
        p->master.write_fn = pty_master_write;
        p->master.ioctl_fn = pty_ioctl;
        p->master.read_ready_fn = pty_master_ready;
        p->master.write_ready_fn = pty_write_ready;
        p->master.retain_fn = pty_master_retain;
        p->master.close_fn = pty_master_close;
        p->master.private = p;

        memset(&p->slave, 0, sizeof(p->slave));
        p->slave.name[0] = (char)('0' + i);
        p->slave.name[1] = 0;
        p->slave.flags = VFS_FLAG_CHARDEV;
        p->slave.inode = 101 + (uint32_t)i * 2;
        p->slave.read_fn = pty_slave_read;
        p->slave.write_fn = pty_slave_write;
        p->slave.ioctl_fn = pty_ioctl;
        p->slave.read_ready_fn = pty_slave_ready;
        p->slave.write_ready_fn = pty_write_ready;
        p->slave.retain_fn = pty_slave_retain;
        p->slave.close_fn = pty_slave_close;
        p->slave.private = p;
        return &p->master;
    }
    return NULL;
}

/* dev_ptmx.open_fn — /dev/ptmx is a cloning device: the descriptor gets a fresh
 * master, the lookup does not.  See the open_fn comment in fs/vfs.h. */
static vfs_node_t *ptmx_open(vfs_node_t *n) {
    (void)n;
    return pty_alloc_master();
}

/* ── Directory operations ─────────────────────────────────────────────────── */

static vfs_node_t *devdir_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    if (strcmp(name, "null")    == 0) return &dev_null;
    if (strcmp(name, "zero")    == 0) return &dev_zero;
    if (strcmp(name, "tty")     == 0) return &dev_tty;
    if (strcmp(name, "ptmx")    == 0) return &dev_ptmx;   /* open_fn clones */
    if (strcmp(name, "pts")     == 0) return &dev_pts_dir;
    if (strcmp(name, "urandom") == 0) return &dev_urandom;
    if (strcmp(name, "dsp") == 0) return &dev_dsp;
    if (strcmp(name, "fb0")     == 0 && framebuffer_available()) return &dev_fb0;
    if (strcmp(name, "input")   == 0) return &dev_input_dir;
    if (strcmp(name, "shm")     == 0) return dev_shm_root;
    if (strcmp(name, "random")  == 0) return &dev_urandom;  /* alias */
    /* stdin/stdout/stderr → tty (serial console) */
    if (strcmp(name, "stdin")   == 0) return &dev_tty;
    if (strcmp(name, "stdout")  == 0) return &dev_tty;
    if (strcmp(name, "stderr")  == 0) return &dev_tty;
    return NULL;
}

static int devdir_readdir(vfs_node_t *node, uint32_t idx, vfs_dirent_t *out) {
    (void)node;
    static const char *names[] = { "null", "zero", "tty", "urandom", "dsp",
                                    "fb0", "input", "ptmx", "pts", "shm",
                                    "stdin", "stdout", "stderr" };
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (i == 4 && !framebuffer_available()) continue;
        if (out_idx == idx) {
            out->ino  = (uint32_t)(idx + 1);
            out->type = (strcmp(names[i], "input") == 0 ||
                         strcmp(names[i], "pts") == 0 ||
                         strcmp(names[i], "shm") == 0) ? VFS_FLAG_DIR
                                                        : VFS_FLAG_CHARDEV;
            strncpy(out->name, names[i], 255);
            out->name[255] = '\0';
            return 0;
        }
        out_idx++;
    }
    return -1;
}

static vfs_node_t *ptsdir_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    if (!name || !name[0] || name[1]) return NULL;
    int id = name[0] - '0';
    if (id < 0 || id >= MAX_PTYS || !ptys[id].used) return NULL;
    /* A lookup changes nothing: it neither takes a reference (the opener does,
     * via vfs_retain -> pty_slave_retain) nor clears the master's hangup (the
     * first holder does, in pty_slave_retain).  Clearing slave_closed here made
     * a bare stat("/dev/pts/N") on an unopened slave silently stop the master
     * from reporting hangup, with nothing left to set it back. */
    return &ptys[id].slave;
}

static int ptsdir_readdir(vfs_node_t *node, uint32_t idx, vfs_dirent_t *out) {
    (void)node;
    uint32_t seen = 0;
    for (int i = 0; i < MAX_PTYS; i++) {
        if (!ptys[i].used) continue;
        if (seen++ != idx) continue;
        out->ino = ptys[i].slave.inode;
        out->type = VFS_FLAG_CHARDEV;
        out->name[0] = (char)('0' + i);
        out->name[1] = 0;
        return 0;
    }
    return -1;
}

static uint32_t fb0_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    (void)n;
    return framebuffer_read(off, len, buf);
}

static uint32_t fb0_write(vfs_node_t *n, uint32_t off, uint32_t len,
                          const uint8_t *buf) {
    (void)n;
    return framebuffer_write(off, len, buf);
}

static int fb0_ioctl(vfs_node_t *n, uint32_t req, void *arg) {
    (void)n;
    return framebuffer_ioctl(req, arg);
}

static int tty_read_ready(vfs_node_t *n) {
    (void)n;
    vfs_node_t *ctty = proc_ctty_node();
    if (ctty && ctty->read_ready_fn)
        return ctty->read_ready_fn(ctty);
    return serial_data_ready();
}

static int tty_write_ready(vfs_node_t *n) {
    (void)n;
    vfs_node_t *ctty = proc_ctty_node();
    if (ctty && ctty->write_ready_fn)
        return ctty->write_ready_fn(ctty);
    return 1;
}

static int keyboard_ready(vfs_node_t *n) {
    (void)n;
    return keyboard_has_events();
}

static int mouse_ready(vfs_node_t *n) {
    (void)n;
    return mouse_has_events();
}

static uint32_t input_event_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                 uint8_t *buf) {
    (void)n; (void)off;
    return keyboard_read_events(len, buf);
}

static uint32_t input_mouse_read(vfs_node_t *n, uint32_t off, uint32_t len,
                                 uint8_t *buf) {
    (void)n; (void)off;
    return mouse_read_events(len, buf);
}

static vfs_node_t *inputdir_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    if (strcmp(name, "event0") == 0) return &dev_input_event0;
    if (strcmp(name, "event1") == 0) return &dev_input_event1;
    return NULL;
}

static int inputdir_readdir(vfs_node_t *node, uint32_t idx, vfs_dirent_t *out) {
    (void)node;
    static const char *names[] = { "event0", "event1" };
    if (idx >= 2) return -1;
    out->ino = idx + 1;
    out->type = VFS_FLAG_CHARDEV;
    strncpy(out->name, names[idx], 255);
    out->name[255] = '\0';
    return 0;
}

/* ── Public init ───────────────────────────────────────────────────────────── */

vfs_node_t *devfs_mount(void) {
    /* /dev/shm — a real writable tmpfs for POSIX shm_open("/dev/shm/..."). */
    dev_shm_root = tmpfs_mount();
    if (dev_shm_root) strncpy(dev_shm_root->name, "shm", 255);

    /* /dev/null */
    memset(&dev_null, 0, sizeof(dev_null));
    strncpy(dev_null.name, "null", 255);
    dev_null.flags    = VFS_FLAG_CHARDEV;
    dev_null.inode    = 1;
    dev_null.read_fn  = null_read;
    dev_null.write_fn = null_write;
    dev_null.read_ready_fn = always_ready;
    dev_null.write_ready_fn = always_ready;

    /* /dev/zero */
    memset(&dev_zero, 0, sizeof(dev_zero));
    strncpy(dev_zero.name, "zero", 255);
    dev_zero.flags    = VFS_FLAG_CHARDEV;
    dev_zero.inode    = 2;
    dev_zero.read_fn  = zero_read;
    dev_zero.write_fn = null_write;   /* discard writes */
    dev_zero.read_ready_fn = always_ready;
    dev_zero.write_ready_fn = always_ready;

    /* /dev/tty */
    memset(&dev_tty, 0, sizeof(dev_tty));
    strncpy(dev_tty.name, "tty", 255);
    dev_tty.flags    = VFS_FLAG_CHARDEV;
    dev_tty.inode    = 3;
    dev_tty.read_fn  = tty_read;
    dev_tty.write_fn = tty_write;
    dev_tty.ioctl_fn = tty_ioctl;
    dev_tty.read_ready_fn = tty_read_ready;
    dev_tty.write_ready_fn = tty_write_ready;

    /* /dev/dsp */
    memset(&dev_dsp, 0, sizeof(dev_dsp));
    strncpy(dev_dsp.name, "dsp", 255);
    dev_dsp.flags    = VFS_FLAG_CHARDEV;
    dev_dsp.inode    = 31;
    dev_dsp.read_fn  = dsp_read;
    dev_dsp.write_fn = dsp_write;
    dev_dsp.read_ready_fn = always_ready;
    dev_dsp.write_ready_fn = always_ready;

    /* /dev/urandom */
    memset(&dev_urandom, 0, sizeof(dev_urandom));
    strncpy(dev_urandom.name, "urandom", 255);
    dev_urandom.flags    = VFS_FLAG_CHARDEV;
    dev_urandom.inode    = 4;
    dev_urandom.read_fn  = urandom_read;
    dev_urandom.write_fn = null_write;   /* discard writes */
    dev_urandom.read_ready_fn = always_ready;
    dev_urandom.write_ready_fn = always_ready;

    /* /dev/fb0 */
    memset(&dev_fb0, 0, sizeof(dev_fb0));
    strncpy(dev_fb0.name, "fb0", 255);
    dev_fb0.flags    = VFS_FLAG_CHARDEV;
    dev_fb0.inode    = 5;
    dev_fb0.size     = framebuffer_size();
    dev_fb0.read_fn  = fb0_read;
    dev_fb0.write_fn = fb0_write;
    dev_fb0.ioctl_fn = fb0_ioctl;
    dev_fb0.read_ready_fn = always_ready;
    dev_fb0.write_ready_fn = always_ready;

    /* /dev/input/event0 */
    memset(&dev_input_event0, 0, sizeof(dev_input_event0));
    strncpy(dev_input_event0.name, "event0", 255);
    dev_input_event0.flags   = VFS_FLAG_CHARDEV;
    dev_input_event0.inode   = 6;
    dev_input_event0.read_fn = input_event_read;
    dev_input_event0.read_ready_fn = keyboard_ready;

    /* /dev/input/event1 */
    memset(&dev_input_event1, 0, sizeof(dev_input_event1));
    strncpy(dev_input_event1.name, "event1", 255);
    dev_input_event1.flags   = VFS_FLAG_CHARDEV;
    dev_input_event1.inode   = 7;
    dev_input_event1.read_fn = input_mouse_read;
    dev_input_event1.read_ready_fn = mouse_ready;

    /* /dev/input */
    memset(&dev_input_dir, 0, sizeof(dev_input_dir));
    strncpy(dev_input_dir.name, "input", 255);
    dev_input_dir.flags      = VFS_FLAG_DIR;
    dev_input_dir.finddir_fn = inputdir_finddir;
    dev_input_dir.readdir_fn = inputdir_readdir;

    /* /dev/ptmx */
    memset(&dev_ptmx, 0, sizeof(dev_ptmx));
    strncpy(dev_ptmx.name, "ptmx", 255);
    dev_ptmx.flags = VFS_FLAG_CHARDEV;
    dev_ptmx.inode = 20;
    dev_ptmx.open_fn = ptmx_open;

    /* /dev/pts */
    memset(&dev_pts_dir, 0, sizeof(dev_pts_dir));
    strncpy(dev_pts_dir.name, "pts", 255);
    dev_pts_dir.flags = VFS_FLAG_DIR;
    dev_pts_dir.finddir_fn = ptsdir_finddir;
    dev_pts_dir.readdir_fn = ptsdir_readdir;

    /* /dev directory */
    memset(&dev_dir, 0, sizeof(dev_dir));
    strncpy(dev_dir.name, "dev", 255);
    dev_dir.flags       = VFS_FLAG_DIR;
    dev_dir.finddir_fn  = devdir_finddir;
    dev_dir.readdir_fn  = devdir_readdir;

    return &dev_dir;
}
