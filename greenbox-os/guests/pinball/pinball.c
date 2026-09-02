/*
 * pinball.c - PINBALL, a table for greenbox with Gumball and Darwin on the
 * backglass.
 *
 * Three balls, one plunger, two flippers, and a score that goes on the board
 * at the top of the panel where a real one keeps it.
 *
 * Controls:
 *   R held 1s    pull the plunger back and launch - the longer the hold, the
 *                harder the shot, and a full second is full power
 *   L held       left flipper, for as long as it is down
 *   R held       right flipper, once the ball is in play
 *   L tap        (attract and game over) back out to the menu screen
 *   L held 3s    the OS escape gesture
 *
 * The flippers read api->buttons() rather than the event queue, because an
 * event cannot say when a hold ends and a flipper that will not drop again is
 * not a flipper. The plunger is the one control that wants both: the live
 * state to charge while the button is down, and GB_EV_R_LONG so that a hold
 * past a second fires at full power whether or not the player lets go.
 *
 * The left button at three seconds is the OS escape and arrives with the kill,
 * which is a real cost here in a way it is not in astro: trapping a ball on a
 * raised left flipper is an ordinary pinball move, and holding it that long
 * ends the game. Nothing in a guest can decline that gesture, so the table
 * says so instead - the backglass puts up RELEASE L at 1.8 s, before the OS
 * paints its own quit bar over the flippers at 2 s.
 *
 * ------------------------------------------------------------------ display
 *
 * Portrait, and not as a preference: a playfield is taller than it is wide,
 * and there is no version of this that works in 135 rows of height. The OS
 * restores the system orientation when the guest exits.
 *
 * The top 16 rows are the backglass - score, ball number, multiplier - and the
 * table is laid out in the 224 rows underneath. Every frame is a full repaint
 * through the band buffer in pin_gfx.c: there is no framebuffer to spare and
 * the lamps, the flippers and the ball all move, so there would be little left
 * static to keep anyway.
 *
 * ------------------------------------------------------------------ physics
 *
 * The ball is a circle with a position and a velocity in Q8 fixed point, and
 * the table is a list of line segments, a few circles and two flippers. Each
 * frame is split into substeps small enough that the ball never moves more
 * than two pixels at a time, which is what keeps a 700 px/s shot from passing
 * straight through a one-pixel wall between frames.
 *
 * Contact is resolved the same way for everything: find the closest point on
 * the surface, and if it is nearer than the ball's radius, push the ball out
 * along the normal and reflect the velocity through it with a restitution that
 * depends on what was hit - a rail keeps a third of the approach speed and a
 * rubber two thirds, while a pop bumper adds a kick of its own on top.
 *
 * The flippers are the only surfaces that move, and they matter precisely
 * because they move: a flipper reflects the ball's velocity RELATIVE to the
 * surface, so the same contact does nothing on a resting flipper and throws
 * the ball up the table on a swinging one. The surface velocity at the contact
 * point is the angular speed times the distance from the pivot, which is why
 * catching a ball on the tip sends it further than catching it at the base -
 * that is not a special case here, it falls out of the arithmetic.
 */

#include "pinball.h"

/* ================================================================ tuning */

#define FRAME_MS      30
#define TOP_H         16        /* backglass rows, 0..15 */

#define BALL_R         3
#define GRAVITY      560        /* px/s^2 down the table */
#define V_MAX        780        /* px/s - two pixels per substep at 16 steps */

#define DRAIN_Y      226        /* below the flippers, ball is gone */

/*
 * A plunge has to carry the ball 190 px up the lane and still have something
 * left at the top, because what waits there is the arch: the ball is thrown
 * left along it and falls into the table from above. Under PLUNGE_MIN it
 * stalls at the divider and drops back into the lane, which is what a real
 * table does and reads on a 135-pixel screen as the game being broken - so the
 * weakest plunge here is one that still gets round, and the charge decides how
 * far along the arch the ball is carried rather than whether it gets there.
 */
#define PLUNGE_MIN   560        /* px/s at a tap */
#define PLUNGE_MAX   720        /* px/s at a full second */
#define CHARGE_MS    900        /* to wind the plunger fully back */

#define SAVE_MS     6000        /* ball save after a launch */
#define QUIT_WARN_MS 1800       /* when to mention the 3 s left-hold */

#define E_RAIL        90        /* restitution, Q8 */
#define E_RUBBER     170
#define E_TARGET     140
#define E_FLIP       130
#define BUMP_KICK    290

/*
 * Tangential friction, Q8, applied to the ball's speed ALONG a surface.
 *
 * A rail gets almost none - a ball running down one should keep its speed.
 * A flipper gets a lot, because flipper rubber is the grippiest thing on a
 * pinball table and two things depend on it. A ball resting on a flipper rolls
 * down towards the tip; without grip it accelerates off the end before anyone
 * can react, which is exactly what "it fell off before I pressed" is. And a
 * ball caught right at the tip is scooped up by the rubber sweeping past it -
 * without grip the only impulse available is the one along the contact normal,
 * which for a ball sitting beside the round end of the bar points sideways, so
 * the hardest shot on the table came out as a shove towards the drain.
 */
#define MU_RAIL        8
#define MU_FLIP       58
/*
 * A flipper standing still grips much less than one that is sweeping, which is
 * not how rubber works but is how this table has to model it. Friction against
 * a static surface mostly goes into spin, and spin is the one thing a ball
 * here does not have: it would come back out of the next contact as a curve,
 * and since there is nowhere to keep it, all a grippy static flipper does is
 * eat speed the player earned. While the bar is moving there is no such
 * problem - the tangential impulse is the shot.
 */
#define MU_FLIP_IDLE  48

#define FLIP_LEN      25
#define FLIP_R         4        /* drawn, and collided against, at this radius */
/*
 * The rest angle is shallower than a real table's, and deliberately. A real
 * playfield is tilted about six degrees, so the gravity a ball feels along a
 * flipper is a tenth of what it feels falling; here the table IS the screen
 * and gravity is all of it, so at a real flipper's slope the ball tobogganed
 * off the tip in a quarter of a second. Flat enough to hold a ball for a
 * moment is what makes rolling down to the tip a shot rather than a race.
 */
#define FLIP_REST     17        /* angle, 256 units to the turn */
#define FLIP_UP      (-19)
#define FLIP_UP_RATE 900        /* units per second going up */
#define FLIP_DN_RATE 520        /* and coming back down */

/* ============================================================== the table */
/*
 * All of it is in screen pixels, laid out for 135x240 with the backglass
 * taking the top 16 rows. The playfield proper is x 4..113; x 116..130 is the
 * plunger lane, and the two are separated by the divider at x=113 that stops
 * short at y=52 so that a launched ball crosses over the top of it and comes
 * down the arch into the table.
 *
 * The boundary below is a closed loop apart from the drain between the flipper
 * tips, which is the entire point of it. If a segment is ever moved, the thing
 * to preserve is that closure: a gap of even one pixel is a ball that leaves
 * the table and never comes back.
 */

enum { K_RAIL = 0, K_RUBBER };

typedef struct {
    int16_t x0, y0, x1, y1;
    uint8_t kind;
} seg_t;

static const seg_t WALL[] = {
    /* left rail, then the arch over the top, clockwise */
    {   4, 178,   4,  70, K_RAIL },
    {   4,  70,   8,  48, K_RAIL },
    {   8,  48,  18,  32, K_RAIL },
    {  18,  32,  34,  23, K_RAIL },
    {  34,  23,  56,  20, K_RAIL },
    {  56,  20,  80,  22, K_RAIL },
    {  80,  22, 100,  30, K_RAIL },
    { 100,  30, 114,  42, K_RAIL },
    { 114,  42, 124,  56, K_RAIL },
    { 124,  56, 130,  72, K_RAIL },
    /* the plunger lane: outer wall, floor, and the divider back up */
    { 130,  72, 130, 236, K_RAIL },
    { 130, 236, 113, 236, K_RAIL },
    { 113, 236, 113,  52, K_RAIL },
    /* the divider tip is bent left, so a ball leaving the lane is thrown into
     * the arch instead of dropping straight back down the way it came */
    { 113,  52, 105,  44, K_RUBBER },
    /* the guides that funnel the lower table onto the flipper pivots */
    {   4, 178,  26, 201, K_RAIL },
    { 113, 178,  90, 201, K_RAIL },
    /* The trough walls, from each pivot down past the bottom of the panel.
     * Everything above closes the table except the drain between the flipper
     * tips - but a flipper is not a wall, and a ball with enough sideways
     * speed can be carried along one and straight past its pivot. Without
     * these it leaves through the side of the table on its way to being
     * counted as drained. */
    {  26, 201,  20, 244, K_RAIL },
    {  90, 201,  96, 244, K_RAIL },
};
#define NWALL ((int)(sizeof WALL / sizeof WALL[0]))

/* Pop bumpers and the two plain posts, all of them circles. */
typedef struct { int16_t x, y, r; uint8_t pop; } circ_t;

static const circ_t CIRC[] = {
    { 36,  86, 10, 1 },
    { 81,  86, 10, 1 },
    { 58, 116, 10, 1 },
    { 20, 128,  4, 0 },
    { 97, 128,  4, 0 },
    { 44,  44,  3, 0 },     /* the posts that make the top lanes lanes */
    { 72,  44,  3, 0 },
};
#define NCIRC ((int)(sizeof CIRC / sizeof CIRC[0]))
#define NBUMP 3

/* Drop targets: a bank of three across the middle, with gaps wide enough for
 * a ball to pass between them while they are still up. */
typedef struct { int16_t x, y, w, h; } rect_t;

static const rect_t DROP[3] = {
    { 30, 146, 11, 7 },
    { 52, 146, 11, 7 },
    { 74, 146, 11, 7 },
};

/* Top lane rollovers. Not physical - a lane is a trigger the ball rolls over
 * on its way down the arch, and giving it walls would only give the ball
 * somewhere to wedge. */
static const int16_t LANE[3][2] = { { 30, 47 }, { 58, 43 }, { 86, 47 } };
#define LANE_TRIG 8

/*
 * The drain mouth is the distance between the flipper tips, and what has to
 * fit through it is not the ball but the ball plus the bar's own radius on
 * each side - 14 px, not 7. At the pivots this table started with, the tips
 * were 11 px apart and a ball that came down the middle sat on both of them
 * and stayed there until the stuck-ball nudge shoved it off. Six pixels of
 * clearance is enough to fall through and still narrow enough that going down
 * the middle is a mistake rather than a coin toss.
 */
#define PIVOT_LX  26
#define PIVOT_RX  90
#define PIVOT_Y  201

#define LANE_X   123            /* plunger lane centre */
#define LANE_TOP  40

/* ================================================================= state */

typedef enum { ST_ATTRACT = 0, ST_READY, ST_PLAY, ST_DRAIN, ST_OVER } state_t;

typedef struct {
    uint16_t magic;
    uint16_t pad;
    uint32_t best;
} save_t;

#define SAVE_MAGIC 0x9B11u

static save_t   s_cfg = { SAVE_MAGIC, 0, 0 };
static state_t  s_state;

/* The ball. Position and velocity in Q8: pixels and pixels per second. */
static int32_t  s_bx, s_by, s_vx, s_vy;

static int      s_who;          /* which of the cast is on this ball */
static uint32_t s_score;
static uint32_t s_bonus;        /* paid out, times the multiplier, on a drain */
static int      s_mult;
static int      s_ball;         /* 1..3 */
static int      s_charge;       /* plunger, 0..255 */
static int32_t  s_save_ms;      /* ball save left */
static int32_t  s_drain_ms;
static int32_t  s_still_ms;     /* how long the ball has been going nowhere */
static int      s_hold_l;       /* ms the left button has been down */

static int      s_flip_a[2];    /* live angle, Q8, 256 units to the turn */
static int      s_flip_w[2];    /* and its speed, units per second */
static uint8_t  s_flip_on[2];

static uint8_t  s_lane_lit[3];
static uint8_t  s_drop_down[3];
static int32_t  s_drop_reset;   /* ms until a completed bank pops back up */

static int16_t  s_lamp[NCIRC];  /* flash timers, ms */
static int16_t  s_lane_fl[3];
static int16_t  s_drop_fl[3];

static char     s_msg[22];
static int32_t  s_msg_ms;
static uint32_t s_anim;         /* attract-mode pulse */

/* ================================================================== util */

static void msg(const char *s)
{
    int i = 0;
    for (; s[i] && i < (int)sizeof s_msg - 1; i++) s_msg[i] = s[i];
    s_msg[i] = 0;
    s_msg_ms = 1500;
}

static void msg_num(const char *s, uint32_t v)
{
    A->snprintf(s_msg, sizeof s_msg, "%s %u", s, (unsigned)v);
    s_msg_ms = 1500;
}

static void add_score(uint32_t v)
{
    s_score += v * (uint32_t)s_mult;
}

static void save_cfg(void)
{
    A->store_put("cfg", &s_cfg, sizeof s_cfg);
}

/* True if anything between these two rows can possibly land in this band. Most
 * of the table is outside any given band, and the cheapest way to draw a wall
 * fifteen times is to not draw it fourteen of them. */
static int in_band(int y0, int y1)
{
    return !(y1 < g_band_y0 || y0 > g_band_y0 + g_band_h - 1);
}

/* ============================================================== flippers */

static int flip_tip_x(int i)
{
    int c = (FLIP_LEN * icos(s_flip_a[i] >> 8)) >> 8;
    return i == 0 ? PIVOT_LX + c : PIVOT_RX - c;
}

static int flip_tip_y(int i)
{
    return PIVOT_Y + ((FLIP_LEN * isin(s_flip_a[i] >> 8)) >> 8);
}

/*
 * Advance both flippers by `us` MICROseconds, with the angle held in Q8 units
 * of a 256th of a turn.
 *
 * Both of those are here because of what the ball does while a flipper is
 * moving. A swing is 40 units in about 45 ms, so at one frame per update the
 * bar crossed 27 of them in a single step - most of its travel - and a ball
 * resting on it was simply on the other side by the time anything was tested.
 * The contact then resolved the only way it could: push the ball out of the
 * bar along the shortest way, which for a ball now underneath is straight
 * down, into the drain. Cradling the ball and flipping it therefore did the
 * exact opposite of what a flipper is for.
 *
 * So the sweep is stepped with the ball instead, a couple of hundred
 * microseconds at a time, and at that resolution whole angle units round to
 * nothing - hence Q8.
 */
static void flippers_advance(uint32_t us)
{
    for (int i = 0; i < 2; i++) {
        int target = (s_flip_on[i] ? FLIP_UP : FLIP_REST) << 8;
        int rate   = s_flip_on[i] ? FLIP_UP_RATE : FLIP_DN_RATE;
        int step   = (int)(((uint32_t)rate * us) / 3906u);  /* 1e6/256 */
        int a      = s_flip_a[i];

        if (a < target) {
            a += step;
            if (a > target) a = target;
            s_flip_w[i] = rate;
        } else if (a > target) {
            a -= step;
            if (a < target) a = target;
            s_flip_w[i] = -rate;
        } else {
            s_flip_w[i] = 0;
        }
        s_flip_a[i] = a;
    }
}

/*
 * The speed of the flipper tip over the frame about to be simulated, px/s -
 * the fastest thing on the table, and what the substep count has to be sized
 * against when the ball itself is sitting still.
 *
 * It is read off where each flipper is GOING, not off how fast it was moving
 * last frame. The frame that matters most is the one where a button has just
 * gone down: the bar is still stationary at that instant, so asking what it
 * has been doing answers zero and buys the sweep a single substep to cross
 * the ball in - which is the whole bug this exists to prevent.
 */
static int flip_tip_speed(void)
{
    int best = 0;
    for (int i = 0; i < 2; i++) {
        int target = (s_flip_on[i] ? FLIP_UP : FLIP_REST) << 8;
        if (s_flip_a[i] == target) continue;
        int rate = s_flip_on[i] ? FLIP_UP_RATE : FLIP_DN_RATE;
        int v = (FLIP_LEN * rate * 256) / 10430;
        if (v > best) best = v;
    }
    return best;
}

/* =============================================================== physics */
/*
 * One contact, resolved. `rq8` is the sum of the ball's radius and whatever
 * radius the surface has; `svx`/`svy` are the surface's own velocity at the
 * contact point, which is zero for everything except a moving flipper.
 *
 * Returns 1 if it touched, so the caller can score it.
 */
static int hit_seg(int x0, int y0, int x1, int y1, int rq8, int e, int mu,
                   int32_t svx, int32_t svy, int kick)
{
    /* Cheap reject first: at sixteen substeps a frame this runs a few hundred
     * times, and the arithmetic below is only worth doing for the one or two
     * surfaces the ball is anywhere near. */
    {
        int bxp = s_bx >> 8, byp = s_by >> 8, m = (rq8 >> 8) + 2;
        int lo, hi;
        lo = x0 < x1 ? x0 : x1; hi = x0 < x1 ? x1 : x0;
        if (bxp < lo - m || bxp > hi + m) return 0;
        lo = y0 < y1 ? y0 : y1; hi = y0 < y1 ? y1 : y0;
        if (byp < lo - m || byp > hi + m) return 0;
    }

    int ex = x1 - x0, ey = y1 - y0;
    int len2 = ex * ex + ey * ey;
    int32_t rx = s_bx - (x0 << 8), ry = s_by - (y0 << 8);

    int32_t t = 0;
    if (len2 > 0) {
        /* rx>>4 keeps the product inside 32 bits; the sixteenth of a pixel it
         * costs is well under the resolution anything here is drawn at. */
        int32_t dot = ((rx >> 4) * ex + (ry >> 4) * ey);
        t = (dot << 4) / len2;
        if (t < 0) t = 0; else if (t > 256) t = 256;
    }

    int32_t dx = s_bx - ((x0 << 8) + ex * t);
    int32_t dy = s_by - ((y0 << 8) + ey * t);
    int32_t d2 = (dx >> 4) * (dx >> 4) + (dy >> 4) * (dy >> 4);   /* Q8 */
    if (d2 >= ((int32_t)rq8 * rq8) >> 8) return 0;

    int32_t dist = (int32_t)isqrt32((uint32_t)d2 << 8);           /* Q8 */
    int32_t nx, ny;
    if (dist < 16) {
        /* Dead centre on the surface: no direction to push out along, so use
         * the segment's own normal, pointed at wherever the ball came from. */
        int32_t l = (int32_t)isqrt32((uint32_t)len2);
        if (l < 1) return 0;
        nx = (-ey << 8) / l;
        ny = ( ex << 8) / l;
        /* Relative to the surface, not to the table: a ball sitting still on
         * a flipper that is sweeping up is, in the frame of the bar, moving
         * down through it, and the normal has to come out on the side the
         * ball is being pushed towards rather than the side it started on. */
        if (((s_vx - svx) >> 8) * nx + ((s_vy - svy) >> 8) * ny > 0)
            { nx = -nx; ny = -ny; }
        dist = 0;
    } else {
        nx = (dx << 8) / dist;
        ny = (dy << 8) / dist;
    }

    /* Out of the surface first, so the next substep starts clean. */
    int32_t pen = rq8 - dist;
    s_bx += (nx * pen) >> 8;
    s_by += (ny * pen) >> 8;

    int32_t vn = (((s_vx - svx) >> 8) * nx + ((s_vy - svy) >> 8) * ny) >> 8;
    if (vn < 0) {
        int32_t j = (-vn * (256 + e)) >> 8;
        s_vx += j * nx;
        s_vy += j * ny;
    }

    /*
     * Friction, against the surface rather than against the table: what the
     * rubber resists is the ball sliding across it, and on a swinging flipper
     * that difference is the shot. It is applied whether or not the ball was
     * closing, because a ball lying on a flipper is not closing on anything
     * and is precisely the case that needs grip.
     */
    if (mu) {
        int32_t tx = -ny, ty = nx;
        int32_t vt = (((s_vx - svx) >> 8) * tx + ((s_vy - svy) >> 8) * ty) >> 8;
        int32_t dv = (vt * mu) >> 8;
        s_vx -= dv * tx;
        s_vy -= dv * ty;
    }
    if (kick) {
        s_vx += kick * nx;
        s_vy += kick * ny;
    }
    return 1;
}

static int hit_circ(int cx, int cy, int r, int e, int kick)
{
    return hit_seg(cx, cy, cx, cy, ((r + BALL_R) << 8), e, MU_RAIL, 0, 0, kick);
}

static void bump_lamp(int i, int ms)
{
    s_lamp[i] = (int16_t)ms;
}

/* One flipper, as a moving capsule. */
static void hit_flipper(int i)
{
    int px = i == 0 ? PIVOT_LX : PIVOT_RX;
    int tx = flip_tip_x(i), ty = flip_tip_y(i);
    int rq8 = (BALL_R + FLIP_R) << 8;

    /* Where along the bar the ball is, which is how far from the pivot the
     * surface is moving - the tip carries three times the speed of the base
     * and hits three times as hard for it. */
    int dxp = (s_bx >> 8) - px, dyp = (s_by >> 8) - PIVOT_Y;
    int d = (int)isqrt32((uint32_t)(dxp * dxp + dyp * dyp));
    if (d > FLIP_LEN) d = FLIP_LEN;

    int w = s_flip_w[i];
    int32_t svx = 0, svy = 0;
    if (w) {
        /* v = w x r, with the turn measured in 256ths and w in units/s:
         * one unit per second is 2*pi/256 rad/s, hence the 10430. */
        int s = (i == 0) ? 1 : -1;
        svx = (int32_t)(-s * isin(s_flip_a[i] >> 8) * d * w) / 10430;
        svy = (int32_t)( icos(s_flip_a[i] >> 8) * d * w) / 10430;
        svx <<= 8;
        svy <<= 8;
    }
    hit_seg(px, PIVOT_Y, tx, ty, rq8, E_FLIP, w ? MU_FLIP : MU_FLIP_IDLE,
            svx, svy, 0);
}

/* ================================================================ events */

static void lane_check(void)
{
    for (int i = 0; i < 3; i++) {
        if (s_lane_lit[i]) continue;
        int dx = (s_bx >> 8) - LANE[i][0], dy = (s_by >> 8) - LANE[i][1];
        if (dx * dx + dy * dy > LANE_TRIG * LANE_TRIG) continue;

        s_lane_lit[i] = 1;
        s_lane_fl[i]  = 400;
        add_score(250);
        s_bonus += 100;

        if (s_lane_lit[0] && s_lane_lit[1] && s_lane_lit[2]) {
            add_score(2500);
            s_bonus += 500;
            if (s_mult < 5) s_mult++;
            s_lane_lit[0] = s_lane_lit[1] = s_lane_lit[2] = 0;
            msg_num("LANES  X", (uint32_t)s_mult);
        } else {
            const cast_t *c = cast_get(s_who);
            char b[4] = { c->lane[i], 0, 0, 0 };
            msg_num(b, 250u * (uint32_t)s_mult);
        }
    }
}

static void drop_check(void)
{
    for (int i = 0; i < 3; i++) {
        if (s_drop_down[i]) continue;
        const rect_t *r = &DROP[i];

        /* Four edges rather than one rectangle test, so that a ball arriving
         * along the bank hits the side of a target the same way it hits the
         * face of one. */
        int hit = 0;
        hit |= hit_seg(r->x, r->y, r->x + r->w, r->y,
                       BALL_R << 8, E_TARGET, MU_RAIL, 0, 0, 0);
        hit |= hit_seg(r->x, r->y + r->h, r->x + r->w, r->y + r->h,
                       BALL_R << 8, E_TARGET, MU_RAIL, 0, 0, 0);
        hit |= hit_seg(r->x, r->y, r->x, r->y + r->h,
                       BALL_R << 8, E_TARGET, MU_RAIL, 0, 0, 0);
        hit |= hit_seg(r->x + r->w, r->y, r->x + r->w, r->y + r->h,
                       BALL_R << 8, E_TARGET, MU_RAIL, 0, 0, 0);
        if (!hit) continue;

        /* The target gives the ball its bounce and drops on the same frame,
         * which is what a drop target does: the ball comes off it, and the
         * gap it leaves is open from the next substep on. */
        s_drop_down[i] = 1;
        s_drop_fl[i]   = 350;
        add_score(500);
        s_bonus += 200;

        if (s_drop_down[0] && s_drop_down[1] && s_drop_down[2]) {
            add_score(5000);
            s_bonus += 1000;
            s_drop_reset = 1400;
            msg("BANK  5000");
        } else {
            msg_num("TARGET", 500u * (uint32_t)s_mult);
        }
    }
}

static void collide(void)
{
    for (int i = 0; i < NWALL; i++) {
        const seg_t *s = &WALL[i];
        hit_seg(s->x0, s->y0, s->x1, s->y1, BALL_R << 8,
                s->kind == K_RUBBER ? E_RUBBER : E_RAIL, MU_RAIL, 0, 0, 0);
    }

    for (int i = 0; i < NCIRC; i++) {
        const circ_t *c = &CIRC[i];
        if (!hit_circ(c->x, c->y, c->r, E_RUBBER, c->pop ? BUMP_KICK : 0))
            continue;
        bump_lamp(i, c->pop ? 260 : 120);
        if (c->pop) {
            add_score(100);
            s_bonus += 50;
        }
    }

    hit_flipper(0);
    hit_flipper(1);

    if (!s_drop_reset) drop_check();
    lane_check();
}

/* One frame of ball movement, split fine enough that nothing is ever moved
 * more than about two pixels between collision tests. */
static void ball_step(uint32_t dt)
{
    s_vy += (int32_t)((GRAVITY * (int32_t)dt) << 8) / 1000;

    int sp = (s_vx >> 8), sq = (s_vy >> 8);
    if (sp < 0) sp = -sp;
    if (sq < 0) sq = -sq;
    int speed = sp > sq ? sp : sq;

    /* A swinging flipper counts as speed even when the ball has none: the
     * thing that must not move more than about two pixels between tests is
     * whichever of the two is faster, and a tip does 570 px/s. */
    int tip = flip_tip_speed();
    if (tip > speed) speed = tip;

    int steps = 1 + (int)((speed * (int32_t)dt) / 2000);
    if (steps > 16) steps = 16;

    /*
     * Everything stays in 32 bits on purpose. A guest is linked -nostdlib
     * against libgcc only, so a 64-bit divide is a call to __divdi3 that has
     * to be relocated into the image - payable, but not for arithmetic that
     * fits: v is at most 780 px/s in Q8, dt is clamped to 60 ms, and the
     * product of the two is well inside a signed 32-bit word.
     */
    int32_t den = 1000 * steps;
    int32_t ax  = (s_vx * (int32_t)dt) / den;
    int32_t ay  = (s_vy * (int32_t)dt) / den;
    uint32_t sub_us = (dt * 1000u) / (uint32_t)steps;
    for (int i = 0; i < steps; i++) {
        flippers_advance(sub_us);
        s_bx += ax;
        s_by += ay;
        collide();
        /* Bounced: the rest of the frame goes wherever the bounce points. */
        ax = (s_vx * (int32_t)dt) / den;
        ay = (s_vy * (int32_t)dt) / den;
    }

    /* Table friction, and a ceiling on the speed so that sixteen substeps is
     * always enough to keep the ball inside the walls. */
    s_vx -= s_vx / 256;
    s_vy -= s_vy / 256;

    int32_t vx = s_vx < 0 ? -s_vx : s_vx, vy = s_vy < 0 ? -s_vy : s_vy;
    int32_t v  = vx > vy ? vx : vy;
    if (v > (V_MAX << 8)) {
        int32_t f = ((int32_t)V_MAX << 16) / v;     /* Q8, under 1.0 */
        s_vx = (s_vx >> 4) * f >> 4;
        s_vy = (s_vy >> 4) * f >> 4;
    }
}

/* ================================================================== game */

static void reset_table(void)
{
    for (int i = 0; i < 3; i++) {
        s_lane_lit[i] = 0;
        s_drop_down[i] = 0;
        s_lane_fl[i] = s_drop_fl[i] = 0;
    }
    for (int i = 0; i < NCIRC; i++) s_lamp[i] = 0;
    s_drop_reset = 0;
}

static void park_ball(void)
{
    s_bx = (int32_t)LANE_X << 8;
    s_by = (int32_t)(236 - BALL_R) << 8;
    s_vx = s_vy = 0;
    s_charge = 0;
    s_still_ms = 0;
}

static void new_ball(void)
{
    /* Which of the two is on the backglass is decided here, per ball, which is
     * the whole reason it is decided anywhere: a game is normally one of each
     * rather than one of them three times. */
    s_who = (int)(rnd() & 1u);
    s_mult = 1;
    s_bonus = 0;
    reset_table();
    park_ball();
    s_state = ST_READY;
}

static void new_game(void)
{
    s_score = 0;
    s_ball  = 1;
    new_ball();
}

static void launch(void)
{
    int p = s_charge;
    if (p < 0) p = 0; else if (p > 255) p = 255;
    int v = PLUNGE_MIN + ((PLUNGE_MAX - PLUNGE_MIN) * p) / 255;

    s_vy = -((int32_t)v << 8);
    s_vx = (int32_t)rnd_range(-20, 20) << 8;
    s_charge   = 0;
    s_save_ms  = SAVE_MS;
    s_still_ms = 0;
    s_state    = ST_PLAY;
    msg("PLAY!");
}

static void drain(void)
{
    if (s_save_ms > 0) {
        park_ball();
        s_state = ST_READY;
        msg("BALL SAVED");
        return;
    }

    uint32_t bonus = s_bonus * (uint32_t)s_mult;
    s_score += bonus;
    s_bonus  = bonus;           /* what the drain screen shows */
    s_drain_ms = 1600;
    s_state = ST_DRAIN;
}

static void end_ball(void)
{
    if (s_ball >= 3) {
        if (s_score > s_cfg.best) {
            s_cfg.best = s_score;
            save_cfg();
        }
        s_state = ST_OVER;
        return;
    }
    s_ball++;
    new_ball();
}

/* ================================================================= input */

/* Returns 0 when the player asked to leave. */
static int handle_input(void)
{
    gb_event_t e;
    while ((e = A->poll_event()) != GB_EV_NONE) {
        if (e == GB_EV_L_LONG) return 0;

        switch (s_state) {
        case ST_ATTRACT:
            if (e == GB_EV_R_SHORT || e == GB_EV_R_LONG) new_game();
            break;

        case ST_READY:
            /* The gesture the table is built around: a second on the right
             * button is a full-power plunge, and it fires here rather than
             * waiting for the release so that holding it does what it looks
             * like it does. A shorter tap launches on the release instead,
             * with whatever the plunger had wound up by then. */
            if (e == GB_EV_R_LONG) {
                s_charge = 255;
                launch();
            }
            break;

        case ST_OVER:
            if (e == GB_EV_R_SHORT || e == GB_EV_R_LONG) new_game();
            else if (e == GB_EV_L_SHORT) s_state = ST_ATTRACT;
            break;

        default:
            break;
        }
    }
    return 1;
}

static void read_buttons(uint32_t dt)
{
    uint8_t b = A->buttons();

    s_hold_l = (b & GB_BTN_L) ? s_hold_l + (int)dt : 0;

    if (s_state == ST_PLAY || s_state == ST_DRAIN) {
        s_flip_on[0] = (b & GB_BTN_L) ? 1 : 0;
        s_flip_on[1] = (b & GB_BTN_R) ? 1 : 0;
    } else {
        s_flip_on[0] = s_flip_on[1] = 0;
    }

    if (s_state == ST_READY) {
        if (b & GB_BTN_R) {
            s_charge += (int)((255 * dt) / CHARGE_MS) + 1;
            if (s_charge > 255) s_charge = 255;
        } else if (s_charge > 0) {
            launch();
        }
    }
}

/* ================================================================ update */

static void tick_timers(uint32_t dt)
{
    for (int i = 0; i < NCIRC; i++)
        if (s_lamp[i] > 0) s_lamp[i] = (int16_t)(s_lamp[i] > (int)dt
                                                 ? s_lamp[i] - (int)dt : 0);
    for (int i = 0; i < 3; i++) {
        if (s_lane_fl[i] > 0)
            s_lane_fl[i] = (int16_t)(s_lane_fl[i] > (int)dt ? s_lane_fl[i] - (int)dt : 0);
        if (s_drop_fl[i] > 0)
            s_drop_fl[i] = (int16_t)(s_drop_fl[i] > (int)dt ? s_drop_fl[i] - (int)dt : 0);
    }
    if (s_msg_ms > 0)  s_msg_ms  -= (int32_t)dt;
    if (s_save_ms > 0) s_save_ms -= (int32_t)dt;

    if (s_drop_reset > 0) {
        s_drop_reset -= (int32_t)dt;
        if (s_drop_reset <= 0) {
            s_drop_reset = 0;
            s_drop_down[0] = s_drop_down[1] = s_drop_down[2] = 0;
        }
    }
}

static void update(uint32_t dt)
{
    s_anim += dt;
    tick_timers(dt);
    /* In play the sweep is advanced inside ball_step, in step with the ball.
     * Everywhere else nothing is watching it but the eye. */
    if (s_state != ST_PLAY) flippers_advance(dt * 1000u);

    switch (s_state) {
    case ST_ATTRACT:
        /* Take turns on the attract screen, a couple of seconds each. */
        s_who = (int)((s_anim / 2500) & 1u);
        break;

    case ST_READY:
        /* The ball sits on the plunger and the plunger winds back under it, so
         * that a wound-up shot looks like a wound-up shot. */
        s_by = (int32_t)(236 - BALL_R + (s_charge * 4) / 255) << 8;
        break;

    case ST_PLAY: {
        ball_step(dt);

        int bx = s_bx >> 8, by = s_by >> 8;

        /* Back down the plunger lane with nothing left: not a drain, a second
         * plunge. Real tables do this and it saves the one dead end this
         * layout has - a ball parked in the lane with no way out. */
        if (bx > 113 && by > 210 && (s_vy >> 8) < 40 && (s_vy >> 8) > -40) {
            park_ball();
            s_state = ST_READY;
            break;
        }

        if (by > DRAIN_Y && bx < 113) {
            drain();
            break;
        }

        /* Wedged somewhere the geometry did not anticipate. Rather than leave
         * the player looking at a stationary ball, shove it. */
        int32_t vx = s_vx < 0 ? -s_vx : s_vx, vy = s_vy < 0 ? -s_vy : s_vy;
        if (vx + vy < (14 << 8)) {
            s_still_ms += (int32_t)dt;
            if (s_still_ms > 2200) {
                s_vx += (int32_t)rnd_range(-90, 90) << 8;
                s_vy -= (int32_t)rnd_range(40, 130) << 8;
                s_still_ms = 0;
                msg("NUDGE");
            }
        } else {
            s_still_ms = 0;
        }
        break;
    }

    case ST_DRAIN:
        s_drain_ms -= (int32_t)dt;
        if (s_drain_ms <= 0) end_ball();
        break;

    default:
        break;
    }
}

/* ================================================================ drawing */

static void draw_felt(const cast_t *c)
{
    int y1 = g_band_y0 + g_band_h;
    for (int y = g_band_y0; y < y1; y++) {
        if (y < TOP_H) continue;
        int t = ((y - TOP_H) * 256) / (g_h - TOP_H);
        fb_row(y, mix565(c->felt_top, c->felt_bot, t));
    }

    /* The plunger lane is a shade off the playfield, so that the divider reads
     * as a divider rather than as a line drawn on open felt. */
    if (in_band(LANE_TOP, 239))
        fb_box(116, LANE_TOP, 15, 240 - LANE_TOP,
               mix565(c->felt_bot, GB_BLACK, 90));
}

static void draw_walls(const cast_t *c)
{
    for (int i = 0; i < NWALL; i++) {
        const seg_t *s = &WALL[i];
        int ymin = s->y0 < s->y1 ? s->y0 : s->y1;
        int ymax = s->y0 < s->y1 ? s->y1 : s->y0;
        if (!in_band(ymin - 1, ymax + 1)) continue;

        uint16_t col = s->kind == K_RUBBER ? mix565(c->glow, GB_WHITE, 60)
                                           : c->trim;

        fb_line(s->x0, s->y0, s->x1, s->y1, col);
        fb_line(s->x0 + 1, s->y0, s->x1 + 1, s->y1, col);
        fb_line(s->x0, s->y0 - 1, s->x1, s->y1 - 1, mix565(col, GB_WHITE, 70));
    }
}

static void draw_bumpers(const cast_t *c)
{
    const char *initial = cast_get(s_who)->name;   /* G or D */

    for (int i = 0; i < NCIRC; i++) {
        const circ_t *b = &CIRC[i];
        if (!in_band(b->y - b->r - 1, b->y + b->r + 1)) continue;

        int lit = s_lamp[i] > 0;
        if (!b->pop) {
            fb_disc(b->x, b->y, b->r, lit ? GB_WHITE : c->trim);
            fb_ring(b->x, b->y, b->r, mix565(c->glow, GB_BLACK, 60));
            continue;
        }

        /* Skirt, body, cap. The cap carries the initial of whoever is on the
         * backglass, which is the cheapest way to make three identical
         * bumpers belong to this ball rather than to the table. */
        fb_disc(b->x, b->y, b->r, mix565(c->trim, GB_BLACK, 110));
        fb_ring(b->x, b->y, b->r, lit ? GB_WHITE : c->glow);
        fb_disc(b->x, b->y, b->r - 3,
                lit ? GB_WHITE : mix565(c->body, GB_BLACK, 40));
        fb_ring(b->x, b->y, b->r - 3, mix565(c->trim, GB_BLACK, 40));

        char ch[2] = { initial[0], 0 };
        fb_text(b->x - 1, b->y - 2, ch,
                lit ? c->trim : mix565(c->glow, GB_WHITE, 80), 1);
    }
}

static void draw_drops(const cast_t *c)
{
    for (int i = 0; i < 3; i++) {
        const rect_t *r = &DROP[i];
        if (!in_band(r->y - 1, r->y + r->h + 1)) continue;

        if (s_drop_down[i]) {
            fb_box(r->x, r->y + r->h - 2, r->w, 2,
                   mix565(c->felt_bot, GB_BLACK, 140));
            continue;
        }
        uint16_t face = s_drop_fl[i] > 0 ? GB_WHITE
                                         : mix565(c->body, GB_WHITE, 40);
        fb_box(r->x, r->y, r->w, r->h, face);
        fb_frame(r->x, r->y, r->w, r->h, c->trim);
        fb_hspan(r->x + 1, r->x + r->w - 2, r->y + 1,
                 mix565(face, GB_WHITE, 120));
    }
}

static void draw_lanes(const cast_t *c)
{
    for (int i = 0; i < 3; i++) {
        int x = LANE[i][0], y = LANE[i][1];
        if (!in_band(y - 9, y + 5)) continue;

        int lit = s_lane_lit[i] || s_lane_fl[i] > 0;
        uint16_t col = lit ? GB_WHITE : mix565(c->trim, GB_WHITE, 40);

        fb_disc(x, y, 4, lit ? c->glow : mix565(c->trim, GB_BLACK, 60));
        fb_ring(x, y, 4, col);

        char ch[2] = { c->lane[i], 0 };
        fb_text(x - 1, y - 2, ch, lit ? c->trim : col, 1);
    }
}

/* The name across the lower playfield, in the felt rather than on it: real
 * tables print something down there and this one otherwise has 40 empty rows
 * above the flippers. */
static void draw_wordmark(const cast_t *c)
{
    if (!in_band(192, 200)) return;
    fb_text_ctrx(58, 194, cast_get(s_who)->name,
                 mix565(c->felt_bot, c->body, 70), 1);
}

static void draw_plunger(const cast_t *c)
{
    if (!in_band(200, 239)) return;

    int pull = (s_charge * 4) / 255;
    int top  = 236 + pull;

    /* The spring: five coils that compress as the plunger is wound back. */
    for (int i = 0; i < 5; i++) {
        int y = top + i + (i * (6 - pull)) / 6;
        if (y > 239) break;
        fb_hspan(118, 128, y, mix565(c->glow, GB_BLACK, i * 30));
    }
    fb_box(117, top - 2, 13, 2, mix565(c->body, GB_WHITE, 60));
}

static void draw_flippers(const cast_t *c)
{
    for (int i = 0; i < 2; i++) {
        int px = i == 0 ? PIVOT_LX : PIVOT_RX;
        int tx = flip_tip_x(i), ty = flip_tip_y(i);
        int ymin = ty < PIVOT_Y ? ty : PIVOT_Y;
        int ymax = ty < PIVOT_Y ? PIVOT_Y : ty;
        if (!in_band(ymin - FLIP_R - 1, ymax + FLIP_R + 1)) continue;

        fb_bar(px, PIVOT_Y, tx, ty, FLIP_R, c->trim);
        fb_bar(px, PIVOT_Y, tx, ty, FLIP_R - 1,
               s_flip_on[i] ? GB_WHITE : mix565(c->body, GB_WHITE, 30));
        fb_disc(px, PIVOT_Y, 2, mix565(c->trim, GB_BLACK, 80));
    }
}

static void draw_ball(const cast_t *c)
{
    int x = s_bx >> 8, y = s_by >> 8;
    if (!in_band(y - BALL_R - 1, y + BALL_R + 1)) return;

    /* Steel, with the character's colour picked up along the lower rim - the
     * one place on the table where the two of them are the light rather than
     * the picture. */
    fb_disc(x, y, BALL_R + 1, GB_RGB(8, 8, 12));
    fb_disc(x, y, BALL_R, GB_RGB(214, 218, 228));
    fb_disc(x, y + 1, BALL_R - 1, mix565(GB_RGB(120, 126, 140), c->glow, 70));
    fb_px(x - 1, y - 1, GB_WHITE);
    fb_px(x, y - 1, GB_RGB(240, 244, 252));
}

/* --------------------------------------------------------------- backglass */

static void draw_backglass(const cast_t *c)
{
    if (!in_band(0, TOP_H - 1)) return;

    fb_box(0, 0, g_w, TOP_H, GB_RGB(10, 10, 16));
    fb_hspan(0, g_w - 1, TOP_H - 1, c->trim);

    char buf[16];
    A->snprintf(buf, sizeof buf, "%u", (unsigned)s_score);
    fb_text(g_w - 2 - TXT_W(buf, 2), 3, buf, GB_WHITE, 2);

    A->snprintf(buf, sizeof buf, "BALL %d", s_ball);
    fb_text(3, 1, buf, mix565(c->glow, GB_WHITE, 60), 1);

    A->snprintf(buf, sizeof buf, "X%d", s_mult);
    fb_text(3, 9, buf, s_mult > 1 ? GB_YELLOW : GB_GREY, 1);

    /* Ball save and the left-hold warning both live here, because the
     * playfield is busy and this strip is where a player is already looking
     * for a number to change. */
    if (s_hold_l > QUIT_WARN_MS)
        fb_text(30, 9, "RELEASE L", GB_RED, 1);
    else if (s_save_ms > 0 && s_state == ST_PLAY)
        fb_text(30, 9, "SAVE", GB_GREEN, 1);
}

static void draw_msg(const cast_t *c)
{
    if (s_msg_ms <= 0 || !in_band(96, 108)) return;
    int a = s_msg_ms > 300 ? 256 : (int)(s_msg_ms * 256 / 300);
    fb_text_ctr(100, s_msg, mix565(c->felt_top, GB_WHITE, a), 1);
}

static void draw_ready(const cast_t *c)
{
    if (in_band(150, 176)) {
        fb_text_ctrx(58, 152, "HOLD R", GB_WHITE, 2);
        fb_text_ctrx(58, 166, "TO LAUNCH", mix565(c->glow, GB_WHITE, 90), 1);
    }
    if (in_band(178, 190))
        fb_text_ctrx(58, 180, c->taunt, mix565(c->body, GB_WHITE, 60), 1);

    /* The charge bar sits beside the lane, next to what it is charging. */
    if (in_band(150, 232)) {
        int h = (s_charge * 80) / 255;
        fb_frame(132, 150, 3, 82, c->trim);
        if (h > 0) fb_box(132, 150 + 80 - h, 3, h,
                          h > 70 ? GB_WHITE : c->glow);
    }
}

static void draw_drain(const cast_t *c)
{
    if (!in_band(96, 132)) return;
    char buf[22];
    fb_text_ctr(100, "BALL LOST", GB_WHITE, 2);
    A->snprintf(buf, sizeof buf, "BONUS %u", (unsigned)s_bonus);
    fb_text_ctr(116, buf, mix565(c->glow, GB_WHITE, 80), 1);
}

static void draw_over(const cast_t *c)
{
    if (in_band(84, 150)) {
        char buf[22];
        fb_text_ctr(86, "GAME OVER", GB_WHITE, 2);
        A->snprintf(buf, sizeof buf, "SCORE %u", (unsigned)s_score);
        fb_text_ctr(104, buf, mix565(c->glow, GB_WHITE, 90), 1);
        A->snprintf(buf, sizeof buf, "BEST %u", (unsigned)s_cfg.best);
        fb_text_ctr(114, buf, GB_YELLOW, 1);
        if (s_score >= s_cfg.best && s_score > 0)
            fb_text_ctr(126, "NEW RECORD!", GB_WHITE, 1);
        fb_text_ctr(140, "R AGAIN   L MENU", mix565(c->body, GB_WHITE, 70), 1);
    }
}

static void draw_attract(const cast_t *c)
{
    int pulse = 128 + (isin((int)(s_anim / 8)) >> 1);

    if (in_band(18, 44)) {
        fb_text_ctr(20, "PINBALL", GB_WHITE, 3);
        fb_text_ctr(38, "GUMBALL AND DARWIN", mix565(c->glow, GB_WHITE, 90), 1);
    }

    /* One face at a time, taking turns - at any scale where they read, two of
     * them do not fit across 135 columns. Whoever is up is also whose colours
     * the screen is wearing, which is the same swap the game makes at the
     * start of every ball. */
    cast_draw_face(s_who, 16, 48, 3);

    if (in_band(140, 220)) {
        char buf[22];
        fb_text_ctr(142, c->name, mix565(c->body, GB_WHITE, 40), 1);
        A->snprintf(buf, sizeof buf, "BEST %u", (unsigned)s_cfg.best);
        fb_text_ctr(154, buf, GB_YELLOW, 1);
        fb_text_ctr(170, "HOLD R TO START",
                    mix565(GB_WHITE, c->glow, 256 - pulse), 1);
        fb_text_ctr(186, "L R  FLIPPERS", GB_GREY, 1);
        fb_text_ctr(196, "HOLD R  PLUNGER", GB_GREY, 1);
        fb_text_ctr(206, "HOLD L 3S  QUIT", GB_GREY, 1);
        fb_text_ctr(214, "3 BALLS", mix565(c->body, GB_WHITE, 40), 1);
    }
}

static void render(void)
{
    const cast_t *c = cast_get(s_who);

    for (int y = 0; y < g_h; y += BAND_H) {
        gfx_band(y, (y + BAND_H <= g_h) ? BAND_H : (g_h - y));

        draw_felt(c);

        if (s_state == ST_ATTRACT) {
            draw_attract(c);
        } else {
            /* The portrait goes down first and everything else over it: it is
             * printed on the playfield, not standing in front of it. */
            cast_draw_backdrop(s_who, 8, 32, 3,
                               mix565(c->felt_top, c->felt_bot, 128));
            draw_wordmark(c);
            draw_walls(c);
            draw_lanes(c);
            draw_drops(c);
            draw_bumpers(c);
            draw_plunger(c);
            draw_flippers(c);
            draw_ball(c);
            draw_backglass(c);

            switch (s_state) {
            case ST_READY: draw_ready(c); break;
            case ST_DRAIN: draw_drain(c); break;
            case ST_OVER:  draw_over(c);  break;
            default:       draw_msg(c);   break;
            }
        }

        A->blit(0, (int16_t)y, g_w, (int16_t)g_band_h, g_fb);
    }
}

/* =================================================================== main */

int gb_main(const gb_api_t *api)
{
    A = api;

    save_t saved;
    if (api->store_get("cfg", &saved, sizeof saved) == (int)sizeof saved &&
        saved.magic == SAVE_MAGIC)
        s_cfg = saved;

    /*
     * Portrait, whatever the system orientation says. Unlike astro there is no
     * flip to offer: which way up a table is is not a matter of taste, the
     * plunger goes at the bottom and the ball falls towards it, so a board set
     * to rotation 2 gets rotation 0 all the same.
     */
    api->set_rotation(0);
    g_w = api->width();
    g_h = api->height();
    if (g_w > SCR_MAX_W) g_w = SCR_MAX_W;
    if (g_h > SCR_MAX_H) g_h = SCR_MAX_H;
    /* The band buffer is the guest's; the primitives that fill it are the
     * OS's. This is where the two are introduced. */
    gfx_attach(api, g_fb, g_w, BAND_H);

    rnd_seed(api->millis() * 2654435761u + api->unix_time() + 7u);

    s_flip_a[0] = s_flip_a[1] = FLIP_REST << 8;
    s_who   = (int)(rnd() & 1u);
    s_mult  = 1;
    s_ball  = 1;
    s_state = ST_ATTRACT;
    reset_table();
    park_ball();

    uint32_t last = api->millis();

    while (!api->should_stop()) {
        uint32_t now = api->millis();
        uint32_t dt  = now - last;
        last = now;
        /* A long stall - the console writing to NVS, say - must not teleport
         * the ball through a wall. Clamp rather than let one frame move it. */
        if (dt > 60) dt = 60;
        if (dt == 0) dt = 1;

        if (!handle_input()) break;
        read_buttons(dt);
        update(dt);
        render();

        /* Always hand back at least one tick: the guest task sits at priority
         * 4, the idle task at 0, and the OS builds with the idle-task watchdog
         * on, so a loop that only ever waited on the SPI bus would trip it. */
        uint32_t spent = api->millis() - now;
        api->sleep_ms(spent >= FRAME_MS - 10 ? 10 : FRAME_MS - spent);
    }

    api->log("exit");
    return 0;
}
