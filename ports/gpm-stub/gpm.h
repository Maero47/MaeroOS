#ifndef _GPM_H_
#define _GPM_H_
/* Minimal libgpm ABI for static links2 on MaeroOS.
 * Gpm_Open reads the kernel mouse ring at /dev/input/event1 directly —
 * there is no gpm daemon; this library IS the mouse driver glue. */

#define GPM_B_LEFT   4
#define GPM_B_MIDDLE 2
#define GPM_B_RIGHT  1

enum Gpm_Etype {
    GPM_MOVE = 1, GPM_DRAG = 2, GPM_DOWN = 4, GPM_UP = 8,
    GPM_SMOOTH = 2048,   /* dx/dy are raw pixels, no 8x grid scaling */
};
#define GPM_HAVE_SMOOTH      /* links #ifdefs on this for the smooth path */

typedef struct Gpm_Connect {
    unsigned short eventMask, defaultMask;
    unsigned short minMod, maxMod;
    int pid, vc;
} Gpm_Connect;

typedef struct Gpm_Event {
    unsigned char buttons, modifiers;
    unsigned short vc;
    short dx, dy, x, y;
    enum Gpm_Etype type;
    int clicks;
    enum Gpm_Etype margin;
    short wdx, wdy;
} Gpm_Event;

extern int gpm_fd;
extern int gpm_flag;

int Gpm_Open(Gpm_Connect *conn, int flag);
int Gpm_Close(void);
int Gpm_GetEvent(Gpm_Event *event);
char *Gpm_GetLibVersion(int *where);

#endif
