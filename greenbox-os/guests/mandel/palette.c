/*
 * palette.c - where the colours come from.
 *
 * Five colours, spaced along a ramp and blended between. That is the shape
 * every palette tool settles on, and it is the right one here for a reason
 * specific to this program: a fractal shows a palette as bands, so what the
 * eye ends up judging is not the five colours but the two hundred and fifty
 * blends between them.
 *
 * Which five is the whole question. The classical harmonies are monochromatic,
 * analogous, complementary, split-complementary, triadic and tetradic - the
 * same hue offsets in every generator ever written - but they are not equally
 * usable here. Triadic and tetradic put three or four saturated hues a third
 * of the wheel apart, and a blend between those IS a rainbow; run it through a
 * fractal at two cycles to the screen and the result is the psychedelic mess
 * this file used to produce.
 *
 * What replaced it is not a harmony from the list but a walk: the palette
 * starts at a dark hue and moves towards the hue of light as it brightens.
 * Outdoors the light is warm, so the brighter something gets the closer to
 * yellow it moves - embers to gold, sea to sand, moss to straw. Walking the
 * other way, green through blue into violet as it brightens, is a thing
 * daylight never does, and it reads at once as a machine choosing colours.
 *
 * The walk has to be LONG, and getting that wrong cost this file two rewrites.
 * A short walk gives five colours that are all the same colour: one hue dimmed
 * and brightened, which is a tonal ramp and not a palette. So the starting
 * hues are chosen for their distance from the light rather than for their own
 * sake - a warm palette begins at wine and comes round through red and ember,
 * rather than beginning at amber with nowhere to go - and the walk crosses
 * three or four hue families on its way.
 *
 * That is also how magenta gets back in, having been thrown out once for
 * having nothing behind it outdoors. True of magenta on its own; false of the
 * road through it, since violet to rose to ember is the most ordinary sight
 * there is. The difference is saturation and not hue, so the arc is walked in
 * dust: any stop landing near magenta gives up most of its saturation. What
 * made the old palettes psychedelic was never one hue, it was hues that had no
 * business being adjacent.
 *
 * Value carries the rest. All five stops climb from shadow to a tinted
 * near-white, saturation falling as they rise - the difference between a
 * colour that reads as lit and one that reads as painted - and all five are
 * meant to be seen, which is why the climb does not double back. The cycle is
 * closed by folding instead: see interpolate().
 *
 * Monochrome is kept at about one palette in twelve, drawn from its own short
 * list of hues that hold a whole screen on their own. It is the most
 * harmonious scheme there is and the least interesting, which is the right
 * ratio for something that will be looked at for a while.
 *
 * Two details of the blending matter as much as the colours. The stops are
 * spaced unevenly, because five equal steps look mechanical and letting one
 * band run long while another turns quickly is what a sunset does. And the
 * blend runs in linear light - square, mix, square-root back - because a
 * straight average of two sRGB values is darker than the light it claims to be
 * mixing, and blended naively red to blue sags through a muddy plum in the
 * middle. That mud is the thing this file exists to avoid.
 *
 * The whole palette is reproducible from its 32-bit seed, which is what makes
 * it cheap to persist: the saved record keeps the seed, not the table.
 */

#include "mandel.h"

uint16_t g_pal[256];
uint32_t g_pal_k = 1 << 16;
uint32_t g_pal_phase;

static uint32_t s_seed;
static uint32_t s_state;

/* A second generator, seeded from the palette seed alone, so that regenerating
 * a palette from its seed gives the same palette however much the program has
 * used the global one since. */
static uint32_t p_rnd(void)
{
    uint32_t x = s_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_state = x;
    return x;
}

static int p_range(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(p_rnd() % (uint32_t)(hi - lo + 1));
}

#define STOPS 5

/* The palette is a ramp of five colours folded in half: entry PAL_PEAK is the
 * light end, and the second half of the table is the first half backwards. */
#define PAL_PEAK 128

static uint8_t s_stop[STOPS][3];    /* the five colours */
static uint8_t s_pos[STOPS];        /* and where each one sits in the cycle */

/* 256 x 3 bytes of working colour, kept off the stack: a guest gets 4 KB of it
 * and this would be a fifth of that. */
static uint8_t s_rgb[256][3];

/* ------------------------------------------------------------------ colour */

/* Hue runs 0..1535 - six sectors of 256 - so the sector is a shift and the
 * fraction within it is a byte. */
static void hsv2rgb(int h, int s, int v, uint8_t out[3])
{
    h %= 1536;
    if (h < 0) h += 1536;
    if (s < 0) s = 0; else if (s > 255) s = 255;
    if (v < 0) v = 0; else if (v > 255) v = 255;

    int sec = h >> 8, f = h & 255;
    int p = (v * (255 - s)) >> 8;
    int q = (v * (255 - ((s * f) >> 8))) >> 8;
    int t = (v * (255 - ((s * (255 - f)) >> 8))) >> 8;

    switch (sec) {
    case 0:  out[0] = (uint8_t)v; out[1] = (uint8_t)t; out[2] = (uint8_t)p; break;
    case 1:  out[0] = (uint8_t)q; out[1] = (uint8_t)v; out[2] = (uint8_t)p; break;
    case 2:  out[0] = (uint8_t)p; out[1] = (uint8_t)v; out[2] = (uint8_t)t; break;
    case 3:  out[0] = (uint8_t)p; out[1] = (uint8_t)q; out[2] = (uint8_t)v; break;
    case 4:  out[0] = (uint8_t)t; out[1] = (uint8_t)p; out[2] = (uint8_t)v; break;
    default: out[0] = (uint8_t)v; out[1] = (uint8_t)p; out[2] = (uint8_t)q; break;
    }
}

/*
 * Yellow, near enough: the hue that light itself has, and the one every
 * palette here is walking towards as it brightens.
 */
#define HUE_LIGHT 256

/*
 * Where a palette starts, and which way round the wheel it walks to reach the
 * light. The hue here is the DARK end - where the palette begins, not where it
 * spends its time - so every one of these is a shadow colour.
 *
 * Both halves of each entry matter, and the second one is what the first
 * version of this got wrong. A palette that starts warm has almost no distance
 * left to travel, because it is already nearly the colour of light: maroon to
 * gold is under fifty degrees, one hue family, and it comes out looking like a
 * single colour dimmed and brightened however many stops are put along it. The
 * warm palettes have to start on the FAR side and come round - wine through
 * red and ember to gold - and the dusk one keeps going up through the arc that
 * daylight only ever shows at the two ends of the day.
 *
 * Which is where magenta gets back in, having been thrown out earlier for
 * having nothing behind it outdoors. That was true of magenta on its own and
 * false of the road through it: violet to rose to ember is the most ordinary
 * sight there is. What makes the old palettes psychedelic was never one hue,
 * it was hues that had no business being adjacent.
 */
static const struct { uint16_t lo, hi; uint8_t up; } BAND[4] = {
    { 1380, 1500, 1 },      /* wine, oxblood -> red, ember, amber, gold  */
    {  780, 1020, 0 },      /* teal, deep water -> sea, moss, straw      */
    { 1020, 1160, 0 },      /* indigo -> blue, teal, leaf, straw         */
    { 1160, 1300, 1 },      /* dusk violet -> rose, ember, amber, gold   */
};

/* ------------------------------------------------------------- the schemes */

enum { SCH_MONO = 0, SCH_ANALOG, SCH_EARTH, SCH_AIRY, SCH_DEEP, SCH_COUNT };

/*
 * The journey: a straight climb from shadow to light, and all five stops on
 * it are meant to be seen.
 *
 * The version before this one spent a stop on getting back down to the dark
 * end, so that the cycle would close - and it cost far more than a stop. Of
 * the four that were left, the two dark ones read as near-black whatever hue
 * they were given and the brightest was nearly white, which leaves exactly one
 * stop showing a colour. That is why five stops kept coming out looking like
 * one: not because the hues were too close, but because only one of them was
 * ever visible.
 *
 * So the climb keeps all five, and the cycle is closed by mirroring instead -
 * see interpolate(). Saturation falls as the value rises, which is what
 * distinguishes light falling on a thing from paint applied to it, but it
 * falls late: the top stop is a tint rather than a white, so it is still one
 * of the five colours and not just the end of the ramp.
 */
static const uint8_t  V_PROFILE[STOPS] = {  30,  90, 160, 215, 250 };
static const uint8_t  S_PROFILE[STOPS] = { 200, 210, 185, 140,  60 };

/*
 * Where each stop sits along the walk, as a fraction of it: the value profile,
 * normalised. Hue is a function of BRIGHTNESS rather than of which stop this
 * happens to be, so a family that squashes the value profile squashes the hue
 * walk with it and stays one lit scene rather than becoming a spectrum.
 */
static const uint16_t V_NORM[STOPS] = { 0, 70, 151, 215, 256 };

static void scheme(void)
{
    /*
     * A monochrome palette has no walk to carry it, so its one hue has to be
     * worth looking at on its own for a whole screen - and it cannot be drawn
     * from the bands above, because two of those start next to magenta on
     * purpose, meaning only to pass through it on the way somewhere warm. Land
     * a monochrome there and the entire picture is pink.
     */
    static const uint16_t MONO_HUE[4][2] = {
        {  480,  700 },     /* forest, moss  */
        {  780, 1020 },     /* teal, sea     */
        { 1020, 1160 },     /* indigo, slate */
        { 1450, 1535 },     /* deep red      */
    };

    int band = p_range(0, 3);
    int h0   = p_range(BAND[band].lo, BAND[band].hi);   /* the dark end */

    /*
     * Monochrome is kept, at about one palette in twelve. It is the most
     * harmonious scheme there is and the least interesting, which is the right
     * ratio for something that is going to be looked at for a while: often
     * enough to be a change of pace, rarely enough not to be the house style.
     */
    int mono = p_range(0, 99) < 8;
    int fam  = mono ? SCH_MONO : p_range(SCH_ANALOG, SCH_COUNT - 1);

    if (mono) {
        int i = p_range(0, 3);
        h0 = p_range(MONO_HUE[i][0], MONO_HUE[i][1]);
    }

    /*
     * How far along the walk the bright end gets, out of 256.
     *
     * This is the number that decides whether the result is five colours or
     * one colour in five brightnesses, and the first version of this file had
     * it far too low - a fiftieth of a turn is a tonal ramp, not a palette.
     * Anything from about a third of the way up is unmistakably several
     * colours: indigo, blue, teal, gold, cream is one walk, not five choices.
     */
    int reach;
    switch (fam) {
    case SCH_MONO:  reach = p_range(0, 26);    break;
    case SCH_ANALOG:reach = p_range(150, 200); break;
    case SCH_EARTH: reach = p_range(180, 235); break;
    case SCH_AIRY:  reach = p_range(170, 235); break;
    default:        reach = p_range(215, 256); break;   /* SCH_DEEP, the full walk */
    }

    /*
     * The walk itself: from the dark hue towards the hue of light. Its length
     * is however far those happen to be apart, which is what keeps the ends
     * honest - an ember palette moves 50 degrees from maroon to gold, a dusk
     * palette moves 180 from indigo, and both arrive somewhere warm.
     *
     * Capped at about 180 degrees, which is as far as it can usefully go: from
     * indigo that reaches gold, through blue and teal and green on the way,
     * and every one of those is a colour the same evening actually contains.
     */
    int target = HUE_LIGHT + p_range(-56, 80);      /* amber .. yellow-green */
    if (BAND[band].up) target += 1536;              /* the long way round */
    int travel = ((target - h0) * reach) >> 8;
    if (travel >  780) travel =  780;
    if (travel < -780) travel = -780;

    for (int i = 0; i < STOPS; i++) {
        int h = h0 + ((travel * V_NORM[i]) >> 8);
        int s = S_PROFILE[i];
        int v = V_PROFILE[i];

        switch (fam) {
        case SCH_EARTH:                 /* ground rather than glass */
            s = (s * p_range(105, 150)) >> 8;
            v = 24 + ((v * 210) >> 8);
            break;
        case SCH_AIRY:                  /* mist, chalk, bleached light */
            s = (s * p_range(150, 200)) >> 8;
            v = 58 + ((v * 200) >> 8);
            break;
        case SCH_DEEP:                  /* dusk, deep water, nothing pale */
            v = (v * 185) >> 8;
            break;
        default:
            break;
        }

        /*
         * Magenta, in dust.
         *
         * Full-strength magenta is the one hue with nothing behind it
         * outdoors, which is why it was thrown out of the base hues entirely -
         * but the road through it, violet to rose to ember, is the most
         * ordinary sight there is. The difference between the two is not the
         * hue at all, it is the saturation: an evening gives dusty rose, and
         * the neon version is what a machine picks. So the arc is walked
         * rather than banned, and the closer a stop lands to magenta the more
         * of its saturation it gives up.
         */
        int dm = ((h % 1536) + 1536) % 1536 - 1330;
        if (dm < -768) dm += 1536;
        else if (dm > 768) dm -= 1536;
        if (dm < 0) dm = -dm;
        if (dm < 210) {
            /* A ceiling rather than a scaling, because what has to be true is
             * that no stop in this arc is ever vivid - not that each one is
             * somewhat less vivid than it would have been. */
            int cap = 96 + (dm * 120) / 210;
            if (s > cap) s = cap;
        }

        /* A little movement, so two palettes of one family are not one
         * palette twice. */
        s += p_range(-24, 24);
        v += p_range(-18, 18);

        hsv2rgb(h, s, v, s_stop[i]);
    }
}

/*
 * Where the five sit along the climb. Unevenly, and by construction rather
 * than by luck: the widths are drawn first and then scaled onto the half
 * cycle, so they add up to exactly the climb however they came out.
 *
 * Four segments for five stops - the fifth does not need a run back to the
 * first, because the mirror in interpolate() is the run back.
 */
static void distribute(void)
{
    int w[STOPS - 1], total = 0;

    for (int i = 0; i < STOPS - 1; i++) {
        const uint8_t *a = s_stop[i], *b = s_stop[i + 1];
        int la = (77 * a[0] + 150 * a[1] + 29 * a[2]) >> 8;
        int lb = (77 * b[0] + 150 * b[1] + 29 * b[2]) >> 8;

        /*
         * Weighted towards the light. A dark segment and a bright one given
         * equal room do not read as equal: the dark one is where the picture
         * hides its detail, and a quarter of the cycle spent near black is a
         * quarter spent showing nothing. So the run between two bright stops
         * gets about half again the width of the climb out of shadow - still
         * uneven, still random, but leaning where there is something to see.
         */
        w[i] = (p_range(30, 66) * (150 + ((la + lb) >> 2))) >> 8;
        if (w[i] < 12) w[i] = 12;
        total += w[i];
    }

    int acc = 0;
    for (int i = 0; i < STOPS - 1; i++) {
        s_pos[i] = (uint8_t)((acc * PAL_PEAK) / total);
        acc += w[i];
    }
    s_pos[STOPS - 1] = PAL_PEAK;        /* the light end, at the fold */
}

/*
 * Fill the cycle: the climb into the first half, then the same climb backwards
 * into the second.
 *
 * Mirroring is what makes a five-colour ramp usable as a cyclic palette. The
 * palette has to join up with itself - a fractal walks round it over and over -
 * and there are only two ways to arrange that. Spend a stop on the return, and
 * four fifths of the palette is spent getting somewhere and one fifth undoing
 * it, with the dark end doubled. Or fold it, and every one of the five colours
 * is passed twice per cycle, in reverse the second time, with no seam anywhere
 * because the fold points are the ends of the ramp and the curve is already
 * flat at both.
 *
 * What that costs is that bands come in symmetric pairs, out and back. On a
 * fractal that is not a tell - the bands are already nested - and it is the
 * same fold that a sunset makes on water.
 *
 * Smoothstep across each segment, so the curve reaches every stop with zero
 * slope and there is no crease where two meet, which matters more here than in
 * a gradient on a page: a fractal stretches some segments across a whole screen
 * and squeezes others into three pixels.
 */
static void interpolate(void)
{
    for (int i = 0; i < STOPS - 1; i++) {
        const uint8_t *ca = s_stop[i];
        const uint8_t *cb = s_stop[i + 1];

        int a   = s_pos[i];
        int len = s_pos[i + 1] - a;

        /* Two stops on the same position is a segment with nothing in it. */
        if (len <= 0) continue;

        for (int k = 0; k < len; k++) {
            int t  = (k << 8) / len;                /* 0..255 across the run */
            int tt = (t * t) >> 8;
            int e  = (tt * (768 - 2 * t)) >> 8;     /* smoothstep, 0..256 */
            int p  = (a + k) & 255;

            for (int c = 0; c < 3; c++) {
                int A = ca[c] * ca[c];              /* linear light */
                int B = cb[c] * cb[c];
                int v = A + (((B - A) * e) >> 8);
                if (v < 0) v = 0;
                s_rgb[p][c] = (uint8_t)isqrt32((uint32_t)v);
            }
        }
    }

    /* Each segment fills up to the entry before the next stop, so the last
     * stop of all - the light end, sitting exactly on the fold - is the one
     * entry no segment covers. It has to be written here or it stays whatever
     * the previous palette left there, which shows as a dark line straight
     * down the middle of every band. */
    s_rgb[PAL_PEAK][0] = s_stop[STOPS - 1][0];
    s_rgb[PAL_PEAK][1] = s_stop[STOPS - 1][1];
    s_rgb[PAL_PEAK][2] = s_stop[STOPS - 1][2];

    /* The fold: entry 128 is the light end and entry 0 the dark one, so 255
     * lands next to 1 and the cycle closes on itself. Entry 0 is the interior
     * and is overwritten by commit(); the ramp reaching it from both sides is
     * what makes that seam invisible. */
    for (int p = PAL_PEAK + 1; p < 256; p++) {
        s_rgb[p][0] = s_rgb[256 - p][0];
        s_rgb[p][1] = s_rgb[256 - p][1];
        s_rgb[p][2] = s_rgb[256 - p][2];
    }
}

/*
 * Contrast, brightness, and enough colour to be a colour. Measured over the
 * whole cycle, because none of the three is a property of any one entry, and
 * kept as a net under the generator rather than as its aim: the profiles above
 * mean this rarely fires, and when it does the palette was one the jitter
 * happened to pull somewhere flat.
 */
static int good_enough(void)
{
    int lmin = 999, lmax = -1, lsum = 0, ssum = 0;

    for (int i = 1; i < 256; i++) {
        int r = s_rgb[i][0], g = s_rgb[i][1], b = s_rgb[i][2];
        int l = (77 * r + 150 * g + 29 * b) >> 8;
        int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
        int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if (l < lmin) lmin = l;
        if (l > lmax) lmax = l;
        lsum += l;
        ssum += hi - lo;
    }

    int lavg = lsum / 255, savg = ssum / 255;
    return (lmax - lmin) >= 90 && lavg >= 45 && lavg <= 190 && savg >= 18;
}

/*
 * The interior is not part of the cycle - it is the one colour that has to
 * read as "nothing happens here" - but black beside a warm palette looks like
 * a hole cut in the picture. Taking the darkest colour of the cycle and
 * darkening it further keeps the picture one object: still clearly the floor,
 * still the same family of colour as the walls.
 */
static void commit(void)
{
    int dark = 1, dl = 999;

    for (int i = 1; i < 256; i++) {
        int l = (77 * s_rgb[i][0] + 150 * s_rgb[i][1] + 29 * s_rgb[i][2]) >> 8;
        if (l < dl) { dl = l; dark = i; }
        g_pal[i] = GB_RGB(s_rgb[i][0], s_rgb[i][1], s_rgb[i][2]);
    }

    int r = (s_rgb[dark][0] * 140) >> 8;
    int g = (s_rgb[dark][1] * 140) >> 8;
    int b = (s_rgb[dark][2] * 140) >> 8;
    g_pal[0] = GB_RGB(r, g, b);
}

void pal_new(uint32_t seed)
{
    if (!seed) seed = rnd() | 1u;
    s_seed  = seed;
    s_state = seed;

    for (int try = 0; try < 12; try++) {
        scheme();
        distribute();
        interpolate();
        if (good_enough()) break;
    }
    commit();
}

uint32_t pal_seed(void) { return s_seed; }

/*
 * Fit the cycle to the scene. vmax is the value a pixel reaches at the far end
 * of what this scene produces - maxiter for an escape count, an exponent of -2
 * for Lyapunov - and the scene says how many times round the palette that
 * should be. Everything past vmax keeps going round, which is what should
 * happen: the values past the end of the scale are the deep water beside the
 * boundary, and they are supposed to stripe.
 */
void pal_map(const scene_t *s)
{
    int32_t vmax = s->vmax > 0 ? s->vmax : 1;
    uint32_t cyc = s->cycles ? s->cycles : 1;

    /* pal_shade takes the root of the value, so the scale is fitted to the
     * root of the range rather than to the range. */
    uint32_t top = isqrt32((uint32_t)vmax);
    if (top < 1) top = 1;
    g_pal_k = (cyc << 24) / top;
    if (g_pal_k == 0) g_pal_k = 1;
    g_pal_phase = s->phase;
}
