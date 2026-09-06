//
// i_input_maeros.c — MaeroOS keyboard backend for fbDOOM.
//
// Reads Linux input_event records from /dev/input/event0 (the MaeroOS
// keyboard driver emits standard EV_KEY codes) and posts doom events.
//

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include "config.h"
#include "d_event.h"
#include "doomkeys.h"

static int kb_fd = -1;

/* referenced by m_menu/m_config (tty-input legacy knobs) */
int vanilla_keyboard_mapping = 1;

void kbd_shutdown(void) {}

struct input_event {
    int32_t tv_sec;
    int32_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

#define EV_KEY 1

/* Linux keycode → doom key */
static int map_key(unsigned short code) {
    switch (code) {
    case 103: return KEY_UPARROW;     /* KEY_UP */
    case 108: return KEY_DOWNARROW;
    case 105: return KEY_LEFTARROW;
    case 106: return KEY_RIGHTARROW;
    case 29:  return KEY_FIRE;        /* left ctrl */
    case 97:  return KEY_FIRE;        /* right ctrl */
    case 57:  return KEY_USE;         /* space */
    case 42:  return KEY_RSHIFT;      /* shift = run */
    case 54:  return KEY_RSHIFT;
    case 1:   return KEY_ESCAPE;
    case 28:  return KEY_ENTER;
    case 15:  return KEY_TAB;
    case 14:  return KEY_BACKSPACE;
    /* letters (Y/N prompts, weapon select via digits, cheats) */
    case 2:  return '1';
    case 3:  return '2';
    case 4:  return '3';
    case 5:  return '4';
    case 6:  return '5';
    case 7:  return '6';
    case 8:  return '7';
    case 16: return 'q';
    case 17: return 'w';
    case 18: return 'e';
    case 19: return 'r';
    case 20: return 't';
    case 21: return 'y';
    case 22: return 'u';
    case 23: return 'i';
    case 24: return 'o';
    case 25: return 'p';
    case 30: return 'a';
    case 31: return 's';
    case 32: return 'd';
    case 33: return 'f';
    case 34: return 'g';
    case 35: return 'h';
    case 36: return 'j';
    case 37: return 'k';
    case 38: return 'l';
    case 44: return 'z';
    case 45: return 'x';
    case 46: return 'c';
    case 47: return 'v';
    case 48: return 'b';
    case 49: return 'n';
    case 50: return 'm';
    case 51: return ',';
    case 52: return '.';
    case 12: return '-';
    case 13: return '=';
    case 58: return KEY_CAPSLOCK;
    case 87: return KEY_F11;
    default: return 0;
    }
}

int I_InitInput(void) {
    kb_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (kb_fd < 0)
        printf("i_input: /dev/input/event0 unavailable\n");
    else
        printf("i_input: MaeroOS keyboard online\n");
    return 0;
}

void I_ShutdownInput(void) {
    if (kb_fd >= 0)
        close(kb_fd);
    kb_fd = -1;
}

void I_GetEvent(void) {
    struct input_event ev;
    event_t event;

    if (kb_fd < 0)
        return;
    while (read(kb_fd, &ev, sizeof(ev)) == (int)sizeof(ev)) {
        if (ev.type != EV_KEY || ev.value > 1)
            continue;                       /* ignore autorepeat */
        {
            int dk = map_key(ev.code);
            if (!dk)
                continue;
            event.type = ev.value ? ev_keydown : ev_keyup;
            event.data1 = dk;
            event.data2 = dk >= 'a' && dk <= 'z' ? dk : 0;
            event.data3 = 0;
            D_PostEvent(&event);
        }
    }
}
