/* xevent — raw X11 client exercising maeroX events (Phase 33c).
 *   xevent --spawn  spawn headless maeroX, map a window, wait for the Expose
 *                   event the server sends on map → XEVENT_OK.
 *   xevent --win    spawn windowed maeroX, map a red window, and on every
 *                   forwarded ButtonPress draw a green mark at the click — so a
 *                   rig click shows up on screen (proves input forwarding). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static int X;
static unsigned id_base, id_mask, root, visual, next_id;

static unsigned u16(const unsigned char *p){ return p[0]|(p[1]<<8); }
static unsigned u32(const unsigned char *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((unsigned)p[3]<<24); }
static int rd(int fd, unsigned char *b, int n){ int g=0; while(g<n){ int r=read(fd,b+g,n-g); if(r>0)g+=r; else if(r==0)return g; else return -1; } return g; }
static void w8(unsigned char **p, unsigned v){ *(*p)++=(unsigned char)v; }
static void w16(unsigned char **p, unsigned v){ w8(p,v); w8(p,v>>8); }
static void w32(unsigned char **p, unsigned v){ w8(p,v); w8(p,v>>8); w8(p,v>>16); w8(p,v>>24); }
static unsigned alloc_id(void){ return id_base | (next_id++ & id_mask); }

static int connect_x(void){
    int fd=socket(AF_UNIX,SOCK_STREAM,0); if(fd<0)return -1;
    struct sockaddr_un a; memset(&a,0,sizeof(a)); a.sun_family=AF_UNIX; strcpy(a.sun_path,"/tmp/.X11-unix/X0");
    if(connect(fd,(struct sockaddr*)&a,sizeof(a.sun_family)+strlen(a.sun_path))==0)return fd;
    close(fd); return -1;
}
static int handshake(void){
    unsigned char req[12]={0}; req[0]='l'; req[2]=11; if(write(X,req,12)!=12)return -1;
    unsigned char h[8]; if(rd(X,h,8)!=8||h[0]!=1)return -1;
    unsigned extra=u16(h+6)*4; unsigned char body[1024]; if(extra>sizeof(body))extra=sizeof(body);
    if((unsigned)rd(X,body,extra)!=extra)return -1;
    id_base=u32(body+4); id_mask=u32(body+8);
    unsigned vlen=u16(body+16), off=32+((vlen+3)&~3u)+2*8;
    root=u32(body+off); visual=u32(body+off+32); return 0;
}
static void create_window(unsigned wid,int x,int y,int w,int h){
    unsigned char b[64],*p=b; w8(&p,1); w8(&p,24); w16(&p,8);
    w32(&p,wid); w32(&p,root); w16(&p,x); w16(&p,y); w16(&p,w); w16(&p,h);
    w16(&p,0); w16(&p,1); w32(&p,visual); w32(&p,0); write(X,b,p-b);
}
static void create_gc(unsigned gc,unsigned d,unsigned fg){
    unsigned char b[32],*p=b; w8(&p,55); w8(&p,0); w16(&p,5);
    w32(&p,gc); w32(&p,d); w32(&p,0x4); w32(&p,fg); write(X,b,p-b);
}
static void change_gc_fg(unsigned gc,unsigned fg){
    unsigned char b[32],*p=b; w8(&p,56); w8(&p,0); w16(&p,4);
    w32(&p,gc); w32(&p,0x4); w32(&p,fg); write(X,b,p-b);
}
static void map_window(unsigned wid){ unsigned char b[8],*p=b; w8(&p,8); w8(&p,0); w16(&p,2); w32(&p,wid); write(X,b,p-b); }
static void fill_rect(unsigned d,unsigned gc,int x,int y,int w,int h){
    unsigned char b[32],*p=b; w8(&p,70); w8(&p,0); w16(&p,5);
    w32(&p,d); w32(&p,gc); w16(&p,x); w16(&p,y); w16(&p,w); w16(&p,h); write(X,b,p-b);
}

int main(int argc,char**argv){
    int spawn=0,win=0; pid_t srv=0;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--spawn"))spawn=1; else if(!strcmp(argv[i],"--win"))win=1; }
    if(spawn||win){
        srv=fork();
        if(srv==0){
            if(win){ execl("/disk/maerox","maerox","3",(char*)0); execl("/maerox","maerox","3",(char*)0); }
            else   { execl("/maerox","maerox","-H",(char*)0); execl("/disk/maerox","maerox","-H",(char*)0); }
            _exit(127);
        }
    }
    for(int i=0;i<200&&(X=connect_x())<0;i++) usleep(20000);
    if(X<0){ printf("XEVENT_FAIL connect\n"); if(srv)kill(srv,9); return 1; }
    if(handshake()<0){ printf("XEVENT_FAIL handshake\n"); if(srv)kill(srv,9); return 1; }

    unsigned w=alloc_id(), gc=alloc_id();
    create_window(w,40,40,320,200);
    create_gc(gc,w,0x00CC3030);
    map_window(w);
    fill_rect(w,gc,0,0,320,200);

    if(!win){
        /* Headless: read events until we see an Expose (type 12) for our win. */
        for(int n=0;n<8;n++){
            unsigned char e[32]; if(rd(X,e,32)!=32) break;
            if(e[0]==12 && u32(e+4)==w){
                printf("XEVENT_OK expose win=0x%x %ux%u\n", w, u16(e+12), u16(e+14));
                fflush(stdout); if(srv){ kill(srv,9); waitpid(srv,0,0);} return 0;
            }
        }
        printf("XEVENT_FAIL no-expose\n"); if(srv)kill(srv,9); return 1;
    }

    /* Windowed: react to forwarded clicks by drawing a green mark. */
    printf("XEVENT_WIN ready\n"); fflush(stdout);
    for(;;){
        unsigned char e[32]; int r=rd(X,e,32); if(r<=0) break;
        if(e[0]==4){                       /* ButtonPress */
            int ex=(int)u16(e+24), ey=(int)u16(e+26);
            change_gc_fg(gc,0x0030E060);
            fill_rect(w,gc,ex-12,ey-12,24,24);
        }
    }
    if(srv){ kill(srv,9); waitpid(srv,0,0);} return 0;
}
