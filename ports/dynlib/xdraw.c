/* xdraw — raw X11 client that draws into a maeroX window.  Creates a top-level
 * window, fills it, draws a rectangle and a PutImage gradient, and verifies the
 * server tracked the window via GetGeometry → prints XDRAW_OK.
 *
 *   xdraw            connect to a running maeroX, draw, verify, exit
 *   xdraw --spawn    fork+exec a headless maeroX first (for smoke tests)
 *   xdraw hold       after drawing, stay connected (for a GUI screenshot)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static int X;                 /* the connection fd */
static unsigned id_base, id_mask, root, visual;
static unsigned next_id;

static unsigned u16(const unsigned char *p){ return p[0]|(p[1]<<8); }
static unsigned u32(const unsigned char *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((unsigned)p[3]<<24); }

static int rd(int fd, unsigned char *b, int n){
    int g=0; while(g<n){ int r=read(fd,b+g,n-g); if(r>0)g+=r; else if(r==0)return g; else return -1; } return g;
}
static void w8(unsigned char **p, unsigned v){ *(*p)++ = (unsigned char)v; }
static void w16(unsigned char **p, unsigned v){ w8(p,v); w8(p,v>>8); }
static void w32(unsigned char **p, unsigned v){ w8(p,v); w8(p,v>>8); w8(p,v>>16); w8(p,v>>24); }

static unsigned alloc_id(void){ return id_base | (next_id++ & id_mask); }

static int connect_x(void){
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a; memset(&a,0,sizeof(a));
    a.sun_family = AF_UNIX; strcpy(a.sun_path, "/tmp/.X11-unix/X0");
    if (connect(fd,(struct sockaddr*)&a, sizeof(a.sun_family)+strlen(a.sun_path))==0) return fd;
    close(fd); return -1;
}

static int handshake(void){
    unsigned char req[12]={0}; req[0]='l'; req[2]=11;
    if (write(X,req,12)!=12) return -1;
    unsigned char h[8]; if (rd(X,h,8)!=8 || h[0]!=1) return -1;
    unsigned extra = u16(h+6)*4;
    unsigned char body[1024]; if (extra>sizeof(body)) extra=sizeof(body);
    if ((unsigned)rd(X,body,extra)!=extra) return -1;
    id_base = u32(body+4); id_mask = u32(body+8);
    unsigned vlen=u16(body+16);
    unsigned off = 32 + ((vlen+3)&~3u) + 2*8;
    root   = u32(body+off);
    visual = u32(body+off+32);       /* root-visual is at screen+32 */
    return 0;
}

static void create_window(unsigned wid, int x,int y,int w,int h){
    unsigned char b[64], *p=b;
    w8(&p,1); w8(&p,24); w16(&p,8);              /* op, depth, length=8 */
    w32(&p,wid); w32(&p,root);
    w16(&p,x); w16(&p,y); w16(&p,w); w16(&p,h);
    w16(&p,0); w16(&p,1);                        /* border=0, class=InputOutput */
    w32(&p,visual); w32(&p,0);                   /* visual, value-mask=0 */
    write(X,b,p-b);
}
static void create_gc(unsigned gc, unsigned drawable, unsigned fg){
    unsigned char b[32], *p=b;
    w8(&p,55); w8(&p,0); w16(&p,5);              /* op, _, length=5 */
    w32(&p,gc); w32(&p,drawable); w32(&p,0x4);   /* mask=GCForeground */
    w32(&p,fg);
    write(X,b,p-b);
}
static void change_gc_fg(unsigned gc, unsigned fg){
    unsigned char b[32], *p=b;
    w8(&p,56); w8(&p,0); w16(&p,4);
    w32(&p,gc); w32(&p,0x4); w32(&p,fg);
    write(X,b,p-b);
}
static void map_window(unsigned wid){
    unsigned char b[8], *p=b; w8(&p,8); w8(&p,0); w16(&p,2); w32(&p,wid); write(X,b,p-b);
}
static void fill_rect(unsigned d, unsigned gc, int x,int y,int w,int h){
    unsigned char b[32], *p=b;
    w8(&p,70); w8(&p,0); w16(&p,5);
    w32(&p,d); w32(&p,gc);
    w16(&p,x); w16(&p,y); w16(&p,w); w16(&p,h);
    write(X,b,p-b);
}
static void put_gradient(unsigned d, unsigned gc, int dx,int dy,int w,int h){
    int stride=((w*4+3)&~3);
    int len = (24 + stride*h + 3)/4;
    unsigned char hdr[24], *p=hdr;
    w8(&p,72); w8(&p,2); w16(&p,len);            /* op, ZPixmap, length */
    w32(&p,d); w32(&p,gc);
    w16(&p,w); w16(&p,h); w16(&p,dx); w16(&p,dy);
    w8(&p,0); w8(&p,24); w16(&p,0);              /* left-pad, depth, unused */
    write(X,hdr,24);
    static unsigned char row[4096];
    for (int yy=0; yy<h; yy++){
        unsigned char *q=row;
        for (int xx=0; xx<w; xx++){
            unsigned char r=(unsigned char)(xx*255/w), g=(unsigned char)(yy*255/h), bl=128;
            w8(&q,bl); w8(&q,g); w8(&q,r); w8(&q,0);   /* BGRX */
        }
        write(X,row,stride);
    }
}
static int get_geometry(unsigned d, int *w,int *h){
    unsigned char b[8], *p=b; w8(&p,14); w8(&p,0); w16(&p,2); w32(&p,d);
    write(X,b,p-b);
    /* Skip any events (byte0 >= 2) queued ahead of the reply (byte0 == 1). */
    for (int i=0;i<32;i++){
        unsigned char rep[32]; if (rd(X,rep,32)!=32) return -1;
        if (rep[0]==1){ *w=u16(rep+16); *h=u16(rep+18); return 0; }
        if (rep[0]==0) return -1;            /* error */
    }
    return -1;
}

int main(int argc, char **argv){
    int spawn=0, windowed=0, hold=0; pid_t srv=0;
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--spawn")) spawn=1;
        else if (!strcmp(argv[i],"--win")) { windowed=1; hold=1; }
        else if (!strcmp(argv[i],"hold")) hold=1;
    }
    if (spawn || windowed){
        srv=fork();
        if (srv==0){
            if (windowed){ execl("/disk/maerox","maerox","3",(char*)0); execl("/maerox","maerox","3",(char*)0); }
            else         { execl("/maerox","maerox","-H",(char*)0); execl("/disk/maerox","maerox","-H",(char*)0); }
            _exit(127);
        }
    }
    for (int i=0;i<200 && (X=connect_x())<0;i++) usleep(20000);
    if (X<0){ printf("XDRAW_FAIL connect\n"); if(srv)kill(srv,9); return 1; }
    if (handshake()<0){ printf("XDRAW_FAIL handshake\n"); if(srv)kill(srv,9); return 1; }

    unsigned win=alloc_id(), gc=alloc_id();
    create_window(win, 40,40, 320,200);
    create_gc(gc, win, 0x00CC3030);              /* red */
    map_window(win);
    fill_rect(win, gc, 0,0, 320,200);            /* whole window red */
    change_gc_fg(gc, 0x0030B050);                /* green */
    fill_rect(win, gc, 30,30, 130,90);           /* green rectangle */
    put_gradient(win, gc, 180,100, 100,70);      /* gradient block */

    int gw=0, gh=0;
    if (get_geometry(win, &gw, &gh)<0){ printf("XDRAW_FAIL geometry\n"); if(srv)kill(srv,9); return 1; }
    if (gw!=320 || gh!=200){ printf("XDRAW_FAIL geom w=%d h=%d\n",gw,gh); if(srv)kill(srv,9); return 1; }

    printf("XDRAW_OK win=0x%x w=%d h=%d\n", win, gw, gh);
    fflush(stdout);

    if (hold){ for(;;) sleep(1); }
    if (srv){ kill(srv,9); waitpid(srv,0,0); }
    return 0;
}
