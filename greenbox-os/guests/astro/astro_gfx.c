/*
 * astro_gfx.c - the pixel half of ASTRO.
 *
 * Nothing here talks to the OS. Every routine writes into the band buffer and
 * clips to the band currently being assembled; astro.c blits.
 *
 * Two constraints shape most of what follows.
 *
 * The first is that api->pixel() costs a windowed SPI transaction - three
 * commands and four payload bytes to set an address window, for one pixel - so
 * a starfield drawn with it would manage single-digit frames per second. Every
 * pixel here is a store into RAM instead, and the panel only ever sees whole
 * rows.
 *
 * The second is that there is no libm and no reason to want one. The rock
 * rasteriser, the ring geometry and the nebula falloff all run on isqrt32()
 * and a 32-entry sine table in fixed point. Soft float would drag in libgcc
 * routines and cost more than the geometry it was meant to simplify.
 *
 * Both of those used to be argued at the top of a file that also contained the
 * spans, the circles, the font and the sprite blitter. Those are the OS's now
 * (osgfx.c, reached through api->gfx), because pinball had copied them
 * verbatim and pacman would have been the third copy. What is left here is
 * what only astro wants: the parallax background and the rocks.
 */

#include "astro.h"

const gb_api_t *A;
int16_t g_w, g_h;

uint16_t g_fb[SCR_MAX_W * BAND_H];

/* =================================================================== rocks */
/*
 * An asteroid is a circle whose radius is modulated by a 16-entry bump table,
 * rasterised a scanline at a time. Sprites were the alternative and lost:
 * rocks appear at every size from 5 to 16 pixels and they tumble, so a sheet
 * would need several hundred frames to cover what four bump tables and an
 * angle offset cover here.
 *
 * The row-to-angle mapping is linear where strictly it is an arcsine. That
 * distributes the bumps a little unevenly around the rim, which on a rock is
 * indistinguishable from the rock.
 */
#define NSHAPE  4

static const int8_t LUMP[NSHAPE][16] = {
    { 12, -9, 18,  4,-16, 10, 21, -5, 14,-18,  6, 16,-11,  2, 20, -7 },
    {-14, 10, 22, -6,  8,-18,  5, 16,-12, 20,  2, -8, 15,  6,-16, 12 },
    { 20,  4,-13, 16, -2, 12,-20,  9, 18, -7,-15, 11,  4, 22,-10,  0 },
    { -5, 17,  8,-19, 12,  3,-11, 23,  7,-14, 18, -3, 10, 15, -9,  5 },
};

/* Crater angle, distance from centre, and radius - the last two as percentages
 * of the rock's radius. They rotate with the rock. */
#define NCRAT 2
static const uint8_t CRAT[NSHAPE][NCRAT][3] = {
    { {  30, 40, 26 }, { 150, 46, 21 } },
    { {  80, 44, 22 }, { 200, 38, 25 } },
    { { 118, 38, 27 }, {  16, 48, 20 } },
    { { 172, 46, 21 }, {  92, 40, 24 } },
};

/* Stone, iron-rich, ice. Eight-step ramps, built once at init. */
#define NKIND 3
static uint16_t s_rockramp[NKIND][8];

static int lump_at(int shape, int a)
{
    const int8_t *t = LUMP[shape];
    int i = (a >> 4) & 15, f = a & 15;
    int v0 = t[i], v1 = t[(i + 1) & 15];
    return v0 + (((v1 - v0) * f) >> 4);
}

/* The silhouette is the whole of the shape language here, so it gets two
 * octaves: the table itself for the big dents, and the next table read three
 * times as fast at a quarter of the weight for the roughness in between. One
 * octave on its own leaves rocks looking like slightly dented balls. */
static int rim_at(int shape, int a)
{
    return lump_at(shape, a) + (lump_at((shape + 1) & (NSHAPE - 1),
                                        (a * 3) & 255) >> 2);
}

void draw_rock(int cx, int cy, int rad, int rot, int shape, int kind)
{
    if (rad < 2) return;
    shape &= (NSHAPE - 1);
    if (kind < 0 || kind >= NKIND) kind = 0;
    const uint16_t *ramp = s_rockramp[kind];

    int y0 = cy - rad, y1 = cy + rad;
    if (y0 < g_band_y0) y0 = g_band_y0;
    if (y1 > g_band_y0 + g_band_h - 1) y1 = g_band_y0 + g_band_h - 1;
    if (y0 > y1) return;

    int rr = rad * rad;
    int db = -(100 * 256) / rad;            /* brightness slope per pixel, Q8 */

    for (int y = y0; y <= y1; y++) {
        int dy = y - cy;
        int t = rr - dy * dy;
        if (t <= 0) continue;
        int w = (int)isqrt32((uint32_t)t);

        int aR = (64 * dy) / rad;           /* -64..64 turns down the rim */
        int xl = cx - w - (w * rim_at(shape, (128 - aR + rot) & 255)) / 100;
        int xr = cx + w + (w * rim_at(shape, (aR + rot) & 255)) / 100;
        if (xr < xl) continue;

        int cl = xl < 0 ? 0 : xl;
        int cr = xr > g_w - 1 ? g_w - 1 : xr;
        if (cl > cr) continue;

        /* Lit from the top left. The whole row is one running add. */
        int b = (128 - ((cl - cx) * 100) / rad - (dy * 100) / rad) << 8;
        uint16_t *p = &g_fb[(y - g_band_y0) * g_w + cl];
        for (int x = cl; x <= cr; x++) {
            int v = b >> 8;
            if (v < 0) v = 0; else if (v > 255) v = 255;
            *p++ = ramp[v >> 5];
            b += db;
        }

        /* A rim on both edges, or the big ones read as soft blobs - but three
         * steps down the ramp from whatever it borders, not flat black. A
         * black line along the lit side looks like a cut-out. */
        if (xl >= 0 && xl < g_w) {
            int v = 128 - ((xl - cx) * 100) / rad - (dy * 100) / rad;
            if (v < 0) v = 0; else if (v > 255) v = 255;
            int i = (v >> 5) - 3; if (i < 0) i = 0;
            g_fb[(y - g_band_y0) * g_w + xl] = ramp[i];
        }
        if (xr >= 0 && xr < g_w) {
            int v = 128 - ((xr - cx) * 100) / rad - (dy * 100) / rad;
            if (v < 0) v = 0; else if (v > 255) v = 255;
            int i = (v >> 5) - 3; if (i < 0) i = 0;
            g_fb[(y - g_band_y0) * g_w + xr] = ramp[i];
        }
    }

    /*
     * Craters, once the rock is big enough for them to be more than noise.
     *
     * A depression lit from the top left has its shadow on the near wall and
     * its lit floor on the far one - the opposite way round from the rock it
     * sits in, and that inversion is what makes a dish read as a dish rather
     * than as a dark blob. Two discs, the lighter one pushed down and right.
     */
    for (int k = 0; k < NCRAT; k++) {
        const uint8_t *c = CRAT[shape][k];
        int cr = (rad * c[2]) / 100;
        /* Below three pixels there is no room for a wall and a floor, and what
         * comes out is a dark speck. The silhouette and the terminator carry
         * the small rocks on their own. */
        if (cr < 3) continue;
        int a  = (rot + c[0]) & 255;
        int d  = (rad * c[1]) / 100;
        int px = cx + ((icos(a) * d) >> 8);
        int py = cy + ((isin(a) * d) >> 8);
        fb_disc(px, py, cr, ramp[1]);
        fb_disc(px + cr / 3, py + cr / 3, cr - cr / 3, ramp[5]);
    }
}

/* ============================================================== background */
/*
 * Three layers, all parallax, none of them interactive: a nebula field, the
 * occasional ring section, and dust. Between them they are the only reason the
 * ship feels like it is moving when there is nothing else on screen.
 */

/*
 * The empty-space gradient. s_grad is the exact colour of a row, used as the
 * base the nebula and dust palettes are mixed against; s_gradd is what
 * actually gets drawn.
 *
 * They differ because RGB565 has 32 blue levels and this gradient crosses
 * about two and a half of them over the whole panel - so drawn flat it is not
 * a gradient at all, it is two horizontal seams. s_gradd holds the four
 * colours of one 4-pixel dither period per row, which turns each seam into a
 * stipple and costs the clear nothing: it was writing a colour per pixel
 * either way.
 */
static uint16_t s_grad[SCR_MAX_H];
static uint16_t s_gradd[SCR_MAX_H][4];
static int      s_flash;                /* crash white-out, counted in frames */

/* ---- nebulae ---- */
#define NT_W    48
#define NT_H    36
#define NT_N    3
#define NEB_N   3

static uint8_t s_tile[NT_N][NT_H][NT_W];

/* Ordered dither. The lookup below has 16 entries, so a smooth cloud would
 * otherwise show 16 hard contours; this scatters the quantisation across a 4x4
 * cell into something the eye reads as texture. It is what makes a nebula
 * drawn at 4x magnification still look like gas. */
static const uint8_t BAYER4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

static const uint16_t NEB_EDGE[4] = {
    GB_RGB( 30, 10, 62), GB_RGB(  8, 34, 56),
    GB_RGB( 62, 20, 34), GB_RGB( 14, 28, 24),
};
static const uint16_t NEB_CORE[4] = {
    GB_RGB(126, 62,196), GB_RGB( 44,136,186),
    GB_RGB(206, 84, 92), GB_RGB( 72,150,116),
};

typedef struct {
    int32_t  x_q8, y_q8;
    uint8_t  tile;
    uint8_t  sh;            /* scale shift: 1 -> 96x72, 2 -> 192x144 */
    uint8_t  par;           /* parallax, in 1/256ths of the scroll speed */
    int16_t  vx;            /* a slow sideways drift of its own */
    uint16_t lut[16];
} neb_t;

static neb_t s_neb[NEB_N];

static int bilerp(int a, int b, int c, int d, int fx, int fy)
{
    int top = a + (((b - a) * fx) >> 8);
    int bot = c + (((d - c) * fx) >> 8);
    return top + (((bot - top) * fy) >> 8);
}

static void gen_tile(uint8_t *dst)
{
    /* Two octaves of value noise. The grids are static rather than automatic
     * because the guest task gets a 4 KB stack and 400 bytes of it is not
     * going spare. */
    static uint8_t g1[10][7];       /* 8x4 pixel cells */
    static uint8_t g2[19][13];      /* 4x2 pixel cells */

    for (int y = 0; y < 10; y++)
        for (int x = 0; x < 7; x++) g1[y][x] = (uint8_t)rnd();
    for (int y = 0; y < 19; y++)
        for (int x = 0; x < 13; x++) g2[y][x] = (uint8_t)rnd();

    for (int y = 0; y < NT_H; y++) {
        int gy1 = y >> 2, fy1 = (y & 3) << 6;
        int gy2 = y >> 1, fy2 = (y & 1) << 7;
        int dy  = ((y - NT_H / 2) * 256) / (NT_H / 2);

        for (int x = 0; x < NT_W; x++) {
            int gx1 = x >> 3, fx1 = (x & 7) << 5;
            int gx2 = x >> 2, fx2 = (x & 3) << 6;

            int a = bilerp(g1[gy1][gx1], g1[gy1][gx1 + 1],
                           g1[gy1 + 1][gx1], g1[gy1 + 1][gx1 + 1], fx1, fy1);
            int b = bilerp(g2[gy2][gx2], g2[gy2][gx2 + 1],
                           g2[gy2 + 1][gx2], g2[gy2 + 1][gx2 + 1], fx2, fy2);
            int v = (a * 3 + b) >> 2;

            /* Radial falloff, squared, so the cloud ends in an edge rather
             * than in the corners of its tile. */
            int dx = ((x - NT_W / 2) * 256) / (NT_W / 2);
            int f  = 256 - (int)isqrt32((uint32_t)(dx * dx + dy * dy));
            if (f < 0) f = 0;
            f = (f * f) >> 8;
            v = (v * f) >> 8;

            /* Gain, then clip the haze off. Without enough gain the falloff
             * leaves nearly every pixel in the bottom half of the range and
             * the cloud gets drawn entirely out of the dark end of its own
             * palette, which is what a nebula you cannot see looks like. */
            v = ((v * 3) >> 1) - 20;
            if (v < 0) v = 0; else if (v > 255) v = 255;
            dst[y * NT_W + x] = (uint8_t)v;
        }
    }
}

static void neb_respawn(neb_t *n, int above)
{
    int c = rnd_range(0, 3);
    n->tile = (uint8_t)rnd_range(0, NT_N - 1);
    n->sh   = (uint8_t)rnd_range(1, 2);
    n->par  = (uint8_t)rnd_range(10, 34);
    n->vx   = (int16_t)rnd_range(-4, 4);

    int w = NT_W << n->sh, h = NT_H << n->sh;
    n->x_q8 = (int32_t)rnd_range(-w / 3, g_w - (2 * w) / 3) << 8;
    n->y_q8 = (int32_t)(above ? rnd_range(-h - 90, -h)
                              : rnd_range(-h, g_h)) << 8;

    /* The lookup table folds the background colour into its low end, so a
     * cloud fades out instead of ending. Rebuilding it per nebula is what
     * keeps the inner draw loop down to a compare, a shift and a load. */
    uint16_t base = s_grad[g_h / 2];
    for (int i = 0; i < 16; i++) {
        /* Reach the edge colour early and spend the rest of the range walking
         * into the core, so the bright middle of a cloud lands inside the
         * values the tile actually produces. */
        int t = i * 40;
        uint16_t col = mix565(base, NEB_EDGE[c], t > 255 ? 255 : t);
        if (i > 6) col = mix565(col, NEB_CORE[c], (i - 6) * 28);
        n->lut[i] = col;
    }
}

static void neb_draw(const neb_t *n)
{
    int sh = n->sh;
    int sx = (int)(n->x_q8 >> 8), sy = (int)(n->y_q8 >> 8);
    int w = NT_W << sh, h = NT_H << sh;

    int ya = g_band_y0, yb = g_band_y0 + g_band_h - 1;
    if (sy > ya) ya = sy;
    if (sy + h - 1 < yb) yb = sy + h - 1;
    if (ya > yb) return;

    int xa = sx < 0 ? 0 : sx;
    int xb = sx + w - 1;
    if (xb > g_w - 1) xb = g_w - 1;
    if (xa > xb) return;

    for (int y = ya; y <= yb; y++) {
        const uint8_t *src = &s_tile[n->tile][(y - sy) >> sh][0];
        const uint8_t *bay = &BAYER4[(y & 3) << 2];
        uint16_t *dst = &g_fb[(y - g_band_y0) * g_w + xa];
        for (int x = xa; x <= xb; x++) {
            /* Dither before the threshold, not after: testing the raw value
             * puts a hard contour around the cloud that no amount of dithering
             * inside it can hide. */
            int v = src[(x - sx) >> sh] + bay[x & 3];
            if (v >= 22) *dst = n->lut[v > 255 ? 15 : (v >> 4)];
            dst++;
        }
    }
}

/* ---- ring sections ---- */
/*
 * A section of a ringed planet's rings, seen from close enough that only an
 * arc of it fits on the panel: one enormous ellipse whose centre sits well off
 * screen, with concentric bands and a division through them. Solved per
 * scanline - for a given row each band contributes two chords - so the whole
 * thing costs a couple of square roots per row and no memory at all.
 */
#define RING_NB 5

/* inner %, outer %, brightness on the 0-31 ramp below */
static const uint8_t RING_BAND[RING_NB][3] = {
    { 50, 60, 12 },
    { 62, 70, 24 },
    { 72, 76,  8 },
    { 80, 91, 30 },
    { 93, 97, 18 },
};

/* Thirty-two steps, and even that is not enough on its own. The arc is shaded
 * by a fade that moves one part in 256 per row, so every ramp step lands as a
 * horizontal contour drawn clean across something that fills half the panel -
 * and a band of flat colour with a hard edge is far more obvious than the
 * gradient it is approximating. RDITH scatters each step over four rows. */
#define RING_RAMP 32
static uint16_t s_ringramp[RING_RAMP];
static const uint8_t RDITH[4] = { 2, 10, 6, 14 };

/*
 * Antialiasing for the ring, and the reason it needs its own machinery.
 *
 * Blending only the two end pixels of each scanline run - the obvious thing -
 * smooths an edge that is steeper than about 45 degrees and does nothing at
 * all for one that is shallower, because a shallow edge does not step across
 * x within a row, it steps across y between rows. On an ellipse this eccentric
 * both regimes are on screen at once: the arc stands vertical at its apex and
 * lies almost flat where it leaves the frame.
 *
 * So coverage comes from the sweep instead. An edge crosses a given row
 * somewhere between where it sat on the row above and where it sits on this
 * one, and over that interval the pixel coverage goes linearly from nothing to
 * everything. A sweep narrower than one pixel is widened to exactly one pixel
 * centred where it was, which turns the same formula back into plain area
 * coverage of a step - so one routine covers a vertical edge, a flat one, and
 * everything between.
 *
 * All of it reads the band buffer as well as writing it, which is only sound
 * because the ring is drawn straight after the nebulae and before anything
 * else: what it blends against is finished background, never half a rock.
 */
static void aa_edge(uint16_t *row, int32_t lo, int32_t hi, uint16_t c, int rise)
{
    int32_t w = hi - lo;
    if (w < 256) {                      /* narrower than a pixel: centre it */
        int32_t m = (lo + hi) >> 1;
        lo = m - 128;
        hi = m + 128;
        w  = 256;
    }
    int i0 = (int)(lo >> 8), i1 = (int)(hi >> 8);
    if (i0 < 0) i0 = 0;
    if (i1 > g_w - 1) i1 = g_w - 1;

    /* Reciprocal once instead of a divide per pixel. */
    int32_t inv = (256 << 12) / w;

    for (int x = i0; x <= i1; x++) {
        int cov = (int)(((((int32_t)x << 8) + 128 - lo) * inv) >> 12);
        if (cov < 0) cov = 0; else if (cov > 256) cov = 256;
        if (!rise) cov = 256 - cov;
        if (cov > 0) row[x] = mix565(row[x], c, cov);
    }
}

/*
 * One run bounded by two swept edges: coverage rises across [rlo, rhi], is
 * solid between, and falls across [flo, fhi]. All four in Q8 screen x.
 */
static void aa_run(int32_t rlo, int32_t rhi, int32_t flo, int32_t fhi,
                   int y, uint16_t c)
{
    int by = y - g_band_y0;
    if (by < 0 || by >= g_band_h) return;
    if (fhi <= rlo) return;
    uint16_t *row = &g_fb[by * g_w];

    /* Widen sub-pixel sweeps here too, so the solid span below starts where
     * the ramps actually finish. */
    if (rhi - rlo < 256) { int32_t m = (rlo + rhi) >> 1; rlo = m - 128; rhi = m + 128; }
    if (fhi - flo < 256) { int32_t m = (flo + fhi) >> 1; flo = m - 128; fhi = m + 128; }

    if (flo <= rhi) {
        /* The band is thinner than its own edges are wide. Nothing here is
         * fully covered, so treat it as one soft blob rather than letting the
         * two ramps fight over the same pixels. */
        aa_edge(row, rlo, fhi, c, 1);
        return;
    }

    aa_edge(row, rlo, rhi, c, 1);
    aa_edge(row, flo, fhi, c, 0);

    int a = (int)(rhi >> 8) + 1, b = (int)(flo >> 8) - 1;
    if (a < 0) a = 0;
    if (b > g_w - 1) b = g_w - 1;
    for (int x = a; x <= b; x++) row[x] = c;
}

/* sqrt in Q8. The shift happens before the root, so this is sqrt(v) * 256 to
 * the same accuracy isqrt32 gives on the whole number. v is bounded by the
 * largest ring radius squared, well inside the shift. */
static int32_t isqrt_q8(uint32_t v)
{
    return (int32_t)isqrt32(v << 16);
}

typedef struct {
    int32_t  cx_q8, cy_q8;
    int16_t  rx, ry;
    int16_t  ry0[RING_NB], ry1[RING_NB];
    int16_t  k0[RING_NB], k1[RING_NB];   /* rx/ry per band, Q8 */
    uint8_t  par;
    uint8_t  alive;
} ring_t;

static ring_t  s_ring;
static int32_t s_ring_next;     /* ms until the next one drifts in */

static void ring_spawn(void)
{
    ring_t *r = &s_ring;
    r->rx = (int16_t)rnd_range(150, 260);
    r->ry = (int16_t)((r->rx * rnd_range(30, 44)) / 100);

    /* Centred off one side, so what crosses the panel is an arc and not an
     * oval sitting in the middle of the screen. */
    int left = (int)(rnd() & 1);
    r->cx_q8 = (int32_t)(left ? -rnd_range(r->rx / 3, (r->rx * 2) / 3)
                              : g_w + rnd_range(r->rx / 3, (r->rx * 2) / 3)) << 8;
    r->cy_q8 = (int32_t)(-r->ry - rnd_range(20, 120)) << 8;
    r->par   = (uint8_t)rnd_range(40, 72);

    /* Per-band radii and the rx/ry ratio, hoisted out of the row loop: it is
     * the difference between two divisions per row and twenty. */
    for (int i = 0; i < RING_NB; i++) {
        int rx0 = (r->rx * RING_BAND[i][0]) / 100;
        int rx1 = (r->rx * RING_BAND[i][1]) / 100;
        r->ry0[i] = (int16_t)((r->ry * RING_BAND[i][0]) / 100);
        r->ry1[i] = (int16_t)((r->ry * RING_BAND[i][1]) / 100);
        r->k0[i] = (int16_t)(r->ry0[i] ? (rx0 << 8) / r->ry0[i] : 0);
        r->k1[i] = (int16_t)(r->ry1[i] ? (rx1 << 8) / r->ry1[i] : 0);
    }
    r->alive = 1;
}

/*
 * Where one ring band's two edges cross a given row, as offsets from the
 * ellipse centre in Q8. Returns 0 when the row misses the band; h0 comes back
 * zero when the row is past the inner ellipse and the chord is solid.
 */
static int ring_edges(const ring_t *r, int i, int dy, int32_t *h0, int32_t *h1)
{
    int ady = dy < 0 ? -dy : dy;
    int ry1b = r->ry1[i];
    *h0 = *h1 = 0;
    if (ady >= ry1b) return 0;

    *h1 = ((int32_t)r->k1[i] * isqrt_q8((uint32_t)(ry1b * ry1b - dy * dy))) >> 8;

    int ry0b = r->ry0[i];
    if (ady < ry0b)
        *h0 = ((int32_t)r->k0[i] * isqrt_q8((uint32_t)(ry0b * ry0b - dy * dy))) >> 8;
    return 1;
}

#define QMIN(a, b) ((a) < (b) ? (a) : (b))
#define QMAX(a, b) ((a) > (b) ? (a) : (b))

static void ring_draw(void)
{
    const ring_t *r = &s_ring;
    if (!r->alive) return;

    int cy = (int)(r->cy_q8 >> 8);
    int y0 = g_band_y0, y1 = g_band_y0 + g_band_h - 1;

    /*
     * Band-major, so each edge carries its position from the previous row and
     * the sweep costs two square roots per row rather than four. Seeded from
     * the row above the band: a sweep that restarted at every band boundary
     * would leave a hard seam every sixteen rows.
     */
    for (int i = 0; i < RING_NB; i++) {
        int32_t p0, p1;
        ring_edges(r, i, y0 - 1 - cy, &p0, &p1);

        for (int y = y0; y <= y1; y++) {
            int dy  = y - cy;
            int ady = dy < 0 ? -dy : dy;

            int32_t c0, c1;
            if (!ring_edges(r, i, dy, &c0, &c1)) { p0 = c0; p1 = c1; continue; }

            /* Dim toward the extremes of the arc, where the rings are
             * running away from the viewer rather than across.
             *
             * 255 and not 256 at the top: full scale is the one value that
             * survives the multiply below without losing a step, so a 256 here
             * paints the single centre row of the ellipse one level brighter
             * than either neighbour - a bright wire straight across. */
            int fade = 255 - (ady * 150) / r->ry;
            int lvl = (((RING_BAND[i][2] * fade) >> 4) + RDITH[y & 3]) >> 4;
            if (lvl < 0) lvl = 0;
            else if (lvl > RING_RAMP - 1) lvl = RING_RAMP - 1;
            uint16_t col = s_ringramp[lvl];

            int32_t o_lo = QMIN(p1, c1), o_hi = QMAX(p1, c1);

            if (c0 == 0 && p0 == 0) {
                /* Solid chord: one run across the whole width, so the pixel at
                 * the centre is not blended twice and left as a dark seam. */
                aa_run(r->cx_q8 - o_hi, r->cx_q8 - o_lo,
                       r->cx_q8 + o_lo, r->cx_q8 + o_hi, y, col);
            } else {
                int32_t i_lo = QMIN(p0, c0), i_hi = QMAX(p0, c0);
                aa_run(r->cx_q8 + i_lo, r->cx_q8 + i_hi,
                       r->cx_q8 + o_lo, r->cx_q8 + o_hi, y, col);
                aa_run(r->cx_q8 - o_hi, r->cx_q8 - o_lo,
                       r->cx_q8 - i_hi, r->cx_q8 - i_lo, y, col);
            }

            p0 = c0;
            p1 = c1;
        }
    }
}


/* ---- dust ---- */
/*
 * Moving in all directions is the point. A field that only slid downward would
 * read as a texture being scrolled; giving every mote its own parallax, its
 * own lateral drift and its own twinkle phase is what makes it read as space.
 * They wrap on all four edges, because the sideways movers have to come back.
 */
#define DUST_N 130

typedef struct {
    int32_t x_q8, y_q8;
    int16_t vx, vy;         /* px/s of its own, on top of the parallax */
    uint8_t par;
    uint8_t pal;
    uint8_t ph, rate;
} dust_t;

static dust_t s_dust[DUST_N];

#define DPAL_N 4
static uint16_t s_dustramp[DPAL_N][4];

static const uint16_t DUST_HI[DPAL_N] = {
    GB_RGB(255,255,255), GB_RGB(255,214,150),
    GB_RGB(170,225,255), GB_RGB(214,180,255),
};

static void dust_place(dust_t *d, int anywhere)
{
    d->x_q8 = (int32_t)rnd_range(0, g_w - 1) << 8;
    d->y_q8 = (int32_t)(anywhere ? rnd_range(0, g_h - 1) : -rnd_range(1, 12)) << 8;
    d->vx   = (int16_t)rnd_range(-22, 22);
    d->vy   = (int16_t)rnd_range(-14, 18);
    d->par  = (uint8_t)rnd_range(18, 255);
    d->pal  = (uint8_t)rnd_range(0, DPAL_N - 1);
    d->ph   = (uint8_t)rnd();
    d->rate = (uint8_t)rnd_range(20, 130);
}

/* ---- the layer as a whole ---- */

void bg_init(void)
{
    /* Empty space is not black: it is a very dark blue that gets marginally
     * warmer toward the bottom of the frame, which gives the scene a floor. */
    for (int y = 0; y < g_h; y++) {
        int t  = (y * 256) / (g_h ? g_h : 1);
        int r8 =  4 + ((11 * t) >> 8);
        int g8 =  3 + (( 6 * t) >> 8);
        int b8 = 14 + ((20 * t) >> 8);
        s_grad[y] = GB_RGB(r8, g8, b8);
        for (int k = 0; k < 4; k++) {
            /*
             * Half a quantisation step either way, which is the whole of it:
             * ordered dithering offsets by +-step/2 before the truncation, and
             * a full step - the obvious thing to write - dithers twice as hard
             * as it should. On these near-black colours that shows up as red
             * speckle, because a channel sitting half a step above zero spends
             * half its pixels clipped at zero.
             *
             * RGB565 steps are 8 on red and blue and 4 on green.
             */
            int bay = (int)BAYER4[((y & 3) << 2) + k];
            int r = r8 + (bay >> 1) - 4;
            int g = g8 + (bay >> 2) - 2;
            int b = b8 + (bay >> 1) - 4;
            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
            s_gradd[y][k] = GB_RGB(r, g, b);
        }
    }

    {
        static const uint16_t LO[NKIND] = {
            GB_RGB(  9,  9, 13), GB_RGB( 16,  8,  6), GB_RGB(  8, 13, 20) };
        static const uint16_t HI[NKIND] = {
            GB_RGB(204,200,194), GB_RGB(232,172,112), GB_RGB(220,242,255) };
        /* Not a linear ramp: a sphere shaded linearly reads as a ball bearing.
         * Slow at the dark end and quick through the middle puts a terminator
         * on the rock instead of a wash. */
        static const uint8_t CURVE[8] = { 0, 12, 34, 72, 118, 168, 214, 255 };
        for (int k = 0; k < NKIND; k++)
            for (int i = 0; i < 8; i++)
                s_rockramp[k][i] = mix565(LO[k], HI[k], CURVE[i]);
    }

    /* Bright, but not so bright that a rock crossing the arc stops reading.
     * The rings are scenery; they must not win a contrast fight with the
     * things that can kill you. */
    for (int i = 0; i < RING_RAMP; i++)
        s_ringramp[i] = mix565(GB_RGB(22, 18, 15), GB_RGB(198, 180, 146),
                               (i * 256) / (RING_RAMP - 1));

    for (int p = 0; p < DPAL_N; p++)
        for (int i = 0; i < 4; i++)
            s_dustramp[p][i] = mix565(s_grad[g_h / 2], DUST_HI[p], 40 + i * 72);

    for (int i = 0; i < NT_N; i++) gen_tile(&s_tile[i][0][0]);
    for (int i = 0; i < NEB_N; i++) neb_respawn(&s_neb[i], 0);
    for (int i = 0; i < DUST_N; i++) dust_place(&s_dust[i], 1);

    s_ring.alive = 0;
    s_ring_next  = rnd_range(1500, 9000);
    s_flash      = 0;
}

void bg_flash(void) { s_flash = 2; }

void bg_update(uint32_t dt_ms, int scroll_v)
{
    int dt = (int)dt_ms;
    if (s_flash) s_flash--;

    for (int i = 0; i < NEB_N; i++) {
        neb_t *n = &s_neb[i];
        int vy = (scroll_v * n->par) >> 8;
        n->y_q8 += ((int32_t)vy * dt * 256) / 1000;
        n->x_q8 += ((int32_t)n->vx * dt * 256) / 1000;
        if ((n->y_q8 >> 8) > g_h) neb_respawn(n, 1);
    }

    if (s_ring.alive) {
        int vy = (scroll_v * s_ring.par) >> 8;
        s_ring.cy_q8 += ((int32_t)vy * dt * 256) / 1000;
        if ((int)(s_ring.cy_q8 >> 8) - s_ring.ry > g_h) {
            s_ring.alive = 0;
            s_ring_next  = rnd_range(6000, 20000);
        }
    } else {
        s_ring_next -= dt;
        if (s_ring_next <= 0) ring_spawn();
    }

    for (int i = 0; i < DUST_N; i++) {
        dust_t *d = &s_dust[i];
        int vy = ((scroll_v * d->par) >> 8) + d->vy;
        d->y_q8 += ((int32_t)vy * dt * 256) / 1000;
        d->x_q8 += ((int32_t)d->vx * dt * 256) / 1000;
        /* >> 7, not >> 5: the fastest motes would otherwise cycle in a couple
         * of frames, which is not a twinkle, it is a fault light. This puts
         * the range at roughly a third of a second to two seconds. */
        d->ph = (uint8_t)(d->ph + ((d->rate * dt) >> 7));

        int x = (int)(d->x_q8 >> 8), y = (int)(d->y_q8 >> 8);
        if (y > g_h + 2) { dust_place(d, 0); continue; }
        if (y < -14)     d->y_q8 = (int32_t)(g_h + 1) << 8;
        if (x < -2)           d->x_q8 = (int32_t)(g_w + 1) << 8;
        else if (x > g_w + 2) d->x_q8 = -(1 << 8);
    }
}

void bg_draw(void)
{
    if (s_flash) {
        for (int y = g_band_y0; y < g_band_y0 + g_band_h; y++)
            fb_row(y, s_flash > 1 ? GB_RGB(255, 244, 220) : GB_RGB(120, 96, 96));
        return;
    }

    for (int y = g_band_y0; y < g_band_y0 + g_band_h; y++) {
        uint16_t *p = &g_fb[(y - g_band_y0) * g_w];
        const uint16_t *q = s_gradd[y];
        for (int x = 0; x < g_w; x++) *p++ = q[x & 3];
    }

    for (int i = 0; i < NEB_N; i++) neb_draw(&s_neb[i]);
    ring_draw();

    for (int i = 0; i < DUST_N; i++) {
        const dust_t *d = &s_dust[i];
        int y = (int)(d->y_q8 >> 8);
        if (y < g_band_y0 || y >= g_band_y0 + g_band_h) continue;
        int x = (int)(d->x_q8 >> 8);
        if (x < 0 || x >= g_w) continue;

        int t = d->ph < 128 ? d->ph : 255 - d->ph;      /* triangle, 0..127 */
        int lvl = t >> 5;
        if (d->par > 190 && lvl < 3) lvl++;
        uint16_t col = s_dustramp[d->pal][lvl];

        g_fb[(y - g_band_y0) * g_w + x] = col;
        /* The nearest motes get a second pixel. That is the whole depth cue -
         * anything more elaborate starts to look like snow. */
        if (d->par > 224 && lvl >= 2) fb_px(x, y + 1, col);
    }
}
