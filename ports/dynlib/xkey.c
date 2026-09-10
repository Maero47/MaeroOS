/* xkey — raw X11 client proving maeroX's keyboard path end to end.
 *
 *   xkey --spawn   spawn a headless maeroX, map a window, take the input focus,
 *                  read the keymap and the modifier map, inject keys through
 *                  maeroX's test channel and assert that each one arrives as a
 *                  KeyPress/KeyRelease with the right keycode and state AND
 *                  translates to the right character -> XKEY_OK.
 *
 * The character step is what proves the keymap rather than just the event.  A
 * real Xlib client would call XLookupString(); libX11 is not linkable here (the
 * cross-built libX11.a needs the maeros-cross container), so this file carries
 * the same translation Xlib's XLookupString performs on a non-XKB server:
 *   - pick column 1 of the keycode's keysym list when Shift is set, else 0;
 *   - with Lock set, use the upper-case column for alphabetic keys;
 *   - map the keysym to Latin-1 (0x20..0xff are their own code point, the
 *     0xff.. control keysyms are keysym & 0x7f);
 *   - with Control set, fold the character to its control code.
 * Everything it works from - the keysym table, the modifier table, the keycode
 * in the event and the state mask - comes from the server over the wire.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#define KEYFIFO "/tmp/.maerox-keys"

/* X11 modifier mask bits. */
#define SHIFT_MASK   0x01
#define LOCK_MASK    0x02
#define CONTROL_MASK 0x04
#define MOD1_MASK    0x08

/* Linux keycodes we inject (drivers/keyboard.c); X keycode = these + 8. */
#define LK_1 2
#define LK_BACKSPACE 14
#define LK_ENTER 28
#define LK_A 30
#define LK_C 46
#define LK_V 47
#define LK_LEFTSHIFT 42
#define LK_LEFTCTRL 29
#define LK_CAPSLOCK 58
#define LK_LEFTALT 56
#define LK_LEFT 105

static int X;
static unsigned id_base, id_mask, root, visual, next_id;
static unsigned min_kc, max_kc;
static int fails;

static unsigned u16(const unsigned char *p){ return p[0]|(p[1]<<8); }
static unsigned u32(const unsigned char *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((unsigned)p[3]<<24); }
static int rd(int fd, unsigned char *b, int n){ int g=0; while(g<n){ int r=read(fd,b+g,n-g); if(r>0)g+=r; else if(r==0)return g; else return -1; } return g; }
static void w8(unsigned char **p, unsigned v){ *(*p)++=(unsigned char)v; }
static void w16(unsigned char **p, unsigned v){ w8(p,v); w8(p,v>>8); }
static void w32(unsigned char **p, unsigned v){ w8(p,v); w8(p,v>>8); w8(p,v>>16); w8(p,v>>24); }
static unsigned alloc_id(void){ return id_base | (next_id++ & id_mask); }

static int connect_x(void){
    int fd=socket(AF_UNIX,SOCK_STREAM,0); if(fd<0)return -1;
    struct sockaddr_un a; memset(&a,0,sizeof(a)); a.sun_family=AF_UNIX;
    strcpy(a.sun_path,"/tmp/.X11-unix/X0");
    if(connect(fd,(struct sockaddr*)&a,sizeof(a.sun_family)+strlen(a.sun_path))==0)return fd;
    close(fd); return -1;
}
static int handshake(void){
    unsigned char req[12]={0}; req[0]='l'; req[2]=11; if(write(X,req,12)!=12)return -1;
    unsigned char h[8]; if(rd(X,h,8)!=8||h[0]!=1)return -1;
    unsigned extra=u16(h+6)*4; unsigned char body[1024]; if(extra>sizeof(body))extra=sizeof(body);
    if((unsigned)rd(X,body,extra)!=extra)return -1;
    id_base=u32(body+4); id_mask=u32(body+8);
    min_kc=body[26]; max_kc=body[27];
    unsigned vlen=u16(body+16), off=32+((vlen+3)&~3u)+2*8;
    root=u32(body+off); visual=u32(body+off+32); return 0;
}
static void create_window(unsigned wid,int x,int y,int w,int h){
    unsigned char b[64],*p=b; w8(&p,1); w8(&p,24); w16(&p,8);
    w32(&p,wid); w32(&p,root); w16(&p,x); w16(&p,y); w16(&p,w); w16(&p,h);
    w16(&p,0); w16(&p,1); w32(&p,visual); w32(&p,0); write(X,b,p-b);
}
static void map_window(unsigned wid){ unsigned char b[8],*p=b; w8(&p,8); w8(&p,0); w16(&p,2); w32(&p,wid); write(X,b,p-b); }
static void set_input_focus(unsigned wid){
    unsigned char b[16],*p=b; w8(&p,42); w8(&p,1 /* RevertToPointerRoot */); w16(&p,3);
    w32(&p,wid); w32(&p,0 /* CurrentTime */); write(X,b,p-b);
}

/* ── replies ─────────────────────────────────────────────────────────────────
 * Events and replies share the connection, so a reply read has to skip any
 * event that arrives first.  Events are always 32 bytes; a reply is 32 bytes
 * plus 4*reply-length more.  buf gets the whole reply. */
static int read_reply(unsigned char *buf, int cap){
    for(int i=0;i<64;i++){
        unsigned char h[32];
        if(rd(X,h,32)!=32) return -1;
        if(h[0]==0){ printf("XKEY_FAIL error code=%d op=%d\n", h[1], h[10]); return -1; }
        if(h[0]!=1) continue;                        /* an event; skip it */
        unsigned extra=u32(h+4)*4;
        if((int)(32+extra)>cap) return -1;
        memcpy(buf,h,32);
        if(extra && (unsigned)rd(X,buf+32,extra)!=extra) return -1;
        return (int)(32+extra);
    }
    return -1;
}

/* keysyms[keycode - min_kc][column] */
static unsigned keysyms[256][8];
static int      per_keycode;
static unsigned char modmap[8][8];      /* [modifier][slot] = keycode */
static int      per_modifier;

static int get_keyboard_mapping(void){
    int count=(int)(max_kc-min_kc+1);
    unsigned char b[8],*p=b; w8(&p,101); w8(&p,0); w16(&p,2); w8(&p,min_kc); w8(&p,count); w16(&p,0);
    if(write(X,b,p-b)!=(int)(p-b)) return -1;
    static unsigned char r[32+256*8*4];
    int n=read_reply(r,sizeof(r)); if(n<0) return -1;
    per_keycode=r[1];
    if(per_keycode<1||per_keycode>8){ printf("XKEY_FAIL keysyms-per-keycode=%d\n",per_keycode); return -1; }
    if(n < 32 + count*per_keycode*4){ printf("XKEY_FAIL short keymap reply %d\n",n); return -1; }
    for(int i=0;i<count && i<256;i++)
        for(int j=0;j<per_keycode;j++)
            keysyms[i][j]=u32(r+32+(i*per_keycode+j)*4);
    return 0;
}
static int get_modifier_mapping(void){
    unsigned char b[4],*p=b; w8(&p,119); w8(&p,0); w16(&p,1);
    if(write(X,b,p-b)!=(int)(p-b)) return -1;
    unsigned char r[32+8*8];
    int n=read_reply(r,sizeof(r)); if(n<0) return -1;
    per_modifier=r[1];
    if(per_modifier<1||per_modifier>8){ printf("XKEY_FAIL keycodes-per-modifier=%d\n",per_modifier); return -1; }
    if(n < 32 + 8*per_modifier){ printf("XKEY_FAIL short modmap reply %d\n",n); return -1; }
    for(int m=0;m<8;m++) for(int k=0;k<per_modifier;k++) modmap[m][k]=r[32+m*per_modifier+k];
    return 0;
}
static unsigned lookup_keysym(unsigned keycode,int col){
    if(keycode<min_kc||keycode>max_kc) return 0;
    return keysyms[keycode-min_kc][col<per_keycode?col:0];
}
static int modifier_has(int mod,unsigned keycode){
    for(int k=0;k<per_modifier;k++) if(modmap[mod][k]==keycode) return 1;
    return 0;
}

/* Xlib's XLookupString, for the core (non-XKB) keymap this server exposes. */
static int lookup_string(unsigned keycode,unsigned state,char *out){
    int col = (state & SHIFT_MASK) ? 1 : 0;
    unsigned ks = lookup_keysym(keycode,col);
    unsigned lower = lookup_keysym(keycode,0);
    if((state & LOCK_MASK) && lower>='a' && lower<='z')
        ks = lookup_keysym(keycode,1);               /* Caps Lock: upper case */
    if(!ks) return 0;
    int c;
    if(ks>=0x20 && ks<=0xff) c=(int)ks;              /* Latin-1 is its own code */
    else if(ks==0xff08||ks==0xff09||ks==0xff0a||ks==0xff0d||ks==0xff1b||ks==0xffff)
        c=(int)(ks & 0x7f);                          /* BackSpace/Tab/Return/... */
    else return 0;                                   /* no character (arrows, F-keys) */
    if(state & CONTROL_MASK){
        if(c>='@' && c<0x7f) c &= 0x1f;
        else if(c==' ') c=0;
        else if(c=='2') c=0;
        else if(c>='3'&&c<='7') c-='3'-27;
        else if(c=='8') c=127;
        else if(c=='/') c=31;
    }
    *out=(char)c;
    return 1;
}

/* ── injection ───────────────────────────────────────────────────────────── */
static int fifo=-1;
static void inject(int code,int value,int mods){
    char line[48];
    int n=snprintf(line,sizeof(line),"k %d %d %d\n",code,value,mods);
    if(fifo>=0) write(fifo,line,n);
}

/* Read the next KeyPress (2) or KeyRelease (3), skipping other events. */
static int next_key_event(unsigned char *e){
    for(int i=0;i<64;i++){
        if(rd(X,e,32)!=32) return -1;
        if(e[0]==2||e[0]==3) return 0;
    }
    return -1;
}

static void check(const char *what,int ok){
    printf("  %-34s %s\n", what, ok?"ok":"FAIL");
    if(!ok) fails++;
}

/* Inject one key and assert the KeyPress it produces. */
static void expect_key(const char *what,unsigned win,int code,int mods,
                       int want_char,int want_c){
    unsigned char e[32];
    inject(code,1,mods);
    if(next_key_event(e)<0){ printf("  %-34s FAIL (no event)\n",what); fails++; return; }
    int ok = (e[0]==2) && (e[1]==(unsigned char)(code+8)) &&
             (u16(e+28)==(unsigned)mods) && (u32(e+12)==win);
    char ch=0;
    int got_char = lookup_string(e[1],u16(e+28),&ch);
    if(ok && (got_char!=want_char || (want_char && ch!=(char)want_c))) ok=0;
    if(want_char)
        printf("  %-34s keycode=%d state=0x%x char=0x%02x %s\n", what,
               e[1], u16(e+28), (unsigned char)ch, ok?"ok":"FAIL");
    else
        printf("  %-34s keycode=%d state=0x%x keysym=0x%04x %s\n", what,
               e[1], u16(e+28), lookup_keysym(e[1],0), ok?"ok":"FAIL");
    if(!ok) fails++;
    /* Every press is followed by its release. */
    inject(code,0,mods);
    if(next_key_event(e)<0 || e[0]!=3 || e[1]!=(unsigned char)(code+8)){
        printf("  %-34s FAIL (no matching KeyRelease)\n",what); fails++;
    }
}

int main(int argc,char**argv){
    int spawn=0; pid_t srv=0;
    for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--spawn")) spawn=1;
    if(spawn){
        srv=fork();
        if(srv==0){
            execl("/maerox","maerox","-H",(char*)0);
            execl("/disk/maerox","maerox","-H",(char*)0);
            _exit(127);
        }
    }
    for(int i=0;i<200&&(X=connect_x())<0;i++) usleep(20000);
    if(X<0){ printf("XKEY_FAIL connect\n"); if(srv)kill(srv,9); return 1; }
    if(handshake()<0){ printf("XKEY_FAIL handshake\n"); if(srv)kill(srv,9); return 1; }
    printf("xkey: keycode range %u..%u\n", min_kc, max_kc);

    unsigned w=alloc_id();
    create_window(w,40,40,320,200);
    map_window(w);
    set_input_focus(w);

    /* GetInputFocus must agree with the SetInputFocus just issued. */
    { unsigned char b[4],*p=b; w8(&p,43); w8(&p,0); w16(&p,1); write(X,b,p-b); }
    { unsigned char r[64]; int n=read_reply(r,sizeof(r));
      check("GetInputFocus == our window", n>=32 && u32(r+8)==w); }

    if(get_keyboard_mapping()<0){ printf("XKEY_FAIL GetKeyboardMapping\n"); if(srv)kill(srv,9); return 1; }
    if(get_modifier_mapping()<0){ printf("XKEY_FAIL GetModifierMapping\n"); if(srv)kill(srv,9); return 1; }
    printf("xkey: keysyms/keycode=%d keycodes/modifier=%d\n", per_keycode, per_modifier);

    check("keymap has 2+ keysyms/keycode", per_keycode>=2);
    check("keysym(A) == XK_a / XK_A",
          lookup_keysym(LK_A+8,0)==0x0061 && lookup_keysym(LK_A+8,1)==0x0041);
    check("keysym(1) == XK_1 / XK_exclam",
          lookup_keysym(LK_1+8,0)==0x0031 && lookup_keysym(LK_1+8,1)==0x0021);
    check("keysym(Enter) == XK_Return", lookup_keysym(LK_ENTER+8,0)==0xff0d);
    check("keysym(Left) == XK_Left",    lookup_keysym(LK_LEFT+8,0)==0xff51);
    check("Shift modifier -> Shift_L keycode",  modifier_has(0,LK_LEFTSHIFT+8));
    check("Lock modifier -> Caps_Lock keycode", modifier_has(1,LK_CAPSLOCK+8));
    check("Control modifier -> Control_L keycode", modifier_has(2,LK_LEFTCTRL+8));
    check("Mod1 modifier -> Alt_L keycode",     modifier_has(3,LK_LEFTALT+8));

    fifo=open(KEYFIFO,O_WRONLY);
    if(fifo<0){ printf("XKEY_FAIL cannot open %s\n",KEYFIFO); if(srv)kill(srv,9); return 1; }

    expect_key("plain a",              w, LK_A,        0,            1, 'a');
    expect_key("Shift+a",              w, LK_A,        SHIFT_MASK,   1, 'A');
    expect_key("CapsLock a",           w, LK_A,        LOCK_MASK,    1, 'A');
    expect_key("Shift+1",              w, LK_1,        SHIFT_MASK,   1, '!');
    expect_key("BackSpace",            w, LK_BACKSPACE,0,            1, 0x08);
    expect_key("Return",               w, LK_ENTER,    0,            1, 0x0d);
    expect_key("Ctrl+a",               w, LK_A,        CONTROL_MASK, 1, 0x01);
    expect_key("Ctrl+c",               w, LK_C,        CONTROL_MASK, 1, 0x03);
    expect_key("Ctrl+v",               w, LK_V,        CONTROL_MASK, 1, 0x16);
    expect_key("Alt+a",                w, LK_A,        MOD1_MASK,    1, 'a');
    expect_key("Left arrow (no char)",  w, LK_LEFT,     0,            0, 0);

    close(fifo);
    if(srv){ kill(srv,9); waitpid(srv,0,0); }
    if(fails){ printf("XKEY_FAIL %d check(s) failed\n",fails); return 1; }
    printf("XKEY_OK keyboard mapping + KeyPress/KeyRelease verified\n");
    return 0;
}
