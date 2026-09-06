/* gtkprobe — a REAL GTK3 application on MaeroOS.  Creates a top-level window
 * with a button + label, draws via Cairo into an X window through maeroX.
 *   gtkprobe          headless-ish: init, build widgets, verify, exit (smoke)
 *   gtkprobe show     spawn windowed maeroX, show the window, run (screenshot) */
#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Wait until maeroX has bound /tmp/.X11-unix/X0 — gtk_init_check tries the
 * display only once, so it must not race the (windowed) server's startup. */
static void wait_for_x(void){
    for (int i=0;i<400;i++){
        int fd=socket(AF_UNIX,SOCK_STREAM,0);
        if (fd>=0){
            struct sockaddr_un a; memset(&a,0,sizeof(a));
            a.sun_family=AF_UNIX; strcpy(a.sun_path,"/tmp/.X11-unix/X0");
            int ok=connect(fd,(struct sockaddr*)&a,sizeof(a.sun_family)+strlen(a.sun_path))==0;
            close(fd);
            if (ok) return;
        }
        usleep(15000);
    }
}

static int clicked = 0, drawn = 0;
static void on_click(GtkButton *b, gpointer u){ (void)b;(void)u; clicked++; printf("GTK_CLICKED %d\n", clicked); fflush(stdout); }

/* Explicit draw callback on a GtkDrawingArea — canonical GTK3 drawing.  This
 * lets us both verify a real cairo draw cycle ran and paint visible content. */
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer u){
    (void)u;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    cairo_set_source_rgb(cr, 0.10, 0.45, 0.70); cairo_paint(cr);           /* blue bg */
    cairo_set_source_rgb(cr, 0.95, 0.65, 0.10);
    cairo_rectangle(cr, 20, 20, width-40, 50); cairo_fill(cr);             /* orange bar */
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 22);
    cairo_move_to(cr, 30, 55); cairo_show_text(cr, "GTK on MaeroOS");
    if (!drawn){ drawn=1; printf("GTK_DRAWN ok\n"); fflush(stdout); }
    return TRUE;
}

int main(int argc, char **argv){
    int show = (argc>1 && !strcmp(argv[1],"show"));
    pid_t srv = fork();
    if (srv==0){
        if (show){ execl("/maerox","maerox","3",(char*)0); execl("/disk/maerox","maerox","3",(char*)0); }
        else     { execl("/maerox","maerox","-H",(char*)0); execl("/disk/maerox","maerox","-H",(char*)0); }
        _exit(127);
    }
    wait_for_x();
    setenv("DISPLAY", ":0", 1);
    setenv("FONTCONFIG_PATH","/etc/fonts",1);
    setenv("GDK_BACKEND","x11",1);
    setenv("HOME","/tmp",1);
    /* Disable the AT-SPI accessibility bridge: GTK3 tries to connect it to a
     * D-Bus session bus at startup, which doesn't exist here and makes window
     * creation block.  This is the standard fix for GTK apps on minimal systems. */
    setenv("NO_AT_BRIDGE","1",1);
    setenv("GTK_A11Y","none",1);

    if (!gtk_init_check(&argc, &argv)) { printf("GTK_FAIL init\n"); fflush(stdout); kill(srv,9); return 1; }
    printf("GTK_OK init v%d.%d.%d\n", gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());
    fflush(stdout);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 320, 180);
    gtk_window_set_title(GTK_WINDOW(win), "GTK on MaeroOS");
    GtkWidget *area = gtk_drawing_area_new();
    g_signal_connect(area, "draw", G_CALLBACK(on_draw), NULL);
    gtk_container_add(GTK_CONTAINER(win), area);
    gtk_widget_show_all(win);
    printf("GTK_WINDOW_SHOWN title='GTK on MaeroOS'\n"); fflush(stdout);

    /* Force an initial draw cycle (invalidate so the frame clock paints). */
    gtk_widget_queue_draw(win);

    if (show){ gtk_main(); }
    else {
        /* Pump the main loop, dispatching frame-clock timeouts (which fire even
         * when no X events are pending — so iterate unconditionally). */
        for (int i=0;i<300 && !drawn;i++){ gtk_main_iteration_do(FALSE); usleep(10000); }
        for (int i=0;i<30;i++){ gtk_main_iteration_do(FALSE); usleep(10000); }
        printf("GTK_PUMPED ok drawn=%d\n", drawn); fflush(stdout);
    }
    kill(srv,9); waitpid(srv,0,0);
    return 0;
}
