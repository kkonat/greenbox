/*
 * osgfx.c - the guest rasteriser: integer geometry over a caller-owned buffer.
 *
 * This is astro's astro_gfx.c and pinball's pin_gfx.c, which were the same two
 * hundred lines twice over - pinball's header said so, and explained the
 * reason: a guest links on its own, with no OS symbols to bind against, so the
 * only ways to share code between two programs were a copy or a fourth
 * directory for build.ps1 to learn about. Both were true. Neither is any more,
 * because there has always been a third way and it is the one the whole system
 * is built on: put it behind the API table.
 *
 * What that buys, in order of how much it matters:
 *
 *   One implementation of the clipping. The band-boundary bug that the ellipse
 *   seeding comment describes was found once, in astro, and fixed once; the
 *   copy in pinball's ring got the same fix by hand. A third copy would have
 *   been a third chance to miss it, and the third copy was already being
 *   written when this file was made instead.
 *
 *   A guest that wants to draw starts by drawing, rather than by pasting.
 *
 *   Some IRAM, and less than it looks like. A guest's .text is copied into the
 *   executable-IRAM heap and there is about 75 KB of it for everything, so
 *   moving code here - IROM, cached, effectively free - ought to be a clear
 *   win. What it actually is depends on how the guest draws: a call through
 *   api->gfx costs an indirect jump where a local one cost a direct call, and
 *   astro makes thousands of them a frame. Measured across the move, pinball's
 *   text fell by 1,084 bytes and astro's by 144. Real, but not the argument.
 *
 * What it deliberately does not buy: any OS state. Nothing here is static
 * except const tables and one PRNG word. The surface, its pixels and its clip
 * all belong to the guest and arrive as an argument, so there is no per-guest
 * teardown, no pointer for the loader to invalidate, and no way for a guest
 * killed mid-frame to leave the OS holding anything.
 *
 * It is also plain C with no IDF headers, which is what lets a guest's host-
 * side simulator compile this exact file and get the exact pixels the board
 * would draw.
 */

#include "osgfx.h"

/* ==================================================================== maths */

static uint32_t s_rng = 0x1234567u;

static void g_rnd_seed(uint32_t s) { s_rng = s ? s : 0xA5A5F00Du; }

static uint32_t g_rnd(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

/* Exact floor(sqrt(v)) by restoring shifts: no division, no float. Every
 * circle in this file is solved per scanline with it. */
static uint32_t g_isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else            { r >>= 1; }
        b >>= 2;
    }
    return r;
}

/* One turn is 256 units. Q8, so +-256 is +-1.0. */
static const int16_t SIN256[32] = {
       0,   50,   98,  142,  181,  213,  237,  251,
     256,  251,  237,  213,  181,  142,   98,   50,
       0,  -50,  -98, -142, -181, -213, -237, -251,
    -256, -251, -237, -213, -181, -142,  -98,  -50,
};

static int16_t g_isin(int16_t a)
{
    int t = a & 255;
    int i = t >> 3, f = t & 7;
    int v0 = SIN256[i], v1 = SIN256[(i + 1) & 31];
    return (int16_t)(v0 + (((v1 - v0) * f) >> 3));
}

/*
 * Clamped, because the callers that pulse a colour compute t from a counter
 * and it is far too easy to overshoot 256. Extrapolating past the endpoint
 * overflows one channel into the next, and a shield bubble that should breathe
 * cyan comes out yellow.
 */
static uint16_t g_mix565(uint16_t a, uint16_t b, int16_t t)
{
    if (t < 0) t = 0; else if (t > 256) t = 256;
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (((br - ar) * t) >> 8);
    int g = ag + (((bg - ag) * t) >> 8);
    int l = ab + (((bb - ab) * t) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | l);
}

/* ================================================================= clipping */
/*
 * The one thing every primitive below starts with: where on the screen this
 * surface is allowed to put pixels, inclusive at both ends. That is the buffer
 * itself, narrowed by the clip rectangle if the caller set one - and a clip
 * rectangle with no area means no clip, so a surface memset to zero and handed
 * a buffer works.
 */
typedef struct { int x0, y0, x1, y1; } lim_t;

static inline lim_t limits(const gb_surf_t *s)
{
    lim_t l;
    l.x0 = s->org_x;
    l.y0 = s->org_y;
    l.x1 = s->org_x + s->w - 1;
    l.y1 = s->org_y + s->h - 1;
    if (s->clip_x1 > s->clip_x0 && s->clip_y1 > s->clip_y0) {
        if (s->clip_x0 > l.x0) l.x0 = s->clip_x0;
        if (s->clip_y0 > l.y0) l.y0 = s->clip_y0;
        if (s->clip_x1 - 1 < l.x1) l.x1 = s->clip_x1 - 1;
        if (s->clip_y1 - 1 < l.y1) l.y1 = s->clip_y1 - 1;
    }
    return l;
}

/* ============================================================== primitives */

static void g_hspan(const gb_surf_t *s, int16_t x0, int16_t x1, int16_t y,
                    uint16_t c)
{
    lim_t l = limits(s);
    if (y < l.y0 || y > l.y1) return;
    int a = x0 < l.x0 ? l.x0 : x0;
    int b = x1 > l.x1 ? l.x1 : x1;
    if (b < a) return;
    uint16_t *p = s->px + (y - s->org_y) * s->w + (a - s->org_x);
    for (int i = b - a + 1; i > 0; i--) *p++ = c;
}

static void g_row(const gb_surf_t *s, int16_t y, uint16_t c)
{
    g_hspan(s, (int16_t)s->org_x, (int16_t)(s->org_x + s->w - 1), y, c);
}

static void g_px(const gb_surf_t *s, int16_t x, int16_t y, uint16_t c)
{
    lim_t l = limits(s);
    if (x < l.x0 || x > l.x1 || y < l.y0 || y > l.y1) return;
    s->px[(y - s->org_y) * s->w + (x - s->org_x)] = c;
}

static void g_box(const gb_surf_t *s, int16_t x, int16_t y,
                  int16_t w, int16_t h, uint16_t c)
{
    for (int i = 0; i < h; i++)
        g_hspan(s, x, (int16_t)(x + w - 1), (int16_t)(y + i), c);
}

static void g_frame(const gb_surf_t *s, int16_t x, int16_t y,
                    int16_t w, int16_t h, uint16_t c)
{
    g_hspan(s, x, (int16_t)(x + w - 1), y, c);
    g_hspan(s, x, (int16_t)(x + w - 1), (int16_t)(y + h - 1), c);
    for (int i = 1; i < h - 1; i++) {
        g_px(s, x, (int16_t)(y + i), c);
        g_px(s, (int16_t)(x + w - 1), (int16_t)(y + i), c);
    }
}

/* =================================================================== curves */

static void g_disc(const gb_surf_t *s, int16_t cx, int16_t cy, int16_t r,
                   uint16_t c)
{
    if (r < 0) return;
    lim_t l = limits(s);
    int y0 = cy - r, y1 = cy + r;
    if (y0 < l.y0) y0 = l.y0;
    if (y1 > l.y1) y1 = l.y1;
    for (int y = y0; y <= y1; y++) {
        int dy = y - cy;
        int t = r * r - dy * dy;
        if (t < 0) continue;
        int hw = (int)g_isqrt((uint32_t)t);
        g_hspan(s, (int16_t)(cx - hw), (int16_t)(cx + hw), (int16_t)y, c);
    }
}

/*
 * Circle outline, two pixels per scanline per side.
 *
 * Seeded from the row above the band rather than from -1. The band renderer
 * hands this one slice of the circle at a time, and an outline that forgot how
 * wide it was on the previous row leaves a gap wherever the curve moves
 * sideways faster than one row down - which, for a twenty-pixel halo on
 * sixteen-row bands, is most frames and a different place each time.
 */
static void g_ring(const gb_surf_t *s, int16_t cx, int16_t cy, int16_t r,
                   uint16_t c)
{
    if (r < 1) return;
    lim_t l = limits(s);
    int y0 = cy - r, y1 = cy + r;
    if (y0 < l.y0) y0 = l.y0;
    if (y1 > l.y1) y1 = l.y1;

    int prev = -1;
    {
        int dy = y0 - 1 - cy, t = r * r - dy * dy;
        if (t >= 0) prev = (int)g_isqrt((uint32_t)t);
    }
    for (int y = y0; y <= y1; y++) {
        int dy = y - cy, t = r * r - dy * dy;
        if (t < 0) { prev = -1; continue; }
        int hw = (int)g_isqrt((uint32_t)t);
        /* Widen this row to meet the last one where the curve outran it.
         * Always on THIS row: writing the bridge to y-1 would drop it whenever
         * y is the first row of a band. */
        if (prev >= 0) {
            int lo = hw < prev ? hw : prev, hi = hw < prev ? prev : hw;
            if (hi - lo > 1) {
                g_hspan(s, (int16_t)(cx - hi), (int16_t)(cx - lo), (int16_t)y, c);
                g_hspan(s, (int16_t)(cx + lo), (int16_t)(cx + hi), (int16_t)y, c);
            }
        }
        g_px(s, (int16_t)(cx - hw), (int16_t)y, c);
        g_px(s, (int16_t)(cx + hw), (int16_t)y, c);
        prev = hw;
    }
}

/* The same, with the radius scaled per axis. Solved per row rather than cached
 * because the callers that pulse a halo pass a different radius every frame. */
static void g_ellipse(const gb_surf_t *s, int16_t cx, int16_t cy,
                      int16_t rx, int16_t ry, uint16_t c)
{
    if (rx < 1 || ry < 1) return;
    lim_t l = limits(s);
    int y0 = cy - ry, y1 = cy + ry;
    if (y0 < l.y0) y0 = l.y0;
    if (y1 > l.y1) y1 = l.y1;

    int k = (rx << 8) / ry;                 /* rx/ry in Q8, hoisted */

    int prev = -1;
    {
        int dy = y0 - 1 - cy;
        int t = ry * ry - dy * dy;
        if (t >= 0) prev = (k * (int)g_isqrt((uint32_t)t)) >> 8;
    }
    for (int y = y0; y <= y1; y++) {
        int dy = y - cy;
        int t = ry * ry - dy * dy;
        if (t < 0) { prev = -1; continue; }
        int hw = (k * (int)g_isqrt((uint32_t)t)) >> 8;
        if (prev >= 0) {
            int lo = hw < prev ? hw : prev, hi = hw < prev ? prev : hw;
            if (hi - lo > 1) {
                g_hspan(s, (int16_t)(cx - hi), (int16_t)(cx - lo), (int16_t)y, c);
                g_hspan(s, (int16_t)(cx + lo), (int16_t)(cx + hi), (int16_t)y, c);
            }
        }
        g_px(s, (int16_t)(cx - hw), (int16_t)y, c);
        g_px(s, (int16_t)(cx + hw), (int16_t)y, c);
        prev = hw;
    }
}

/* Bresenham, unclipped at the ends: static geometry gets run over once per
 * band and g_px does the rejecting. A few hundred rejected stores cost less
 * than the bookkeeping of clipping a line properly. */
static void g_line(const gb_surf_t *s, int16_t x0, int16_t y0,
                   int16_t x1, int16_t y1, uint16_t c)
{
    int ax = x0, ay = y0;
    int dx = x1 - ax, dy = y1 - ay;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        g_px(s, (int16_t)ax, (int16_t)ay, c);
        if (ax == x1 && ay == y1) break;
        int e2 = err << 1;
        if (e2 > -dy) { err -= dy; ax += sx; }
        if (e2 <  dx) { err += dx; ay += sy; }
    }
}

static void g_bar(const gb_surf_t *s, int16_t x0, int16_t y0,
                  int16_t x1, int16_t y1, int16_t r, uint16_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int n = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (n < 1) n = 1;
    for (int i = 0; i <= n; i++)
        g_disc(s, (int16_t)(x0 + (dx * i) / n), (int16_t)(y0 + (dy * i) / n), r, c);
}

/* ===================================================================== font */
/*
 * 3x5, written as five 3-bit rows per glyph and transposed to columns by the
 * macro, so the art stays legible in the source and costs one byte per column
 * at run time.
 */
#define GBIT(r0,r1,r2,r3,r4,b) (uint8_t)(  \
      (((r0) >> (b)) & 1)                  \
    | ((((r1) >> (b)) & 1) << 1)           \
    | ((((r2) >> (b)) & 1) << 2)           \
    | ((((r3) >> (b)) & 1) << 3)           \
    | ((((r4) >> (b)) & 1) << 4))
#define G(r0,r1,r2,r3,r4) { GBIT(r0,r1,r2,r3,r4,2), \
                            GBIT(r0,r1,r2,r3,r4,1), \
                            GBIT(r0,r1,r2,r3,r4,0) }

static const uint8_t FONT35[53][3] = {
    G(0b000,0b000,0b000,0b000,0b000),   /*   */
    G(0b111,0b101,0b101,0b101,0b111),   /* 0 */
    G(0b010,0b110,0b010,0b010,0b111),   /* 1 */
    G(0b111,0b001,0b111,0b100,0b111),   /* 2 */
    G(0b111,0b001,0b111,0b001,0b111),   /* 3 */
    G(0b101,0b101,0b111,0b001,0b001),   /* 4 */
    G(0b111,0b100,0b111,0b001,0b111),   /* 5 */
    G(0b111,0b100,0b111,0b101,0b111),   /* 6 */
    G(0b111,0b001,0b001,0b001,0b001),   /* 7 */
    G(0b111,0b101,0b111,0b101,0b111),   /* 8 */
    G(0b111,0b101,0b111,0b001,0b111),   /* 9 */
    G(0b111,0b101,0b111,0b101,0b101),   /* A */
    G(0b110,0b101,0b110,0b101,0b110),   /* B */
    G(0b111,0b100,0b100,0b100,0b111),   /* C */
    G(0b110,0b101,0b101,0b101,0b110),   /* D */
    G(0b111,0b100,0b111,0b100,0b111),   /* E */
    G(0b111,0b100,0b111,0b100,0b100),   /* F */
    G(0b011,0b100,0b101,0b101,0b011),   /* G */
    G(0b101,0b101,0b111,0b101,0b101),   /* H */
    G(0b111,0b010,0b010,0b010,0b111),   /* I */
    G(0b001,0b001,0b001,0b101,0b111),   /* J */
    G(0b101,0b101,0b110,0b101,0b101),   /* K */
    G(0b100,0b100,0b100,0b100,0b111),   /* L */
    G(0b101,0b111,0b111,0b101,0b101),   /* M */
    G(0b110,0b101,0b101,0b101,0b101),   /* N */
    G(0b111,0b101,0b101,0b101,0b111),   /* O */
    G(0b111,0b101,0b111,0b100,0b100),   /* P */
    G(0b111,0b101,0b101,0b111,0b001),   /* Q */
    G(0b111,0b101,0b111,0b110,0b101),   /* R */
    G(0b111,0b100,0b111,0b001,0b111),   /* S */
    G(0b111,0b010,0b010,0b010,0b010),   /* T */
    G(0b101,0b101,0b101,0b101,0b111),   /* U */
    G(0b101,0b101,0b101,0b101,0b010),   /* V */
    G(0b101,0b101,0b111,0b111,0b101),   /* W */
    G(0b101,0b101,0b010,0b101,0b101),   /* X */
    G(0b101,0b101,0b010,0b010,0b010),   /* Y */
    G(0b111,0b001,0b010,0b100,0b111),   /* Z */
    G(0b000,0b000,0b111,0b000,0b000),   /* - */
    G(0b000,0b000,0b000,0b000,0b010),   /* . */
    G(0b000,0b010,0b000,0b010,0b000),   /* : */
    G(0b010,0b010,0b010,0b000,0b010),   /* ! */
    G(0b000,0b010,0b111,0b010,0b000),   /* + */
    G(0b101,0b101,0b010,0b000,0b000),   /* ' */
    G(0b001,0b001,0b010,0b100,0b100),   /* / */
    G(0b111,0b001,0b011,0b000,0b010),   /* ? */
    G(0b000,0b000,0b000,0b010,0b100),   /* , */
    G(0b000,0b111,0b000,0b111,0b000),   /* = */
    G(0b001,0b010,0b010,0b010,0b001),   /* ( */
    G(0b100,0b010,0b010,0b010,0b100),   /* ) */
    G(0b001,0b010,0b100,0b010,0b001),   /* < */
    G(0b100,0b010,0b001,0b010,0b100),   /* > */
    G(0b101,0b010,0b101,0b000,0b000),   /* * */
    G(0b101,0b001,0b010,0b100,0b101),   /* % */
};

static int glyph_of(char c)
{
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 11 + (c - 'a');
    switch (c) {
    case '-':  return 37;
    case '.':  return 38;
    case ':':  return 39;
    case '!':  return 40;
    case '+':  return 41;
    case 0x27: return 42;       /* apostrophe */
    case '/':  return 43;
    case '?':  return 44;
    case ',':  return 45;
    case '=':  return 46;
    case '(':  return 47;
    case ')':  return 48;
    case '<':  return 49;
    case '>':  return 50;
    case '*':  return 51;
    case '%':  return 52;
    default:   return 0;
    }
}

static void g_text(const gb_surf_t *s, int16_t x, int16_t y, const char *str,
                   uint16_t c, uint8_t scale)
{
    if (!str) return;
    int sc = scale < 1 ? 1 : scale;
    lim_t l = limits(s);
    int cx = x;
    for (; *str; str++, cx += 4 * sc) {
        if (cx > l.x1) return;
        const uint8_t *g = FONT35[glyph_of(*str)];
        for (int col = 0; col < 3; col++) {
            uint8_t bits = g[col];
            if (!bits) continue;
            for (int row = 0; row < 5; row++)
                if (bits & (1u << row))
                    g_box(s, (int16_t)(cx + col * sc), (int16_t)(y + row * sc),
                          (int16_t)sc, (int16_t)sc, c);
        }
    }
}

/* The advance of the last glyph is not part of the width: a 4-pixel pitch on a
 * 3-pixel glyph means the trailing column is space, and centring text with it
 * included puts everything half a pixel left. */
static int16_t g_text_w(const char *str, uint8_t scale)
{
    int sc = scale < 1 ? 1 : scale;
    int n = 0;
    if (str) while (str[n]) n++;
    if (n == 0) return 0;
    return (int16_t)(n * 4 * sc - sc);
}

/* ================================================================== sprites */

static void g_sprite(const gb_surf_t *s, const gb_sprite_t *sp,
                     int16_t x, int16_t y)
{
    if (!sp || !sp->rows || !sp->pal || sp->nrows < 1 || sp->ncols < 1) return;

    int scale = sp->scale < 1 ? 1 : sp->scale;
    int nrows = sp->nrows, ncols = sp->ncols;
    lim_t l = limits(s);

    /*
     * Work out which source rows can possibly land in this band before
     * touching any of them. A 26x28 face at scale 4 covers seven bands, so six
     * of every seven visits should do almost nothing - which they will not if
     * the answer is arrived at by clipping a thousand boxes one at a time.
     */
    int r0 = (l.y0 - y) / scale;
    int r1 = (l.y1 - y) / scale;
    if (r0 < 0) r0 = 0;
    if (r1 > nrows - 1) r1 = nrows - 1;
    if (r0 > r1) return;

    for (int r = r0; r <= r1; r++) {
        const char *src = sp->rows[r];
        if (!src) continue;
        int py = y + r * scale;
        /* Bank into the turn: the nose leads by a pixel and the tail lags.
         * Free animation out of a static sprite. */
        int sx = x;
        if (sp->lean && nrows > 1)
            sx += (sp->lean * (nrows - 1 - r)) / (nrows - 1);

        for (int col = 0; col < ncols; col++) {
            char ch = src[col];
            if (ch == '.' || ch == 0) continue;
            for (int i = 0; i < sp->npal; i++) {
                if (sp->pal[i].ch != ch) continue;
                uint16_t c = sp->tint
                           ? g_mix565(sp->pal[i].col, sp->tint_to, sp->tint)
                           : sp->pal[i].col;
                g_box(s, (int16_t)(sx + col * scale), (int16_t)py,
                      (int16_t)scale, (int16_t)scale, c);
                break;
            }
        }
    }
}

/* ================================================================= the table */

const gb_gfx_t g_gb_gfx = {
    .row     = g_row,
    .hspan   = g_hspan,
    .px      = g_px,
    .box     = g_box,
    .frame   = g_frame,

    .disc    = g_disc,
    .ring    = g_ring,
    .ellipse = g_ellipse,
    .line    = g_line,
    .bar     = g_bar,

    .text    = g_text,
    .text_w  = g_text_w,

    .sprite  = g_sprite,

    .isqrt   = g_isqrt,
    .isin    = g_isin,
    .mix565  = g_mix565,
    .rnd     = g_rnd,
    .rnd_seed = g_rnd_seed,
};
