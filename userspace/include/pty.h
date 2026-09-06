#pragma once
#include <termios.h>

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int openpty(int *amaster, int *aslave, char *name,
            const struct termios *termp, const struct winsize *winp);
