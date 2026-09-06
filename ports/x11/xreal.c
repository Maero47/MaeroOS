/* xreal — a REAL Xlib client linked against the cross-built libX11/libxcb.
 * It connects to maeroX via XOpenDisplay and draws with XFillRectangle —
 * proving maeroX speaks enough real X11 for the actual library GTK/Firefox use.
 *   xreal          spawn headless maeroX, connect, draw, report (smoke)
 *   xreal --win    spawn windowed maeroX, connect, draw, hold (screenshot) */
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    int win = (argc > 1 && !strcmp(argv[1], "--win"));
    pid_t srv = fork();
    if (srv == 0) {
        if (win) { execl("/disk/maerox","maerox","3",(char*)0); execl("/maerox","maerox","3",(char*)0); }
        else     { execl("/maerox","maerox","-H",(char*)0); execl("/disk/maerox","maerox","-H",(char*)0); }
        _exit(127);
    }

    Display *d = 0;
    for (int i = 0; i < 200 && !d; i++) { d = XOpenDisplay(":0"); if (!d) usleep(20000); }
    if (!d) { printf("XREAL_FAIL XOpenDisplay\n"); fflush(stdout); kill(srv,9); return 1; }

    int s = DefaultScreen(d);
    Window root = RootWindow(d, s);
    Window w = XCreateSimpleWindow(d, root, 60, 50, 300, 180, 0, 0, 0x002F6FB0);
    XSelectInput(d, w, ExposureMask | ButtonPressMask);
    XMapWindow(d, w);
    GC gc = XCreateGC(d, w, 0, 0);
    printf("XREAL_OK connected root=0x%lx win=0x%lx\n",
           (unsigned long)root, (unsigned long)w);
    fflush(stdout);

    int painted = 0, loops = win ? 100000 : 400;
    for (int i = 0; i < loops; i++) {
        while (XPending(d)) {
            XEvent e; XNextEvent(d, &e);
            if (e.type == Expose && !painted) {
                XSetForeground(d, gc, 0x00E0A020); XFillRectangle(d, w, gc, 40, 30, 220, 70);
                XSetForeground(d, gc, 0x0040C060); XFillRectangle(d, w, gc, 70, 110, 160, 40);
                XFlush(d); painted = 1;
                printf("XREAL_PAINTED\n"); fflush(stdout);
                if (!win) { kill(srv,9); waitpid(srv,0,0); return 0; }
            }
            if (e.type == ButtonPress) {
                XSetForeground(d, gc, 0x00FFFFFF);
                XFillRectangle(d, w, gc, e.xbutton.x-8, e.xbutton.y-8, 16, 16);
                XFlush(d);
            }
        }
        usleep(20000);
    }
    if (!painted) printf("XREAL_FAIL no-expose\n");
    fflush(stdout);
    kill(srv,9); waitpid(srv,0,0);
    return painted ? 0 : 1;
}
