/* xmal — raw X11 client that throws malformed requests at maeroX and checks the
 * server answers each with an X error (or drops only the offending client) and
 * keeps serving everybody else.  Prints XMAL_OK when every case passed.
 *
 *   xmal --spawn   fork+exec a headless maeroX first (for smoke tests)
 *
 * Cases, in order, all against ONE server instance:
 *   short       a request below its minimum length       -> BadLength
 *   putimage    image rows beyond the request length     -> BadLength
 *   getimage    w*h that overflowed the old int check    -> error, no crash
 *   ids         reused id / GC request on a window       -> BadIDChoice / BadGC
 *   clip        65535x65535 fills far outside a window   -> finish in < 3 s
 *   glyphs      AddGlyphs replaces an id, FreeGlyphs removes it
 *   len0        a zero-length request                    -> that client dropped
 *   msb         an MSB-first setup                       -> setup Failed
 *   exitmid     a client exits with replies pending      -> server survives
 *   stall       a client never reads 20+ MB of replies   -> others unaffected
 *   (dos runs right after glyphs, while id_base is still this client's)
 *   dos         CopyGC onto itself with a freed tile     -> BadMatch, no crash
 *               a GC's font set to a GC (itself)         -> BadFont
 *               ConfigureWindow with sibling = itself    -> BadMatch
 *               ListFonts "********************Z"        -> reply in < 3 s
 *               windows nested 300 deep                  -> BadAlloc, no crash
 *               PolyLine of long segments off-window     -> done in < 3 s
 * and afterwards a normal client still draws: this connection round-trips a
 * fill through GetImage, then xdraw and the real-Xlib xreal run against the
 * same server (xreal's own server cannot bind the socket, so it uses ours).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static unsigned id_base, id_mask, root, visual, next_id = 1;
static unsigned render_major;
static int failures;

static unsigned u16(const unsigned char *p){ return p[0]|(p[1]<<8); }
static unsigned u32(const unsigned char *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((unsigned)p[3]<<24); }
static void w8(unsigned char **p, unsigned v){ *(*p)++ = (unsigned char)v; }
static void w16(unsigned char **p, unsigned v){ w8(p,v); w8(p,v>>8); }
static void w32(unsigned char **p, unsigned v){ w8(p,v); w8(p,v>>8); w8(p,v>>16); w8(p,v>>24); }
static unsigned alloc_id(void){ return id_base | (next_id++ & id_mask); }

static long now_ms(void){
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

#define FAIL(...) do { printf("XMAL_FAIL " __VA_ARGS__); printf("\n"); \
                       fflush(stdout); failures++; } while (0)

/* Non-blocking fds + a deadline: a server that hangs shows up as a failed
 * case instead of a probe blocked forever. */
static int rd_to(int fd, unsigned char *b, int n, int ms){
    int g = 0;
    long end = now_ms() + ms;
    for (int spins = 0; g < n; spins++) {
        int r = read(fd, b + g, n - g);
        if (r > 0) { g += r; continue; }
        if (r == 0) return g;                   /* EOF */
        if (errno != EAGAIN && errno != EINTR) return -1;
        long t = now_ms();
        if ((t >= 0 && t > end) || (t < 0 && spins > ms)) return g;
        usleep(1000);
    }
    return g;
}
static int wr(int fd, const void *p, int n){
    const unsigned char *b = p;
    int g = 0;
    for (int spins = 0; g < n && spins < 20000; spins++) {
        int r = write(fd, b + g, n - g);
        if (r > 0) g += r;
        else if (r < 0 && errno != EAGAIN && errno != EINTR) return -1;
        else usleep(500);
    }
    return g;
}

static int connect_x(void){
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a; memset(&a,0,sizeof(a));
    a.sun_family = AF_UNIX; strcpy(a.sun_path, "/tmp/.X11-unix/X0");
    if (connect(fd,(struct sockaddr*)&a, sizeof(a.sun_family)+strlen(a.sun_path))==0) {
        fcntl(fd, F_SETFL, O_RDWR | O_NONBLOCK);
        return fd;
    }
    close(fd); return -1;
}

static int handshake(int X){
    unsigned char req[12]={0}; req[0]='l'; req[2]=11;
    if (wr(X,req,12)!=12) return -1;
    unsigned char h[8]; if (rd_to(X,h,8,5000)!=8 || h[0]!=1) return -1;
    unsigned extra = u16(h+6)*4;
    unsigned char body[1024]; if (extra>sizeof(body)) return -1;
    if (rd_to(X,body,(int)extra,5000)!=(int)extra) return -1;
    id_base = u32(body+4); id_mask = u32(body+8);
    unsigned vlen=u16(body+16);
    unsigned off = 32 + ((vlen+3)&~3u) + body[21]*8;   /* pixmap formats: 8 bytes each */
    root   = u32(body+off);
    visual = u32(body+off+32);
    return 0;
}
static int open_client(void){
    int X = -1;
    for (int i=0;i<200 && (X=connect_x())<0;i++) usleep(20000);
    if (X < 0) return -1;
    if (handshake(X) < 0) { close(X); return -1; }
    return X;
}

/* Read the next reply or error, skipping events.  Returns the 32-byte packet's
 * type (0 error, 1 reply) with the packet in pkt, or -1 on timeout/EOF.  The
 * extra data of a reply is read and discarded (up to `keep` bytes into data). */
static int next_packet(int X, unsigned char pkt[32], unsigned char *data, int keep, int ms){
    for (int i = 0; i < 256; i++) {
        if (rd_to(X, pkt, 32, ms) != 32) return -1;
        if (pkt[0] == 0) return 0;
        if (pkt[0] == 1) {
            unsigned extra = u32(pkt+4) * 4;
            unsigned char sink[4096];
            unsigned got = 0;
            while (got < extra) {
                unsigned chunk = extra - got; if (chunk > sizeof(sink)) chunk = sizeof(sink);
                if (rd_to(X, sink, (int)chunk, ms) != (int)chunk) return -1;
                if (data && got < (unsigned)keep) {
                    unsigned c = chunk; if (got + c > (unsigned)keep) c = keep - got;
                    memcpy(data + got, sink, c);
                }
                got += chunk;
            }
            return 1;
        }
    }
    return -1;
}

/* GetInputFocus round trip: proves the server is alive and has processed
 * everything sent before it.  Returns the elapsed ms (0 if no clock), -1 on
 * failure. */
static long sync_x(int X, int ms){
    long t0 = now_ms();
    unsigned char b[4] = {43, 0, 1, 0};
    if (wr(X, b, 4) != 4) return -1;
    unsigned char pkt[32];
    for (;;) {
        int t = next_packet(X, pkt, 0, 0, ms);
        if (t < 0) return -1;
        if (t == 1) break;
        /* errors from earlier requests are not this sync's business */
    }
    long t1 = now_ms();
    return (t0 >= 0 && t1 >= 0) ? t1 - t0 : 0;
}

/* Expect the next reply/error to be error `code`; then sync. */
static void expect_error(int X, int code, const char *what){
    unsigned char pkt[32];
    int t = next_packet(X, pkt, 0, 0, 5000);
    if (t != 0) FAIL("%s: expected error %d, got %s", what, code,
                     t == 1 ? "a reply" : "nothing (server gone?)");
    else if (pkt[1] != code) FAIL("%s: expected error %d, got error %d", what, code, pkt[1]);
    else printf("xmal: %s -> error %d ok\n", what, code);
    if (sync_x(X, 5000) < 0) FAIL("%s: server did not answer afterwards", what);
}

/* ── request builders ─────────────────────────────────────────────────────── */
static void create_window(int X, unsigned wid, int w, int h){
    unsigned char b[32], *p=b;
    w8(&p,1); w8(&p,24); w16(&p,8);
    w32(&p,wid); w32(&p,root);
    w16(&p,0); w16(&p,0); w16(&p,w); w16(&p,h);
    w16(&p,0); w16(&p,1);
    w32(&p,visual); w32(&p,0);
    wr(X,b,32);
}
static void create_gc(int X, unsigned gc, unsigned d, unsigned fg){
    unsigned char b[20], *p=b;
    w8(&p,55); w8(&p,0); w16(&p,5);
    w32(&p,gc); w32(&p,d); w32(&p,0x4); w32(&p,fg);
    wr(X,b,20);
}
static void change_gc_fg(int X, unsigned gc, unsigned fg){
    unsigned char b[16], *p=b;
    w8(&p,56); w8(&p,0); w16(&p,4);
    w32(&p,gc); w32(&p,0x4); w32(&p,fg);
    wr(X,b,16);
}
static void fill_rect(int X, unsigned d, unsigned gc, int x, int y, int w, int h){
    unsigned char b[20], *p=b;
    w8(&p,70); w8(&p,0); w16(&p,5);
    w32(&p,d); w32(&p,gc);
    w16(&p,x); w16(&p,y); w16(&p,w); w16(&p,h);
    wr(X,b,20);
}
static void get_image_req(int X, unsigned d, int x, int y, int w, int h){
    unsigned char b[20], *p=b;
    w8(&p,73); w8(&p,2); w16(&p,5);
    w32(&p,d); w16(&p,x); w16(&p,y); w16(&p,w); w16(&p,h); w32(&p,0xFFFFFFFF);
    wr(X,b,20);
}
/* One pixel via GetImage; returns -1 on failure. */
static long get_pixel(int X, unsigned d, int x, int y){
    get_image_req(X, d, x, y, 1, 1);
    unsigned char pkt[32], px[4] = {0};
    if (next_packet(X, pkt, px, 4, 5000) != 1) return -1;
    return (long)(u32(px) & 0x00FFFFFF);
}

static unsigned query_render(int X){
    unsigned char b[16], *p=b;
    w8(&p,98); w8(&p,0); w16(&p,4); w16(&p,6); w16(&p,0);
    memcpy(p, "RENDER\0\0", 8); p += 8;
    wr(X,b,16);
    unsigned char pkt[32];
    if (next_packet(X, pkt, 0, 0, 5000) != 1 || !pkt[8]) return 0;
    return pkt[9];
}

/* ── cases ────────────────────────────────────────────────────────────────── */
static void case_short(int X){
    /* CreateWindow with length 2 (8 bytes): 24 bytes of fields missing. */
    unsigned char b[8], *p=b;
    w8(&p,1); w8(&p,24); w16(&p,2); w32(&p,alloc_id());
    wr(X,b,8);
    expect_error(X, 16, "short CreateWindow");
    /* CopyArea with length 3 (fields up to 28 bytes). */
    unsigned char c[12] = {62,0,3,0, 1,0,0,0, 1,0,0,0};
    wr(X,c,12);
    expect_error(X, 16, "short CopyArea");
    if (render_major) {
        /* RENDER Composite with length 2 instead of 9. */
        unsigned char r[8] = {(unsigned char)render_major, 8, 2, 0, 3,0,0,0};
        wr(X,r,8);
        expect_error(X, 16, "short RENDER Composite");
        /* AddGlyphs claiming 8000 glyphs in a 16-byte request. */
        unsigned gs = alloc_id();
        unsigned char g[12], *q=g;
        w8(&q,render_major); w8(&q,17); w16(&q,3); w32(&q,gs); w32(&q,0x33);
        wr(X,g,12);
        unsigned char a[16], *s=a;
        w8(&s,render_major); w8(&s,20); w16(&s,4); w32(&s,gs); w32(&s,8000); w32(&s,1);
        wr(X,a,16);
        expect_error(X, 16, "AddGlyphs count beyond request");
    }
}

static void case_putimage(int X, unsigned win, unsigned gc){
    /* 100x1000 ZPixmap image with only one row of data behind the header. */
    int iw = 100, ih = 1000;
    int len = (24 + iw*4) / 4;
    unsigned char *b = calloc(1, (size_t)len*4), *p = b;
    w8(&p,72); w8(&p,2); w16(&p,len);
    w32(&p,win); w32(&p,gc);
    w16(&p,iw); w16(&p,ih); w16(&p,0); w16(&p,0);
    w8(&p,0); w8(&p,24); w16(&p,0);
    wr(X,b,len*4);
    free(b);
    expect_error(X, 16, "PutImage rows beyond request");
    /* 65535x65535 in a bare 24-byte request (the i686 wrap case). */
    unsigned char h[24], *q=h;
    w8(&q,72); w8(&q,2); w16(&q,6);
    w32(&q,win); w32(&q,gc);
    w16(&q,65535); w16(&q,65535); w16(&q,0); w16(&q,0);
    w8(&q,0); w8(&q,24); w16(&q,0);
    wr(X,h,24);
    expect_error(X, 16, "PutImage 65535x65535 header only");
}

static void case_getimage(int X, unsigned win){
    get_image_req(X, win, 0, 0, 46341, 46341);   /* product wraps int32 */
    expect_error(X, 11, "GetImage 46341x46341");
    get_image_req(X, win, 0, 0, 65535, 65535);
    expect_error(X, 11, "GetImage 65535x65535");
}

static void case_ids(int X, unsigned win, unsigned gc){
    create_window(X, win, 10, 10);               /* win is already in use */
    expect_error(X, 14, "CreateWindow reused id");
    create_gc(X, gc, win, 0);                    /* gc too */
    expect_error(X, 14, "CreateGC reused id");
    change_gc_fg(X, win, 0x123456);              /* ChangeGC on a window */
    expect_error(X, 13, "ChangeGC on a window");
    unsigned char f[8], *p=f; w8(&p,60); w8(&p,0); w16(&p,2); w32(&p,win);
    wr(X,f,8);                                   /* FreeGC on a window */
    expect_error(X, 13, "FreeGC on a window");
    /* The window must be intact: geometry still 64x64. */
    unsigned char g[8], *q=g; w8(&q,14); w8(&q,0); w16(&q,2); w32(&q,win);
    wr(X,g,8);
    unsigned char pkt[32];
    if (next_packet(X, pkt, 0, 0, 5000) != 1 || u16(pkt+16) != 64 || u16(pkt+18) != 64)
        FAIL("window damaged by reused-id / GC requests");
    else printf("xmal: window intact after id misuse ok\n");
}

static void case_clip(int X, unsigned win, unsigned gc){
    /* 256 rectangles of 65535x65535 starting at -32768: before clipping this
     * was ~1.1e12 per-pixel iterations. */
    enum { NR = 256 };
    int len = 3 + NR*2;
    unsigned char *b = malloc((size_t)len*4), *p = b;
    change_gc_fg(X, gc, 0x00336699);
    w8(&p,70); w8(&p,0); w16(&p,len); w32(&p,win); w32(&p,gc);
    for (int i = 0; i < NR; i++) { w16(&p,-32768); w16(&p,-32768); w16(&p,65535); w16(&p,65535); }
    long t0 = now_ms();
    wr(X,b,len*4);
    free(b);
    /* ClearArea and CopyArea with the same huge extents. */
    unsigned char ca[16], *q=ca;
    w8(&q,61); w8(&q,0); w16(&q,4); w32(&q,win); w16(&q,-32768); w16(&q,-32768); w16(&q,65535); w16(&q,65535);
    wr(X,ca,16);
    fill_rect(X, win, gc, -32768, -32768, 65535, 65535);
    unsigned char cp[28], *r=cp;
    w8(&r,62); w8(&r,0); w16(&r,7); w32(&r,win); w32(&r,win); w32(&r,gc);
    w16(&r,-32768); w16(&r,-32768); w16(&r,-32000); w16(&r,-32000); w16(&r,65535); w16(&r,65535);
    wr(X,cp,28);
    if (render_major) {
        unsigned pic = alloc_id(), solid = alloc_id();
        unsigned char cpic[20], *s=cpic;
        w8(&s,render_major); w8(&s,4); w16(&s,5); w32(&s,pic); w32(&s,win); w32(&s,0x30); w32(&s,0);
        wr(X,cpic,20);
        unsigned char sf[16], *t=sf;
        w8(&t,render_major); w8(&t,33); w16(&t,4); w32(&t,solid);
        w16(&t,0x3333); w16(&t,0x6666); w16(&t,0x9999); w16(&t,0xFFFF);
        wr(X,sf,16);
        unsigned char comp[36], *u=comp;     /* Composite Over, 65535^2 */
        w8(&u,render_major); w8(&u,8); w16(&u,9); w8(&u,3); w8(&u,0); w16(&u,0);
        w32(&u,solid); w32(&u,0); w32(&u,pic);
        w16(&u,0); w16(&u,0); w16(&u,0); w16(&u,0); w16(&u,-32768); w16(&u,-32768);
        w16(&u,65535); w16(&u,65535);
        wr(X,comp,36);
        int flen = 5 + 64*2;                 /* FillRectangles, 64 huge rects */
        unsigned char *fr = malloc((size_t)flen*4), *v = fr;
        w8(&v,render_major); w8(&v,26); w16(&v,flen); w8(&v,1); w8(&v,0); w16(&v,0);
        w32(&v,pic); w16(&v,0x3333); w16(&v,0x6666); w16(&v,0x9999); w16(&v,0xFFFF);
        for (int i = 0; i < 64; i++) { w16(&v,-32768); w16(&v,-32768); w16(&v,65535); w16(&v,65535); }
        wr(X,fr,flen*4);
        free(fr);
    }
    long el = sync_x(X, 30000);
    long t1 = now_ms();
    if (el < 0) { FAIL("huge clipped fills: server did not answer within 30 s"); return; }
    long total = (t0 >= 0 && t1 >= 0) ? t1 - t0 : 0;
    if (total > 3000) FAIL("huge clipped fills took %ld ms", total);
    else printf("xmal: huge clipped fills done in %ld ms ok\n", total);
    long px = get_pixel(X, win, 10, 10);
    if (px != 0x336699) FAIL("clipped fill pixel = 0x%lx, want 0x336699", px);
}

static void add_glyph(int X, unsigned gs, unsigned id, unsigned char cov){
    unsigned char b[32], *p=b;              /* 1 glyph, 1x1 A8, stride 4 */
    w8(&p,render_major); w8(&p,20); w16(&p,8);
    w32(&p,gs); w32(&p,1); w32(&p,id);
    w16(&p,1); w16(&p,1); w16(&p,0); w16(&p,0); w16(&p,1); w16(&p,0);
    w8(&p,cov); w8(&p,0); w8(&p,0); w8(&p,0);
    wr(X,b,32);
}
static void draw_glyph(int X, unsigned src, unsigned pic, unsigned gs, unsigned id){
    unsigned char b[40], *p=b;              /* CompositeGlyphs8, one element */
    w8(&p,render_major); w8(&p,23); w16(&p,10);
    w8(&p,3); w8(&p,0); w16(&p,0);
    w32(&p,src); w32(&p,pic); w32(&p,0); w32(&p,gs); w16(&p,0); w16(&p,0);
    w8(&p,1); w8(&p,0); w16(&p,0); w16(&p,2); w16(&p,2);   /* count, dx, dy */
    w8(&p,id); w8(&p,0); w8(&p,0); w8(&p,0);
    wr(X,b,40);
}
static void case_glyphs(int X, unsigned gc){
    if (!render_major) { FAIL("RENDER not advertised"); return; }
    unsigned win = alloc_id(), pic = alloc_id(), src = alloc_id(), gs = alloc_id();
    create_window(X, win, 16, 16);
    unsigned char cpic[20], *s=cpic;
    w8(&s,render_major); w8(&s,4); w16(&s,5); w32(&s,pic); w32(&s,win); w32(&s,0x30); w32(&s,0);
    wr(X,cpic,20);
    unsigned char sf[16], *t=sf;             /* opaque white */
    w8(&t,render_major); w8(&t,33); w16(&t,4); w32(&t,src);
    w16(&t,0xFFFF); w16(&t,0xFFFF); w16(&t,0xFFFF); w16(&t,0xFFFF);
    wr(X,sf,16);
    unsigned char cg[12], *u=cg;
    w8(&u,render_major); w8(&u,17); w16(&u,3); w32(&u,gs); w32(&u,0x33);
    wr(X,cg,12);

    change_gc_fg(X, gc, 0);
    fill_rect(X, win, gc, 0, 0, 16, 16);
    add_glyph(X, gs, 7, 255);
    draw_glyph(X, src, pic, gs, 7);
    long a = get_pixel(X, win, 2, 2);
    fill_rect(X, win, gc, 0, 0, 16, 16);
    add_glyph(X, gs, 7, 0);                  /* replace id 7: empty coverage */
    draw_glyph(X, src, pic, gs, 7);
    long b = get_pixel(X, win, 2, 2);
    add_glyph(X, gs, 7, 255);
    unsigned char fg[12], *v=fg;             /* FreeGlyphs(gs, 7) */
    w8(&v,render_major); w8(&v,22); w16(&v,3); w32(&v,gs); w32(&v,7);
    wr(X,fg,12);
    draw_glyph(X, src, pic, gs, 7);
    long c = get_pixel(X, win, 2, 2);
    unsigned char fs[8], *w=fs;              /* FreeGlyphSet, then use it */
    w8(&w,render_major); w8(&w,19); w16(&w,2); w32(&w,gs);
    wr(X,fs,8);
    draw_glyph(X, src, pic, gs, 7);
    if (sync_x(X, 5000) < 0) { FAIL("glyphs: server gone"); return; }
    if (a != 0xFFFFFF) FAIL("glyph drawn = 0x%lx, want 0xffffff", a);
    else if (b != 0) FAIL("replaced glyph still drew the old bitmap (0x%lx)", b);
    else if (c != 0) FAIL("freed glyph still drew (0x%lx)", c);
    else printf("xmal: glyph replace/free ok\n");
}

static void case_len0(int X){
    int Y = open_client();
    if (Y < 0) { FAIL("len0: cannot connect"); return; }
    unsigned char b[8] = {127, 0, 0, 0, 0, 0, 0, 0};
    wr(Y, b, 8);
    unsigned char pkt[32];
    int t = next_packet(Y, pkt, 0, 0, 5000);
    int eof = 0;
    for (int i = 0; i < 50 && !eof; i++) { unsigned char z; if (rd_to(Y, &z, 1, 100) == 0) eof = 1; }
    close(Y);
    if (t != 0 || pkt[1] != 16) FAIL("len0: no BadLength");
    else if (!eof) FAIL("len0: client not disconnected");
    else printf("xmal: zero-length request -> BadLength + drop ok\n");
    if (sync_x(X, 5000) < 0) FAIL("len0: server gone");
}

static void case_msb(int X){
    int Y = -1;
    for (int i=0;i<50 && (Y=connect_x())<0;i++) usleep(20000);
    if (Y < 0) { FAIL("msb: cannot connect"); return; }
    unsigned char req[12]={0}; req[0]='B'; req[3]=11;
    wr(Y, req, 12);
    unsigned char h[8];
    int n = rd_to(Y, h, 8, 5000);
    close(Y);
    if (n != 8 || h[0] != 0) FAIL("msb: setup not refused");
    else printf("xmal: MSB-first setup refused ok\n");
    if (sync_x(X, 5000) < 0) FAIL("msb: server gone");
}

static unsigned make_big_window(int Y, int w, int h){
    unsigned win = alloc_id();
    create_window(Y, win, w, h);
    return win;
}
static void case_exitmid(int X){
    for (int round = 0; round < 3; round++) {
        int Y = open_client();
        if (Y < 0) { FAIL("exitmid: cannot connect"); return; }
        unsigned win = make_big_window(Y, 256, 256);
        /* 16 x 256 KiB GetImage replies, then vanish without reading. */
        unsigned char b[16*20], *p=b;
        for (int i = 0; i < 16; i++) {
            w8(&p,73); w8(&p,2); w16(&p,5); w32(&p,win);
            w16(&p,0); w16(&p,0); w16(&p,256); w16(&p,256); w32(&p,0xFFFFFFFF);
        }
        wr(Y, b, sizeof(b));
        close(Y);
    }
    if (sync_x(X, 5000) < 0) FAIL("exitmid: server died or hung");
    else printf("xmal: clients exiting mid-reply ok\n");
}
/* A client that never reads: 24 x 1 MiB of replies (over the 20 MiB queue
 * cap) — the server must neither block on it nor stop serving X. */
static int case_stall(int X){
    int Y = open_client();
    if (Y < 0) { FAIL("stall: cannot connect"); return -1; }
    unsigned win = make_big_window(Y, 512, 512);
    unsigned char b[24*20], *p=b;
    for (int i = 0; i < 24; i++) {
        w8(&p,73); w8(&p,2); w16(&p,5); w32(&p,win);
        w16(&p,0); w16(&p,0); w16(&p,512); w16(&p,512); w32(&p,0xFFFFFFFF);
    }
    wr(Y, b, sizeof(b));
    /* A second staller that stays under the cap and stays connected. */
    int Z = open_client();
    if (Z >= 0) {
        unsigned w2 = make_big_window(Z, 1024, 1024);
        get_image_req(Z, w2, 0, 0, 1024, 1024);
    }
    long el = sync_x(X, 10000);
    if (el < 0) FAIL("stall: server blocked on a client that does not read");
    else if (el > 3000) FAIL("stall: round trip took %ld ms", el);
    else printf("xmal: non-reading clients do not block the server ok\n");
    close(Y);
    return Z;                                 /* kept open through the end */
}


/* Requests a single client used to crash or hang the whole server with. */
static void create_window_in(int X, unsigned wid, unsigned parent){
    unsigned char b[32], *p=b;
    w8(&p,1); w8(&p,24); w16(&p,8);
    w32(&p,wid); w32(&p,parent);
    w16(&p,0); w16(&p,0); w16(&p,4); w16(&p,4);
    w16(&p,0); w16(&p,1);
    w32(&p,visual); w32(&p,0);
    wr(X,b,32);
}
static void destroy_window(int X, unsigned wid){
    unsigned char b[8], *p=b; w8(&p,4); w8(&p,0); w16(&p,2); w32(&p,wid); wr(X,b,8);
}
static void case_dos(int X, unsigned win){
    /* 4: tile pixmap freed while the GC holds it, then CopyGC(gc, gc). */
    unsigned pm = alloc_id(), g = alloc_id();
    unsigned char b[64], *p;
    p=b; w8(&p,53); w8(&p,24); w16(&p,4); w32(&p,pm); w32(&p,win); w16(&p,8); w16(&p,8); wr(X,b,16);
    create_gc(X, g, win, 0x00112233);
    p=b; w8(&p,56); w8(&p,0); w16(&p,4); w32(&p,g); w32(&p,0x400); w32(&p,pm); wr(X,b,16);
    p=b; w8(&p,54); w8(&p,0); w16(&p,2); w32(&p,pm); wr(X,b,8);
    p=b; w8(&p,57); w8(&p,0); w16(&p,4); w32(&p,g); w32(&p,g); w32(&p,0x400); wr(X,b,16);
    expect_error(X, 8, "CopyGC onto itself");
    p=b; w8(&p,56); w8(&p,0); w16(&p,5); w32(&p,g); w32(&p,0x500); w32(&p,1); w32(&p,0); wr(X,b,20);
    fill_rect(X, win, g, 0, 0, 8, 8);          /* tiled fill through the kept tile */
    if (sync_x(X, 5000) < 0) FAIL("server gone after CopyGC onto itself");
    /* 5: the GC's font set to a GC id (its own): no font, no recursion. */
    p=b; w8(&p,56); w8(&p,0); w16(&p,4); w32(&p,g); w32(&p,0x4000); w32(&p,g); wr(X,b,16);
    expect_error(X, 7, "GC font = the GC itself");
    p=b; w8(&p,47); w8(&p,0); w16(&p,2); w32(&p,g); wr(X,b,8);   /* QueryFont on the GC */
    if (sync_x(X, 5000) < 0) FAIL("server gone after QueryFont on a GC");
    p=b; w8(&p,60); w8(&p,0); w16(&p,2); w32(&p,g); wr(X,b,8);
    /* 6: ConfigureWindow with the window as its own sibling. */
    unsigned w2 = alloc_id();
    create_window_in(X, w2, root);
    p=b; w8(&p,12); w8(&p,0); w16(&p,5); w32(&p,w2); w16(&p,0x60); w16(&p,0);
    w32(&p,w2); w32(&p,0); wr(X,b,20);
    expect_error(X, 8, "ConfigureWindow sibling = itself");
    destroy_window(X, w2);
    if (sync_x(X, 5000) < 0) FAIL("server gone after the self-sibling window");
    /* 7: a pattern with 20 stars against every font name. */
    const char *pat = "********************Z";
    int n = (int)strlen(pat), len = 2 + (n + 3) / 4;
    memset(b, 0, sizeof(b));
    p=b; w8(&p,49); w8(&p,0); w16(&p,len); w16(&p,10); w16(&p,n); memcpy(p, pat, n);
    long t0 = now_ms();
    wr(X,b,len*4);
    unsigned char pkt[32];
    if (next_packet(X, pkt, 0, 0, 5000) != 1) FAIL("ListFonts star pattern: no reply");
    else if (now_ms() - t0 > 3000) FAIL("ListFonts star pattern took %ld ms", now_ms() - t0);
    else printf("xmal: ListFonts with 20 stars in %ld ms ok\n", now_ms() - t0);
    /* 8: nesting 300 deep: refused somewhere, the server survives, and
     * destroying the chain (a recursive walk) is fine. */
    unsigned top = alloc_id(), parent = top, refused = 0;
    create_window_in(X, top, root);
    for (int i = 0; i < 300; i++) { unsigned c2 = alloc_id(); create_window_in(X, c2, parent); parent = c2; }
    for (int i = 0; i < 300; i++) {
        int t = next_packet(X, pkt, 0, 0, 300);
        if (t < 0) break;
        if (t == 0 && pkt[1] == 11) refused++;
    }
    if (!refused) FAIL("300-deep window nesting was not refused");
    else printf("xmal: deep nesting refused (%u BadAlloc) ok\n", refused);
    destroy_window(X, top);
    if (sync_x(X, 5000) < 0) FAIL("server gone after the deep window chain");
    /* 9: 2000 segments of +-32000 pixels, all far off the window. */
    int np = 2000, plen = 3 + np;
    unsigned char *pl = malloc((size_t)plen * 4);
    p = pl; w8(&p,65); w8(&p,0); w16(&p,plen); w32(&p,win); w32(&p,g = alloc_id());
    for (int i = 0; i < np; i++) { w16(&p, (unsigned)(i & 1 ? 32000 : -32000)); w16(&p, (unsigned)-30000); }
    create_gc(X, g, win, 0x00FF00FF);
    t0 = now_ms();
    wr(X, pl, plen * 4);
    free(pl);
    long dt = sync_x(X, 10000);
    if (dt < 0) FAIL("server gone after the off-window PolyLine");
    else if (now_ms() - t0 > 3000) FAIL("off-window PolyLine took %ld ms", now_ms() - t0);
    else printf("xmal: off-window PolyLine in %ld ms ok\n", now_ms() - t0);
    p=b; w8(&p,60); w8(&p,0); w16(&p,2); w32(&p,g); wr(X,b,8);
}

static int run_child(const char *path, const char *arg){
    pid_t pid = fork();
    if (pid == 0) {
        execl(path, path, arg, (char*)0);
        _exit(127);
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(int argc, char **argv){
    int spawn = (argc > 1 && !strcmp(argv[1], "--spawn"));
    pid_t srv = 0;
    signal(SIGPIPE, SIG_IGN);
    if (spawn) {
        srv = fork();
        if (srv == 0) {
            execl("/maerox","maerox","-H",(char*)0);
            execl("/disk/maerox","maerox","-H",(char*)0);
            _exit(127);
        }
    }
    int X = open_client();
    if (X < 0) { printf("XMAL_FAIL connect/handshake\n"); if (srv) kill(srv,9); return 1; }
    render_major = query_render(X);

    unsigned win = alloc_id(), gc = alloc_id();
    create_window(X, win, 64, 64);
    create_gc(X, gc, win, 0x00CC3030);
    if (sync_x(X, 5000) < 0) { printf("XMAL_FAIL setup\n"); if (srv) kill(srv,9); return 1; }

    case_short(X);
    case_putimage(X, win, gc);
    case_getimage(X, win);
    case_ids(X, win, gc);
    case_clip(X, win, gc);
    case_glyphs(X, gc);
    case_dos(X, win);      /* before the cases that open clients (and reset id_base) */
    case_len0(X);
    case_msb(X);
    case_exitmid(X);
    int Z = case_stall(X);

    /* After all of it a well-formed client still draws on this server. */
    change_gc_fg(X, gc, 0x0030B050);
    fill_rect(X, win, gc, 0, 0, 64, 64);
    long px = get_pixel(X, win, 32, 32);
    if (px != 0x30B050) FAIL("final draw read back 0x%lx", px);
    else printf("xmal: normal drawing after malformed cases ok\n");
    fflush(stdout);
    if (run_child("/xdraw", 0) != 0) FAIL("xdraw against the same server failed");
    if (run_child("/xreal", 0) != 0) FAIL("xreal against the same server failed");
    if (sync_x(X, 5000) < 0) FAIL("server gone at the end");

    if (Z >= 0) close(Z);
    close(X);
    if (!failures) printf("XMAL_OK\n");
    else printf("XMAL_FAIL %d case(s)\n", failures);
    fflush(stdout);
    if (srv) { kill(srv,9); waitpid(srv,0,0); }
    return failures ? 1 : 0;
}
