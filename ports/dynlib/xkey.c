/* xkey — raw X11 client proving maeroX's keyboard path end to end.
 *
 *   xkey --spawn   spawn a headless maeroX, map a window, take the input focus,
 *                  read the keymap and the modifier map, inject keys through
 *                  maeroX's test channel and assert that each one arrives as a
 *                  KeyPress/KeyRelease with the right keycode and state AND
 *                  translates to the right character -> XKEY_OK.
 *
 * It also injects CLICKS, to prove that one does not take the keyboard away
 * from the window the client asked for - see click_focus_case().
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
#define LK_B 48
#define LK_C 46
#define LK_V 47
#define LK_RIGHTALT 100
#define LK_LEFTMETA 125
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
static void create_window_on(unsigned wid,unsigned parent,int x,int y,int w,int h){
    unsigned char b[64],*p=b; w8(&p,1); w8(&p,24); w16(&p,8);
    w32(&p,wid); w32(&p,parent); w16(&p,x); w16(&p,y); w16(&p,w); w16(&p,h);
    w16(&p,0); w16(&p,1); w32(&p,visual); w32(&p,0); write(X,b,p-b);
}
static void create_window(unsigned wid,int x,int y,int w,int h){ create_window_on(wid,root,x,y,w,h); }
static void map_window(unsigned wid){ unsigned char b[8],*p=b; w8(&p,8); w8(&p,0); w16(&p,2); w32(&p,wid); write(X,b,p-b); }
static void unmap_window(unsigned wid){ unsigned char b[8],*p=b; w8(&p,10); w8(&p,0); w16(&p,2); w32(&p,wid); write(X,b,p-b); }
/* A pixmap exists here only to occupy a resource slot and then give it back:
 * that is how this probe controls which of two windows lands in the lower slot
 * (see the click test below). */
static void create_pixmap(unsigned pid,int w,int h){
    unsigned char b[16],*p=b; w8(&p,53); w8(&p,24); w16(&p,4);
    w32(&p,pid); w32(&p,root); w16(&p,w); w16(&p,h); write(X,b,p-b);
}
static void free_pixmap(unsigned pid){ unsigned char b[8],*p=b; w8(&p,54); w8(&p,0); w16(&p,2); w32(&p,pid); write(X,b,p-b); }
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
/* A click at a screen position, delivered through the same on_x_click() the
 * desktop calls, so the hit-test and the focus policy under test are the real
 * ones and not a test-only copy. */
static void click(int x,int y){
    char line[48];
    int n=snprintf(line,sizeof(line),"c %d %d\n",x,y);
    if(fifo>=0) write(fifo,line,n);
}

/* ── press/release pairing ───────────────────────────────────────────────────
 * Every KeyPress the client receives must eventually be matched by a
 * KeyRelease for the SAME keycode on the SAME window, and no KeyRelease may
 * arrive without a preceding press.  A client that is left holding a key it
 * never released has a keyboard state that does not match reality: modifiers
 * stay stuck, shortcuts start firing on their own.  Every event this probe
 * reads goes through pair_note(), so the invariant is checked for the whole
 * run, not only where it is being tested on purpose. */
static unsigned held_win[256];      /* window that got the press, 0 = not held */
static int      pair_errors;

static void pair_note(const unsigned char *e){
    unsigned kc=e[1], win=u32(e+12);
    if(e[0]==2){
        if(held_win[kc])
            { printf("  PAIRING FAIL keycode=%u pressed twice with no release\n",kc); pair_errors++; }
        held_win[kc]=win;
    } else if(e[0]==3){
        if(!held_win[kc])
            { printf("  PAIRING FAIL keycode=%u released with no press\n",kc); pair_errors++; }
        else if(held_win[kc]!=win)
            { printf("  PAIRING FAIL keycode=%u pressed on 0x%x released on 0x%x\n",
                     kc, held_win[kc], win); pair_errors++; }
        held_win[kc]=0;
    }
}
static int pair_all_released(void){
    for(int i=0;i<256;i++) if(held_win[i]) return 0;
    return 1;
}

/* Read the next event of type t1 or t2, skipping the rest.  Every key event
 * that passes through goes to pair_note() whether or not it is the one being
 * waited for, so the pairing invariant covers the whole run. */
static int next_event(unsigned char *e,int t1,int t2){
    for(int i=0;i<64;i++){
        if(rd(X,e,32)!=32) return -1;
        if(e[0]==2||e[0]==3) pair_note(e);
        if(e[0]==t1||e[0]==t2) return 0;
    }
    return -1;
}
/* Read the next KeyPress (2) or KeyRelease (3), skipping other events. */
static int next_key_event(unsigned char *e){ return next_event(e,2,3); }

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

/* The window the server says has the keyboard, 0 if the read failed. */
static unsigned get_input_focus(void){
    unsigned char b[4],*p=b; w8(&p,43); w8(&p,0); w16(&p,1); write(X,b,p-b);
    unsigned char r[64];
    return read_reply(r,sizeof(r))>=32 ? u32(r+8) : 0;
}

/* ── a click must not steal the keyboard ─────────────────────────────────────
 * The shape Firefox makes: a full-screen toplevel the browser leaves blank, a
 * same-size child (the MozContainer) on top of it, and GTK holding the keyboard
 * on the TOPLEVEL via SetInputFocus.  A click in the page has to land on the
 * child - it is the window on top - while the keyboard stays where the client
 * put it.  It did not: the click path ran its own hit-test and then took the
 * focus unconditionally, so the child got a FocusIn that GDK discards (it only
 * tracks focus on toplevels) and the toplevel got a FocusOut that GDK does
 * process.  The browser then had every key addressed to a window it believed
 * was not focused.
 *
 * Whether that happened at all depended on which of the two landed in the
 * higher resource slot, which is why this runs twice with the slots arranged
 * both ways - see the call site. */
static void click_focus_case(const char *what,unsigned top,unsigned child){
    unsigned char e[32];
    char label[64];

    set_input_focus(top);                    /* the client claims the keyboard */
    /* Round-trip before injecting: requests travel over the socket and
     * injections over the FIFO, and nothing orders one against the other.  A
     * reply proves the server is past the SetInputFocus (and past the
     * CreateWindow/MapWindow before it). */
    unsigned before=get_input_focus();
    snprintf(label,sizeof(label),"%s: SetInputFocus took",what);
    check(label, before==top);

    click(640,400);                          /* inside both windows */
    unsigned btn_win=0;
    if(next_event(e,4,4)==0) btn_win=u32(e+12);          /* ButtonPress */
    snprintf(label,sizeof(label),"%s: click -> the child",what);
    check(label, btn_win==child);
    next_event(e,5,5);                                   /* its ButtonRelease */

    unsigned focus_win=get_input_focus();
    snprintf(label,sizeof(label),"%s: focus stays toplevel",what);
    check(label, focus_win==top);

    inject(LK_A,1,0);
    unsigned key_win=0;
    if(next_event(e,2,2)==0) key_win=u32(e+12);
    inject(LK_A,0,0);
    next_event(e,3,3);
    snprintf(label,sizeof(label),"%s: keys -> the toplevel",what);
    check(label, key_win==top);
    if(btn_win!=child || focus_win!=top || key_win!=top)
        printf("    (%s: button->0x%x focus->0x%x key->0x%x, "
               "toplevel=0x%x child=0x%x)\n",
               what,btn_win,focus_win,key_win,top,child);
}

/* The channel this probe injects through must not exist unless it was asked
 * for.  Before anything else, run a server the way the desktop runs one — no
 * -K — and check that no FIFO appears; then the -K server below has to create
 * one.  Two observations of the filesystem, not of a flag. */
static int check_channel_gate(void){
    pid_t srv=fork();
    if(srv==0){
        execl("/maerox","maerox","-H",(char*)0);
        execl("/disk/maerox","maerox","-H",(char*)0);
        _exit(127);
    }
    if(srv<0) return -1;
    unlink(KEYFIFO);                       /* clear anything left by an earlier run */
    int seen=0;
    for(int i=0;i<100;i++){                /* 2 s: well past the server's startup */
        int fd=connect_x();
        if(fd>=0){ close(fd); if(access(KEYFIFO,F_OK)==0) seen=1; break; }
        usleep(20000);
    }
    if(!seen && access(KEYFIFO,F_OK)==0) seen=1;
    kill(srv,9); waitpid(srv,0,0);
    return seen;
}

int main(int argc,char**argv){
    int spawn=0; pid_t srv=0;
    for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--spawn")) spawn=1;
    if(spawn){
        int gated=check_channel_gate();
        check("no -K: no injection channel", gated==0);
    }
    if(spawn){
        srv=fork();
        if(srv==0){
            /* -K opens the key-injection channel this probe writes to.  maeroX
             * refuses it unless it is also headless, so the desktop build never
             * has one; the probe has to ask for it explicitly. */
            execl("/maerox","maerox","-H","-K",(char*)0);
            execl("/disk/maerox","maerox","-H","-K",(char*)0);
            _exit(127);
        }
    }
    for(int i=0;i<200&&(X=connect_x())<0;i++) usleep(20000);
    if(X<0){ printf("XKEY_FAIL connect\n"); if(srv)kill(srv,9); return 1; }
    if(handshake()<0){ printf("XKEY_FAIL handshake\n"); if(srv)kill(srv,9); return 1; }
    printf("xkey: keycode range %u..%u\n", min_kc, max_kc);

    unsigned w=alloc_id(), w2=alloc_id();
    create_window(w,40,40,320,200);
    map_window(w);
    create_window(w2,40,260,320,200);
    map_window(w2);
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
    check("Mod1 modifier -> Alt_R keycode",     modifier_has(3,LK_RIGHTALT+8));
    check("Mod4 modifier -> Super_L keycode",   modifier_has(6,LK_LEFTMETA+8));
    check("keysym(Super_L) == XK_Super_L",      lookup_keysym(LK_LEFTMETA+8,0)==0xffeb);

    check("with -K: the channel exists", access(KEYFIFO,F_OK)==0);
    /* maeroX creates the FIFO at startup; give it a moment if we got here first. */
    for(int i=0;i<200 && (fifo=open(KEYFIFO,O_WRONLY))<0;i++) usleep(20000);
    if(fifo<0){
        printf("XKEY_FAIL cannot open %s (was maeroX started with -K?)\n",KEYFIFO);
        if(srv) kill(srv,9);
        return 1;
    }

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
    /* Right Alt must set Mod1Mask, the mask the modifier map advertises for it.
     * The keycode travelling as 108 is the other half: while the kernel dropped
     * the E0 38 scancode outright, this keycode could not exist at all. */
    expect_key("Right Alt is a key",   w, LK_RIGHTALT, 0,            0, 0);
    expect_key("a with Right Alt held", w, LK_A,       MOD1_MASK,    1, 'a');

    /* A release goes to the window that got the press, even when the focus has
     * moved in between.  This is the invariant that Alt-Tab used to break. */
    {
        unsigned char e[32];
        int ok;
        inject(LK_A,1,0);                      /* press while w has the focus */
        ok = next_key_event(e)==0 && e[0]==2 && u32(e+12)==w;
        set_input_focus(w2);                   /* focus moves mid-keystroke */
        inject(LK_A,0,0);
        ok = ok && next_key_event(e)==0 && e[0]==3 && u32(e+12)==w;
        check("release follows its press window", ok);
        /* And the NEXT press does go to the new focus. */
        inject(LK_A,1,0);
        ok = next_key_event(e)==0 && e[0]==2 && u32(e+12)==w2;
        inject(LK_A,0,0);
        ok = ok && next_key_event(e)==0 && e[0]==3 && u32(e+12)==w2;
        check("next press follows the new focus", ok);
        set_input_focus(w);
    }

    /* A release whose press was never delivered must produce nothing at all.
     * If a stray release were sent it would arrive before the press that
     * follows it, so reading one event is enough to tell. */
    {
        unsigned char e[32];
        int ok;
        inject(LK_B,0,0);                      /* release with no press */
        inject(LK_B,1,0);
        ok = next_key_event(e)==0 && e[0]==2 && e[1]==(unsigned char)(LK_B+8);
        inject(LK_B,0,0);
        ok = ok && next_key_event(e)==0 && e[0]==3;
        check("unmatched release is not sent", ok);
    }

    /* ── the click/focus shape, with the resource slots arranged both ways ──
     * maeroX allocates a resource in the first free slot, so this probe can put a
     * toplevel and its same-size child on either side of each other in the
     * array while creating them in a fixed order.  Both arrangements must give
     * the same answer, because "topmost" is create order and nothing else.
     *
     * First: child created after the toplevel and landing ABOVE it in the
     * array.  Scanning slots and keeping the last match picks the child here,
     * which is what used to hand it the keyboard.
     *
     * A toplevel is created small and mapped: maeroX's kiosk WM resizes any
     * real top-level to fill the screen, exactly as it does for Firefox.  The
     * child is created at full size so it is not resized again. */
    unsigned top1=alloc_id(), child1=alloc_id();
    create_window(top1,0,0,600,400);
    map_window(top1);
    create_window_on(child1,top1,0,0,1280,800);
    map_window(child1);
    click_focus_case("child above",top1,child1);

    /* Second: the same shape with the child BELOW the toplevel in the array.
     * A pixmap takes the next free slot, the toplevel takes the one after it,
     * the pixmap is freed, and the child drops into the hole - so the child is
     * created last but sits in the lower slot.  Scanning slots picks the
     * TOPLEVEL here, so this arrangement is the one that catches a hit-test
     * still going by slot order: the button must go to the child regardless. */
    unmap_window(top1);
    unmap_window(child1);
    unsigned pad=alloc_id(), top2=alloc_id(), child2=alloc_id();
    create_pixmap(pad,16,16);
    create_window(top2,0,0,600,400);
    map_window(top2);
    free_pixmap(pad);
    create_window_on(child2,top2,0,0,1280,800);
    map_window(child2);
    click_focus_case("child below",top2,child2);

    check("every key released at the end", pair_all_released());
    check("no pairing violations",         pair_errors==0);

    close(fifo);
    if(srv){ kill(srv,9); waitpid(srv,0,0); }
    if(fails){ printf("XKEY_FAIL %d check(s) failed\n",fails); return 1; }
    printf("XKEY_OK keyboard mapping + KeyPress/KeyRelease verified\n");
    return 0;
}
