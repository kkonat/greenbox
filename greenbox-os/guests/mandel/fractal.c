/*
 * fractal.c - the per-pixel half of MANDEL.
 *
 * Three kernels, one raster, and the search that decides where to point them.
 *
 * Nothing here touches the panel. fr_at() answers one question about one pixel
 * - is this outside, and how far - and mandel.c turns the answer into a
 * colour. That split is what lets the location search render a 32-pixel-wide
 * preview through exactly the same code that later fills the screen: the
 * preview is not an approximation of the picture, it IS the picture, sampled
 * coarsely.
 *
 * All three kernels return the same thing: FR_INSIDE for a point in the set
 * (or, for Lyapunov, a chaotic one), and otherwise a Q8 value that grows
 * smoothly with distance from the boundary. Smoothly matters - an escape count
 * is an integer, and an integer painted through a 255-colour cycle gives the
 * concentric banding that makes a fractal look like a contour map. The
 * fractional part costs two log2 lookups per escaped pixel and removes it.
 */

#include "mandel.h"

/* ==================================================================== maths */

static uint32_t s_rng = 0x2545F491u;

void rnd_seed(uint32_t s) { s_rng = s ? s : 0x2545F491u; }
uint32_t rnd_state(void)  { return s_rng; }

uint32_t rnd(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

int rnd_range(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static fx rnd_fx(fx lo, fx hi)
{
    if (hi <= lo) return lo;
    return lo + (fx)(((uint64_t)rnd() * (uint32_t)(hi - lo)) >> 32);
}

/* Exact floor(sqrt(v)) by restoring shifts: no division, no float. The colour
 * mapping runs it once per pixel, which is why it is a fixed 16 steps rather
 * than a Newton iteration that would converge in a data-dependent number. */
uint32_t isqrt32(uint32_t v)
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

/*
 * A quarter of a sine, 65 points, linearly interpolated between them. The
 * error that leaves is under 1e-4 of full scale, which matters here in a way
 * it did not in astro: a Julia parameter is picked by walking the boundary of
 * the main cardioid, and a coarse sine would walk beside it instead of on it.
 */
static const int16_t SIN_Q15[65] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,
};

int32_t isin15(uint16_t turn)
{
    /* Fold a whole turn onto the quarter table: bit 15 is the sign, bit 14
     * mirrors the quarter, and what is left indexes it. */
    int neg = (turn & 0x8000u) != 0;
    uint16_t q = turn & 0x7FFFu;
    if (q & 0x4000u) q = (uint16_t)(0x8000u - q);

    int i = q >> 8;                     /* 0..64, and 64 only when f == 0 */
    int f = q & 255;
    int32_t v0 = SIN_Q15[i];
    int32_t v1 = SIN_Q15[i < 64 ? i + 1 : 64];
    int32_t v  = v0 + (((v1 - v0) * f) >> 8);
    return neg ? -v : v;
}

/*
 * log2 of a plain unsigned integer, Q16: the exponent from the top set bit,
 * then 32 points of log2(1+m) interpolated across the mantissa. That is all
 * the accuracy a colour index needs and cheaper than any series.
 */
static const uint32_t LOG2_TAB[33] = {   /* the last entry is 65536, not 65535 */
         0,   2909,   5732,   8473,  11136,  13727,  16248,  18704,
     21098,  23433,  25711,  27936,  30109,  32234,  34312,  36346,
     38336,  40286,  42196,  44068,  45904,  47705,  49472,  51207,
     52911,  54584,  56229,  57845,  59434,  60997,  62534,  64047,
     65536,
};

uint32_t ilog2_q16(uint32_t v)
{
    if (v == 0) return 0;

    int e = 31;
    while (!(v & 0x80000000u)) { v <<= 1; e--; }        /* NSAU on this core */

    uint32_t m = (v >> 16) & 0x7FFFu;   /* 15 bits below the implicit one */
    int i = m >> 10;
    int f = m & 1023;
    uint32_t lo = LOG2_TAB[i], hi = LOG2_TAB[i + 1];
    return ((uint32_t)e << 16) + lo + (((hi - lo) * (uint32_t)f) >> 10);
}

/* ==================================================================== scene */

static uint8_t  s_mode;
static uint16_t s_maxiter = 100;
static fx       s_jx, s_jy;
static uint8_t  s_seq[SEQ_MAX];
static uint8_t  s_len = 2;

static fx s_cx, s_cy, s_hw;         /* the view, in world units */
static fx s_x0, s_y0, s_step;       /* the raster it is currently mapped to */

/* Lyapunov settles before it measures: the first LY_WARM steps of the map are
 * thrown away so the exponent is taken on the attractor rather than on the way
 * to it, and LY_ITER of them are averaged. */
#define LY_WARM  30
#define LY_ITER  70

void fr_scene(const scene_t *s)
{
    s_mode    = s->mode;
    s_maxiter = s->maxiter ? s->maxiter : 100;
    s_jx      = s->jx;
    s_jy      = s->jy;
    s_cx      = s->cx;
    s_cy      = s->cy;
    s_hw      = s->hw ? s->hw : FX(1.6);
    s_len     = s->seq_len ? s->seq_len : 2;
    if (s_len > SEQ_MAX) s_len = SEQ_MAX;
    for (int i = 0; i < SEQ_MAX; i++) s_seq[i] = (uint8_t)(s->seq[i] & 1);

    fr_view(1, 1);                  /* something valid until fr_view is called */
}

void fr_view(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    /* One pitch for both axes, so a view is a rectangle of the world with the
     * same shape as the panel and nothing is ever stretched. The half-pitch
     * puts the sample in the middle of its pixel rather than on its corner,
     * which is what makes a preview line up with the render it stands for. */
    s_step = (fx)(((int64_t)s_hw * 2) / w);
    if (s_step < 1) s_step = 1;
    s_x0 = s_cx - (fx)((int64_t)s_step * w / 2) + (s_step >> 1);
    s_y0 = s_cy - (fx)((int64_t)s_step * h / 2) + (s_step >> 1);
}

/* ------------------------------------------------------------- the kernels */

/*
 * Escape time for z <- z*z + c, shared by both quadratic modes: Mandelbrot
 * hands it c = the pixel and z = 0, Julia hands it z = the pixel and a c that
 * is the same all over the screen. One kernel, because it is one iteration.
 *
 * The squares stay in int64 across the escape test on purpose. |z| <= 2 at the
 * top of the loop so the coordinates themselves fit Q28, but the moment a
 * point escapes its square can reach 36 - past what Q28 holds - and that
 * overshoot is exactly the value the smooth count needs.
 */
static int32_t escape(fx cx, fx cy, fx zx, fx zy)
{
    const int64_t esc = (int64_t)4 << FX_BITS;

    int64_t x2 = ((int64_t)zx * zx) >> FX_BITS;
    int64_t y2 = ((int64_t)zy * zy) >> FX_BITS;
    int n = 0;

    while (x2 + y2 <= esc) {
        if (n >= s_maxiter) return FR_INSIDE;
        zy = (fx)(((int64_t)zx * zy) >> (FX_BITS - 1)) + cy;    /* 2xy + cy */
        zx = (fx)(x2 - y2) + cx;
        x2 = ((int64_t)zx * zx) >> FX_BITS;
        y2 = ((int64_t)zy * zy) >> FX_BITS;
        n++;
    }

    /* nu = n + 1 - log2(log2|z|). The escaped |z| says how far past the escape
     * radius this last iteration overshot, and subtracting it makes the value
     * continuous across the whole exterior instead of stepping once per
     * iteration. */
    int64_t m = x2 + y2;
    int sh = 0;
    while (m > 0x7FFFFFFFll) { m >>= 1; sh++; }

    int32_t l2 = (int32_t)(ilog2_q16((uint32_t)m) + ((uint32_t)sh << 16))
               - (FX_BITS << 16);               /* log2(|z|^2), Q16 */
    if (l2 < 2) l2 = 2;
    int32_t lz = l2 >> 1;                       /* log2|z|, in (1, 2.6) */
    int32_t ll = (int32_t)ilog2_q16((uint32_t)lz) - (16 << 16);

    int32_t nu = ((int32_t)n << 16) + (1 << 16) - ll;
    if (nu < 0) nu = 0;
    return nu >> 8;
}

/*
 * The main cardioid and the period-2 bulb, tested in closed form. They are
 * most of the black in a wide view and every point in them costs a full
 * maxiter otherwise, so this is the difference between a first screen in half
 * a second and one in two. The guard keeps the intermediate products small
 * enough to sit comfortably inside int64; nothing outside that box is in
 * either lobe anyway.
 */
static int in_bulbs(fx x, fx y)
{
    if (x > FX(0.6) || x < FX(-1.4) || y > FX(1.2) || y < FX(-1.2)) return 0;

    int64_t y2 = ((int64_t)y * y) >> FX_BITS;
    fx      xq = x - FX(0.25);
    int64_t q  = (((int64_t)xq * xq) >> FX_BITS) + y2;

    if (((q * (q + xq)) >> FX_BITS) < (y2 >> 2)) return 1;      /* cardioid */

    fx      xp = x + FX_ONE;
    int64_t d  = (((int64_t)xp * xp) >> FX_BITS) + y2;
    return d < (int64_t)FX(0.0625);                             /* the bulb */
}

/*
 * Lyapunov. The logistic map is driven by a repeating word over two rates -
 * "ab", "aabab" - and the exponent is the average of log2|r(1-2x)| along the
 * orbit. Negative means the orbit settles, and those points are drawn;
 * positive means it never does, and chaos is painted as interior, because the
 * whole picture is the shape of the border between the two.
 *
 * x stays in [0,1] and r in [2,4], so every product here fits Q28 without a
 * guard. The one value that needs care is the derivative when it lands on
 * zero, and ilog2_q16(0) answering 0 turns that into a large negative term -
 * which is what a superstable orbit ought to contribute.
 */
static int32_t lyapunov(fx ra, fx rb)
{
    fx x = FX_ONE >> 1;
    int32_t sum = 0;
    int k = 0;

    for (int i = 0; i < LY_WARM + LY_ITER; i++) {
        fx r = s_seq[k] ? rb : ra;
        if (++k >= s_len) k = 0;

        if (i >= LY_WARM) {
            fx d = (fx)(((int64_t)r * (FX_ONE - 2 * x)) >> FX_BITS);
            uint32_t a = (uint32_t)(d < 0 ? -d : d);
            sum += (int32_t)ilog2_q16(a) - (FX_BITS << 16);
        }
        x = (fx)(((int64_t)r * (((int64_t)x * (FX_ONE - x)) >> FX_BITS))
                 >> FX_BITS);
    }

    int32_t lam = sum / LY_ITER;                /* Q16 */
    if (lam >= 0) return FR_INSIDE;             /* chaotic */
    return (-lam) >> 8;                         /* Q8, rising into order */
}

/* ---------------------------------------------------------------- sampling */

static int32_t sample(fx a, fx b)
{
    switch (s_mode) {
    case MODE_JULIA:  return escape(s_jx, s_jy, a, b);
    case MODE_LYAP:   return lyapunov(a, b);
    default:          return in_bulbs(a, b) ? FR_INSIDE : escape(a, b, 0, 0);
    }
}

int32_t fr_at(int px, int py)
{
    return sample(s_x0 + (fx)(px * s_step), s_y0 + (fx)(py * s_step));
}

/*
 * The four quarter-points of one pixel. With the centre the caller already has,
 * that is a quincunx: five samples, and the cheapest arrangement that puts one
 * on each diagonal.
 *
 * They are handed back rather than combined here because the combination is of
 * COLOURS, and colour is not this file's business. Averaging the values first
 * would be the wrong operation and not much cheaper - halfway along a filament
 * the five values are scattered across the whole range, and the colour of
 * their average is not the average of their colours. It also could not soften
 * the edge of the set, where four of the five samples have no value at all.
 */
void fr_at_ss4(int px, int py, int32_t out[4])
{
    fx q = s_step >> 2;
    if (q < 1) q = 1;

    fx x = s_x0 + (fx)(px * s_step);
    fx y = s_y0 + (fx)(py * s_step);

    out[0] = sample(x - q, y - q);
    out[1] = sample(x + q, y - q);
    out[2] = sample(x - q, y + q);
    out[3] = sample(x + q, y + q);
}

/*
 * Is there a visible step in colour between these two pixels? Asked in palette
 * steps rather than in iterations, so that it means the same thing at every
 * zoom, in every mode and under every palette: the question is whether the
 * picture jumps here, and the palette is what decides that.
 *
 * Taking indices rather than values is what lets the refinement pass run off
 * the stored picture instead of needing the values kept as well - 240 bytes of
 * lag instead of 130 KB of value buffer.
 */
int fr_jump(uint8_t a, uint8_t b)
{
    if ((a == 0) != (b == 0)) return 1;         /* one of them is interior */
    int d = (int)a - (int)b;
    if (d < 0) d = -d;
    if (d > 128) d = 256 - d;                   /* the cycle wraps */
    return d > FR_JUMP_STEPS;
}

/* ================================================================ the search */
/*
 * Pointing this somewhere at random gives a dull picture nearly every time:
 * the inside of the set is one flat colour, the far outside is another, and
 * the only place worth rendering is the hair between them. Two things find it.
 *
 * The first is a bisection. Take any point known to be inside and any point
 * known to be outside; the segment between them crosses the boundary, so halve
 * it until the ends are one screen-width apart and the crossing is somewhere
 * in the middle, at exactly the scale about to be drawn. That is a guarantee
 * rather than a hope, and it costs about forty escape tests.
 *
 * The second is the preview. Straddling the boundary is necessary and not
 * sufficient - a filament edge can be perfectly smooth and perfectly boring -
 * so each candidate is rendered 32 pixels wide through the real kernel and
 * scored for how much is actually going on. Six hundred samples per try, forty
 * tries at the outside, and a deadline over the lot.
 */

#define PRE_MAX     32
#define FIND_TRIES  40
#define FIND_MS     700
/*
 * The bar a candidate has to clear to be accepted on the spot. Scores run from
 * about 100 to about 210 in practice, and the two ends are easy to tell apart
 * by eye: 120 is a smooth wash with one filament across it, 200 is a frame
 * that is busy corner to corner. 175 is set high enough that most candidates
 * are refused - which is the point, since refusing costs a millisecond and
 * accepting costs a second of rendering plus however long it is looked at.
 * Whatever scored highest is used when the clock runs out, so a demanding bar
 * never means no picture.
 */
#define FIND_GOOD   175

static int32_t s_prev[PRE_MAX * PRE_MAX];

static int cls_inside(fx x, fx y) { return sample(x, y) == FR_INSIDE; }

static fx fxabs(fx v) { return v < 0 ? -v : v; }

/*
 * Land on the boundary at the scale hw. Returns 0 when the box handed in has
 * no interior at all - a dust Julia set has none - and the caller falls back
 * to a wide view, which is the right picture for that case anyway.
 */
static int find_boundary(fx hw, fx *ox, fx *oy,
                         fx x_lo, fx x_hi, fx y_lo, fx y_hi)
{
    fx ax = 0, ay = 0, bx = 0, by = 0;
    int have_in = 0, have_out = 0;

    for (int i = 0; i < 160 && !(have_in && have_out); i++) {
        fx x = rnd_fx(x_lo, x_hi), y = rnd_fx(y_lo, y_hi);
        if (cls_inside(x, y)) {
            if (!have_in)  { ax = x; ay = y; have_in  = 1; }
        } else {
            if (!have_out) { bx = x; by = y; have_out = 1; }
        }
    }
    if (!have_in || !have_out) return 0;

    for (int i = 0; i < 40; i++) {
        fx dx = bx - ax, dy = by - ay;
        if (fxabs(dx) <= hw && fxabs(dy) <= hw) break;
        fx mx = ax + (dx >> 1), my = ay + (dy >> 1);
        if (cls_inside(mx, my)) { ax = mx; ay = my; }
        else                    { bx = mx; by = my; }
    }

    *ox = ax + ((bx - ax) >> 1);
    *oy = ay + ((by - ay) >> 1);
    return 1;
}

/* Iterations worth spending at this zoom. The detail near the boundary gets
 * finer as you descend and a count that does not follow it turns filaments
 * into blobs; a count that overshoots only costs time on the black. */
static uint16_t iters_for(fx hw)
{
    if (hw <= 0) hw = FX(1.6);
    uint32_t zoom = (uint32_t)((((int64_t)FX(1.6)) << 8) / hw);   /* Q8 */
    int32_t  l    = (int32_t)ilog2_q16(zoom) - (8 << 16);         /* log2 */
    if (l < 0) l = 0;
    int it = 72 + (int)((l * 24) >> 16);
    if (it > 300) it = 300;
    return (uint16_t)it;
}

/* hw scaled by 0.707 u times over: a log-uniform zoom, which is the only kind
 * that makes sense when every level looks like the one above it. */
static fx zoom_down(fx hw, int u)
{
    for (int i = 0; i < u; i++) hw = (fx)(((int64_t)hw * 181) >> 8);
    return hw;
}

static void propose(uint8_t mode, scene_t *s, int u)
{
    memset(s, 0, sizeof *s);
    s->mode = mode;
    s->seq_len = 2;
    s->seq[0] = 0;
    s->seq[1] = 1;

    switch (mode) {

    case MODE_JULIA: {
        /*
         * c is picked on the boundary of the main cardioid or of the period-2
         * bulb and then nudged off it. That is where the interesting Julia
         * sets live: well inside is a fat featureless blob, far outside is
         * dust, and the boundary itself is a dendrite. The nudge is a few
         * thousandths - small enough to stay in the interesting band, random
         * enough that connected and dusty both come up.
         */
        uint16_t th = (uint16_t)rnd();
        if (rnd() & 1) {
            /* the cardioid: c = e^it/2 - e^2it/4 */
            int32_t c1 = icos15(th), s1 = isin15(th);
            uint16_t t2 = (uint16_t)(th * 2u);
            int32_t c2 = icos15(t2), s2 = isin15(t2);
            s->jx = (fx)(((int64_t)c1 * (FX_ONE / 2)) >> 15)
                  - (fx)(((int64_t)c2 * (FX_ONE / 4)) >> 15);
            s->jy = (fx)(((int64_t)s1 * (FX_ONE / 2)) >> 15)
                  - (fx)(((int64_t)s2 * (FX_ONE / 4)) >> 15);
        } else {
            /* the period-2 bulb: c = -1 + e^it/4 */
            s->jx = -FX_ONE + (fx)(((int64_t)icos15(th) * (FX_ONE / 4)) >> 15);
            s->jy =           (fx)(((int64_t)isin15(th) * (FX_ONE / 4)) >> 15);
        }
        fx eps = rnd_fx(FX(0.0006), FX(0.010));
        s->jx += rnd_fx(-eps, eps);
        s->jy += rnd_fx(-eps, eps);

        s->hw = zoom_down(FX(1.5), u);
        s->maxiter = iters_for(s->hw);
        s->cx = s->cy = 0;
        fr_scene(s);
        if (u > 2 && !find_boundary(s->hw, &s->cx, &s->cy,
                                    FX(-1.5), FX(1.5), FX(-1.5), FX(1.5))) {
            s->cx = s->cy = 0;
            s->hw = FX(1.5);
            s->maxiter = iters_for(s->hw);
        }
        break;
    }

    case MODE_LYAP: {
        /*
         * A random word over {a,b}, forced to contain both letters, because
         * "aaa" is the plain logistic map and draws a set of vertical stripes.
         * The window is a square somewhere in [2,4]^2, which is where the map
         * is defined and where the ordered tongues grow.
         */
        s->seq_len = (uint8_t)rnd_range(2, SEQ_MAX);
        for (int i = 0; i < s->seq_len; i++) s->seq[i] = (uint8_t)(rnd() & 1);
        s->seq[rnd_range(0, s->seq_len - 1)] = 0;
        s->seq[rnd_range(0, s->seq_len - 1)] = 1;
        if (s->seq[0] == s->seq[1] && s->seq_len == 2) s->seq[1] ^= 1;

        s->maxiter = LY_ITER;
        fx hw = zoom_down(FX(0.95), u);
        if (hw < FX(0.004)) hw = FX(0.004);
        s->hw = hw;

        /* Keep the whole window inside the domain: past [2,4] the map runs
         * away and the exponent stops meaning anything. */
        fx lo = FX(2.0) + hw, hi = FX(4.0) - hw;
        s->cx = rnd_fx(lo, hi);
        s->cy = rnd_fx(lo, hi);
        fr_scene(s);
        if (find_boundary(hw, &s->cx, &s->cy, lo, hi, lo, hi)) {
            if (s->cx < lo) s->cx = lo; else if (s->cx > hi) s->cx = hi;
            if (s->cy < lo) s->cy = lo; else if (s->cy > hi) s->cy = hi;
        }
        break;
    }

    default: {
        fx hw = zoom_down(FX(1.6), u);
        if (hw < FX(0.00012)) hw = FX(0.00012);     /* the Q28 floor */
        s->hw = hw;
        s->maxiter = iters_for(hw);
        s->cx = FX(-0.6);
        s->cy = 0;
        fr_scene(s);
        if (find_boundary(hw, &s->cx, &s->cy,
                          FX(-2.2), FX(0.7), FX(-1.25), FX(1.25))) {
            /* Off-centre by up to half a view, so the boundary is not always
             * pinned to the middle of the screen like a specimen. */
            s->cx += rnd_fx(-(hw >> 1), hw >> 1);
            s->cy += rnd_fx(-(hw >> 1), hw >> 1);
        } else {
            s->hw = FX(1.6);
            s->maxiter = iters_for(s->hw);
        }
        break;
    }
    }

    s->vmax   = (mode == MODE_LYAP) ? 640 : ((int32_t)s->maxiter << 8);
    /* One trip round the palette across the whole range, or two. More than two
     * and the bands near the boundary are thinner than a pixel, which reads as
     * speckle rather than as detail - the picture stops being a picture. */
    s->cycles = (uint8_t)rnd_range(1, 2);
    s->phase  = (uint8_t)(rnd() & 255u);
}

/*
 * How much is going on in a preview, out of about 210.
 *
 * Three terms, because each one alone has a dull picture that passes it.
 * Variety counts how much of the value range is used at all - a wash of two
 * shades scores nothing. Edges count how often the frame crosses between
 * inside and outside, which is the boundary being present rather than merely
 * nearby. Gradient measures how fast the value moves between neighbours,
 * relative to the range within the frame, so that it means the same thing for
 * an escape count and for a Lyapunov exponent.
 */
static int score_preview(int w, int h)
{
    const int n = w * h;
    int n_in = 0;
    int32_t vmin = 0x7FFFFFFF, vmax = -1;

    for (int i = 0; i < n; i++) {
        int32_t v = s_prev[i];
        if (v == FR_INSIDE) { n_in++; continue; }
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    if (n - n_in < n / 24) return 0;        /* all interior, nothing to look at */

    int32_t span = vmax - vmin + 1;
    int edges = 0, npair = 0;
    int64_t grad = 0;
    uint32_t bits = 0;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int32_t a = s_prev[y * w + x];
            if (a != FR_INSIDE)
                bits |= 1u << (int)(((int64_t)(a - vmin) * 16) / span);

            for (int d = 0; d < 2; d++) {
                int nx = x + (d == 0), ny = y + (d == 1);
                if (nx >= w || ny >= h) continue;
                int32_t b = s_prev[ny * w + nx];
                if ((a == FR_INSIDE) != (b == FR_INSIDE)) { edges++; continue; }
                if (a == FR_INSIDE) continue;
                int32_t dv = a - b;
                grad += dv < 0 ? -dv : dv;
                npair++;
            }
        }
    }

    int nb = 0;
    for (uint32_t m = bits; m; m >>= 1) nb += (int)(m & 1);

    int s_var  = nb * 6;                                    /* 0..96 */
    int s_edge = (edges * 200) / n;
    if (s_edge > 70) s_edge = 70;
    int s_grad = npair ? (int)((grad * 400) / ((int64_t)npair * span)) : 0;
    if (s_grad > 40) s_grad = 40;

    int sc = s_var + s_edge + s_grad;
    int frac = (n_in * 256) / n;
    if (frac > 200) sc /= 3;                    /* nearly all black */
    if (nb <= 2 && edges * 40 < n) sc /= 2;     /* a flat wash with an edge */
    return sc;
}

int fr_find(uint8_t mode, scene_t *out)
{
    int pw, ph;
    if (g_w >= g_h) { pw = PRE_MAX; ph = (PRE_MAX * g_h) / g_w; }
    else            { ph = PRE_MAX; pw = (PRE_MAX * g_w) / g_h; }
    if (pw < 4) pw = 4;
    if (ph < 4) ph = 4;

    /*
     * The scale is drawn once and every candidate is proposed at it, because
     * a search free to pick the zoom as well would always come back with the
     * deepest one it tried - detail is what the score measures, and there is
     * more of it further down. That is a viewer that only ever shows filaments.
     * Fixing the scale first turns the search into the question it should be
     * asking: given this much of the plane, where is the best of it.
     */
    int u;
    switch (mode) {
    case MODE_JULIA: u = rnd_range(0, 10); break;
    case MODE_LYAP:  u = rnd_range(0, 8);  break;
    default:         u = rnd_range(0, 26); break;
    }

    uint32_t start = A->millis();
    int best = -1;

    for (int t = 0; t < FIND_TRIES; t++) {
        scene_t s;
        propose(mode, &s, u);

        fr_scene(&s);
        fr_view(pw, ph);
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++)
                s_prev[y * pw + x] = fr_at(x, y);

        int sc = score_preview(pw, ph);
        if (sc > best) { best = sc; *out = s; }
        if (sc >= FIND_GOOD) break;
        /* Always give it three honest tries; after that the clock decides,
         * because a program that thinks for two seconds before drawing looks
         * broken however good the picture turns out to be. */
        if (t >= 2 && A->millis() - start >= FIND_MS) break;
    }
    return best;
}

int fr_valid(const scene_t *s)
{
    if (s->mode >= MODE_COUNT) return 0;
    if (s->hw <= 0 || s->hw > FX(4.0)) return 0;
    if (s->maxiter < 8 || s->maxiter > 400) return 0;
    if (s->vmax <= 0) return 0;
    if (s->cycles < 1 || s->cycles > 8) return 0;
    if (s->mode == MODE_LYAP && (s->seq_len < 2 || s->seq_len > SEQ_MAX)) return 0;
    return 1;
}
