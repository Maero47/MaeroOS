/*
 * maeroX core fonts.
 *
 * The server has one face, the desktop's 8x16 terminal bitmap font
 * (include/font8x16.h, ASCII), offered under XLFD names in a medium and a
 * bold (smeared) weight and as ISO 8859-1 and ISO 10646-1 encodings, plus the
 * usual aliases ("fixed", "8x16", "cursor"...).  OpenFont accepts any name
 * and gives this face, so a client asking for a font the server does not
 * have still draws text instead of failing; ListFonts only reports the names
 * below, so clients that search for a face see what is really there.
 *
 * Characters outside ASCII exist with an empty glyph, except the control
 * ranges 0-31 and 127-159, which are reported as nonexistent (zero metrics):
 * xterm draws its line-drawing characters itself when the font lacks them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xs.h"
#include <font8x16.h>

typedef struct {
    xobj_t o;
    int    bold;
    uint32_t name_atom;
} font_t;

static const char *const font_names[] = {
    "-misc-fixed-medium-r-normal--16-120-100-100-c-80-iso8859-1",
    "-misc-fixed-bold-r-normal--16-120-100-100-c-80-iso8859-1",
    "-misc-fixed-medium-r-normal--16-120-100-100-c-80-iso10646-1",
    "-misc-fixed-bold-r-normal--16-120-100-100-c-80-iso10646-1",
    "fixed", "8x16", "variable", "cursor",
};
#define N_FONT_NAMES (int)(sizeof(font_names) / sizeof(font_names[0]))

static int lc(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch; }

/* X font name patterns: '*' any run, '?' one character, case-insensitive. */
static int pat_match(const char *p, int pl, const char *s) {
    if (pl == 0) return *s == 0;
    if (*p == '*') {
        for (;;) {
            if (pat_match(p + 1, pl - 1, s)) return 1;
            if (!*s) return 0;
            s++;
        }
    }
    if (!*s) return 0;
    if (*p != '?' && lc((uint8_t)*p) != lc((uint8_t)*s)) return 0;
    return pat_match(p + 1, pl - 1, s + 1);
}

static int name_is_bold(const char *s, int len) {
    for (int i = 0; i + 4 <= len; i++)
        if (lc(s[i]) == 'b' && lc(s[i + 1]) == 'o' && lc(s[i + 2]) == 'l' && lc(s[i + 3]) == 'd')
            return 1;
    return 0;
}

static font_t *font_of(uint32_t fid) {
    xobj_t *o = res_lookup(fid);
    if (o && o->type == XT_FONT) return (font_t *)o;
    if (o && o->type == XT_GC) return font_of(((gc_t *)o)->font);
    return NULL;
}

int font_exists(uint32_t fid) { return font_of(fid) != NULL; }

int font_open(client_t *c, uint32_t fid, const char *name, int len) {
    font_t *f = calloc(1, sizeof(*f));
    if (!f) { x_error(c, BadAlloc, fid); return -1; }
    f->o.id = fid; f->o.type = XT_FONT; f->o.owner = (int8_t)c->index;
    f->bold = name_is_bold(name, len);
    const char *full = font_names[f->bold];
    for (int i = 0; i < N_FONT_NAMES; i++)
        if ((int)strlen(font_names[i]) == len && pat_match(name, len, font_names[i]) &&
            font_names[i][0] == '-')
            full = font_names[i];
    f->name_atom = atom_intern(full, (int)strlen(full), 0);
    res_add(&f->o);
    return 0;
}

void font_close(uint32_t fid) {
    xobj_t *o = res_lookup(fid);
    if (!o || o->type != XT_FONT) return;
    res_remove(o);
    free(o);
}

const uint8_t *font_glyph(uint32_t fid, unsigned ch, int *bold) {
    font_t *f = font_of(fid);
    *bold = f ? f->bold : 0;
    if (ch < 32 || ch > 126) return NULL;
    return font8x16[ch];
}

static int char_exists(unsigned ch) { return !(ch < 32 || (ch >= 127 && ch < 160)); }

static void charinfo(uint8_t *p, int exists) {
    memset(p, 0, 12);
    if (!exists) return;
    put16(p + 0, 0);
    put16(p + 2, FONT_W);
    put16(p + 4, FONT_W);
    put16(p + 6, FONT_ASCENT);
    put16(p + 8, FONT_DESCENT);
}

#define NPROPS 9
static int font_props(uint8_t *p, font_t *f) {
    struct { const char *name; uint32_t val; int is_atom; const char *sval; } pr[NPROPS] = {
        { "FONT", 0, 2, NULL },
        { "PIXEL_SIZE", 16, 0, NULL },
        { "POINT_SIZE", 120, 0, NULL },
        { "RESOLUTION_X", 100, 0, NULL },
        { "FONT_ASCENT", FONT_ASCENT, 0, NULL },
        { "FONT_DESCENT", FONT_DESCENT, 0, NULL },
        { "SPACING", 0, 1, "C" },
        { "WEIGHT_NAME", 0, 1, NULL },
        { "AVERAGE_WIDTH", 80, 0, NULL },
    };
    for (int i = 0; i < NPROPS; i++) {
        uint32_t a = atom_intern(pr[i].name, (int)strlen(pr[i].name), 0);
        uint32_t v = pr[i].val;
        if (pr[i].is_atom == 2) v = f ? f->name_atom : 0;
        else if (pr[i].is_atom == 1) {
            const char *s = pr[i].sval ? pr[i].sval : (f && f->bold ? "Bold" : "Medium");
            v = atom_intern(s, (int)strlen(s), 0);
        }
        put32(p + 8 * i, a);
        put32(p + 8 * i + 4, v);
    }
    return NPROPS * 8;
}

/* The 52 bytes after the reply header shared by QueryFont and
 * ListFontsWithInfo: bounds, char range, ascent/descent. */
static void font_info(uint8_t *p, int nprops, uint32_t last_field) {
    memset(p, 0, 52);
    charinfo(p + 0, 1);                       /* min-bounds */
    charinfo(p + 16, 1);                      /* max-bounds */
    put16(p + 32, 0);                         /* min-char-or-byte2 */
    put16(p + 34, 255);                       /* max-char-or-byte2 */
    put16(p + 36, ' ');                       /* default-char */
    put16(p + 38, (uint32_t)nprops);
    p[40] = 0;                                /* draw-direction LeftToRight */
    p[41] = 0; p[42] = 0;                     /* min/max byte1 */
    p[43] = 0;                                /* all-chars-exist */
    put16(p + 44, FONT_ASCENT);
    put16(p + 46, FONT_DESCENT);
    put32(p + 48, last_field);
}

void font_query(client_t *c, uint32_t fid) {
    font_t *f = font_of(fid);
    if (!f) { x_error(c, BadFont, fid); return; }
    int m = 256;
    size_t len = 60 + NPROPS * 8 + (size_t)m * 12;
    uint8_t *r = calloc(1, len);
    if (!r) { x_error(c, BadAlloc, 0); return; }
    r[0] = 1;
    put16(r + 2, c->seq);
    put32(r + 4, (uint32_t)((len - 32) / 4));
    font_info(r + 8, NPROPS, (uint32_t)m);
    int off = 60 + font_props(r + 60, f);
    for (int i = 0; i < m; i++) charinfo(r + off + 12 * i, char_exists((unsigned)i));
    out_write(c, r, len);
    free(r);
}

void font_text_extents(client_t *c, uint32_t fid, const uint8_t *s, int n16) {
    (void)s;
    if (!font_of(fid)) { x_error(c, BadFont, fid); return; }
    if (n16 < 0) n16 = 0;
    uint8_t d[24];
    memset(d, 0, sizeof(d));
    put16(d + 0, FONT_ASCENT);
    put16(d + 2, FONT_DESCENT);
    put16(d + 4, FONT_ASCENT);
    put16(d + 6, FONT_DESCENT);
    put32(d + 8, (uint32_t)(n16 * FONT_W));
    put32(d + 12, 0);
    put32(d + 16, (uint32_t)(n16 * FONT_W));
    send_reply(c, 0, d);
}

void font_list(client_t *c, const char *pat, int len, int max, int with_info) {
    int hits[N_FONT_NAMES], n = 0;
    for (int i = 0; i < N_FONT_NAMES && n < max; i++)
        if (pat_match(pat, len, font_names[i])) hits[n++] = i;
    if (!with_info) {
        size_t bytes = 0;
        for (int i = 0; i < n; i++) bytes += 1 + strlen(font_names[hits[i]]);
        uint8_t *buf = malloc(bytes + 1);
        if (!buf) { x_error(c, BadAlloc, 0); return; }
        size_t o = 0;
        for (int i = 0; i < n; i++) {
            size_t l = strlen(font_names[hits[i]]);
            buf[o++] = (uint8_t)l;
            memcpy(buf + o, font_names[hits[i]], l);
            o += l;
        }
        uint8_t d[24];
        memset(d, 0, sizeof(d));
        put16(d, (uint32_t)n);
        send_reply_var(c, 0, d, buf, (int)o);
        free(buf);
        return;
    }
    for (int i = 0; i < n; i++) {
        const char *nm = font_names[hits[i]];
        int nl = (int)strlen(nm), pad = (4 - (nl & 3)) & 3;
        size_t len = 60 + NPROPS * 8 + (size_t)nl + (size_t)pad;
        uint8_t *r = calloc(1, len);
        if (!r) { x_error(c, BadAlloc, 0); return; }
        font_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.bold = name_is_bold(nm, nl);
        tmp.name_atom = atom_intern(nm, nl, 0);
        r[0] = 1;
        r[1] = (uint8_t)nl;
        put16(r + 2, c->seq);
        put32(r + 4, (uint32_t)((len - 32) / 4));
        font_info(r + 8, NPROPS, (uint32_t)(n - i - 1));
        font_props(r + 60, &tmp);
        memcpy(r + 60 + NPROPS * 8, nm, (size_t)nl);
        out_write(c, r, len);
        free(r);
    }
    uint8_t last[60];
    memset(last, 0, sizeof(last));
    last[0] = 1;
    put16(last + 2, c->seq);
    put32(last + 4, 7);
    out_write(c, last, 60);
}

void font_get_path(client_t *c) {
    static const uint8_t path[] = { 9, 'b', 'u', 'i', 'l', 't', '-', 'i', 'n', 's' };
    uint8_t d[24];
    memset(d, 0, sizeof(d));
    put16(d, 1);
    send_reply_var(c, 0, d, path, sizeof(path));
}
