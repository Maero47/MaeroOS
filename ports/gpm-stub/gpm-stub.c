/* Real "gpm" for MaeroOS: translates /dev/input/event1 (Linux input_event
 * stream from the kernel PS/2 driver) into Gpm_Events for links2's fb UI.
 * Built with musl in the cross container; statically linked into links. */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "gpm.h"

/* 16-byte i686 input_event — keep local, never <linux/input.h> (time64
 * musl would change the size and silently break the read framing). */
struct mae_input_event {
    int sec, usec;
    unsigned short type, code;
    int value;
};

#define EV_SYN_  0
#define EV_KEY_  1
#define EV_REL_  2
#define REL_X_   0x00
#define REL_Y_   0x01
#define REL_WHEEL_ 0x08
#define BTN_LEFT_   0x110
#define BTN_RIGHT_  0x111
#define BTN_MIDDLE_ 0x112

int gpm_fd = -1;
int gpm_flag = 0;

static unsigned char cur_buttons;   /* GPM_B_* held right now */
static unsigned char pending_up;    /* release seen in same SYN burst as press */

int Gpm_Open(Gpm_Connect *conn, int flag) {
    (void)conn; (void)flag;
    gpm_fd = open("/dev/input/event1", O_RDONLY);
    if (gpm_fd < 0)
        return -1;
    gpm_flag = 1;
    cur_buttons = 0;
    pending_up = 0;
    return gpm_fd;            /* links keeps this as its select() fd */
}

int Gpm_Close(void) {
    if (gpm_fd >= 0)
        close(gpm_fd);
    gpm_fd = -1;
    gpm_flag = 0;
    return 0;
}

static unsigned char btn_bit(unsigned short code) {
    if (code == BTN_LEFT_)   return GPM_B_LEFT;
    if (code == BTN_RIGHT_)  return GPM_B_RIGHT;
    if (code == BTN_MIDDLE_) return GPM_B_MIDDLE;
    return 0;
}

/* Must return 1 per event; returning <=0 makes links disable the mouse
 * permanently.  Only called when select() says gpm_fd is readable, so the
 * drain-until-SYN read loop blocks at most mid-burst (the kernel driver
 * always terminates a packet with EV_SYN in the same interrupt). */
int Gpm_GetEvent(Gpm_Event *ev) {
    int dx = 0, dy = 0, wdy = 0;
    unsigned char pressed = 0, released = 0;
    struct mae_input_event ie;

    memset(ev, 0, sizeof(*ev));
    ev->vc = 1;

    if (pending_up) {         /* deferred release from a fast click */
        ev->type = (enum Gpm_Etype)(GPM_UP | GPM_SMOOTH);
        ev->buttons = pending_up;
        cur_buttons &= ~pending_up;
        pending_up = 0;
        return 1;
    }

    for (;;) {
        ssize_t n = read(gpm_fd, &ie, sizeof(ie));
        if (n == 0) {
            /* MaeroOS event reads don't block: ring momentarily empty
             * mid-burst (or spurious wakeup).  Returning <=0 here would
             * permanently disable the mouse in links — wait instead. */
            usleep(2000);
            continue;
        }
        if (n != (ssize_t)sizeof(ie))
            return -1;
        if (ie.type == EV_REL_ && ie.code == REL_X_) dx += ie.value;
        else if (ie.type == EV_REL_ && ie.code == REL_Y_) dy += ie.value;
        else if (ie.type == EV_REL_ && ie.code == REL_WHEEL_) wdy += ie.value;
        else if (ie.type == EV_KEY_) {
            unsigned char b = btn_bit(ie.code);
            if (!b) continue;
            if (ie.value) { pressed |= b; cur_buttons |= b; }
            else if (pressed & b) pending_up |= b;   /* press+release same burst */
            else { released |= b; cur_buttons &= ~b; }
        } else if (ie.type == EV_SYN_) {
            break;
        }
    }

    ev->dx = (short)dx;
    ev->dy = (short)dy;
    ev->wdy = (short)wdy;
    if (pressed) {
        ev->type = GPM_DOWN;
        ev->buttons = pressed;
    } else if (released) {
        ev->type = GPM_UP;          /* UP carries the released button */
        ev->buttons = released;
    } else if (cur_buttons) {
        ev->type = GPM_DRAG;
        ev->buttons = cur_buttons;
    } else {
        ev->type = GPM_MOVE;
        ev->buttons = 0;
    }
    ev->type = (enum Gpm_Etype)((int)ev->type | GPM_SMOOTH);
    return 1;
}

char *Gpm_GetLibVersion(int *where) {
    if (where) *where = 12007;
    return "1.20.7";
}
