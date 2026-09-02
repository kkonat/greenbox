/*
 * astro.c - ASTRO, a vertical scroller for greenbox.
 *
 * You fly up through an asteroid field that never ends. The score is the
 * distance you cover before something hits you.
 *
 * Controls:
 *   L held       thrust left, for as long as it is down
 *   R held       thrust right
 *   L held 3s    the OS escape gesture - see below
 *
 * The ship has mass. Thrust accelerates it and letting go does not stop it -
 * there is a little drag and nothing else, so crossing the field means firing
 * the other way in time to arrive. That is the whole of the skill: the rocks
 * are not hard to see coming, they are hard to be somewhere else for.
 *
 * Movement reads api->buttons() rather than the event queue. Events describe
 * gestures that have already finished - a tap when the button comes up, a hold
 * when its threshold is earned - and none of them says when a hold ends, so
 * nothing built on events alone can move while a button is down and stop when
 * it is released. The menus still use events, because a menu wants gestures.
 *
 * One thing the game cannot take back: three seconds on the left button is the
 * OS escape, and it arrives with the kill. Crossing the field takes well under
 * a second, so holding left that long means parked against the wall rather
 * than flying, but it is still a run ending for what looks like no reason -
 * hence the warning the HUD puts up as it approaches.
 *
 * ------------------------------------------------------------------ display
 *
 * The game asks the OS for portrait. A vertical scroller with 135 rows of
 * height gives about a second and a half of warning at speed, which is not
 * enough to dodge with two buttons; turned the other way there are 240 rows
 * and the same field is playable. api->set_rotation() exists for this, and the
 * OS restores its own orientation when the guest exits.
 *
 * If the user already runs the board in portrait, that is the orientation this
 * uses: their choice of which way up is honoured and there is nothing left to
 * decide. If they run it in landscape there is no way to derive which portrait
 * they meant - turning the board left and turning it right are both reasonable
 * - so one is picked and L on the title screen flips it. That flip is kept
 * here rather than written to the OS settings, because wanting a game the
 * other way up is not a reason to turn the launcher upside down.
 *
 * -------------------------------------------------------------- performance
 *
 * Every frame is a full repaint - there is nothing static left on screen once
 * the background is parallaxing - which is what the band buffer in astro_gfx.c
 * is for. A frame is about 32000 pixels of drawing into RAM plus 65 KB down
 * the SPI bus at 40 MHz, and lands comfortably inside the 30 ms budget below.
 *
 * The loop always gives back at least one tick. The guest task runs at
 * priority 4 with the idle task at 0, and the OS builds with the idle-task
 * watchdog enabled: a render loop that never blocks would trip it in five
 * seconds.
 */

#include "astro.h"

/* ================================================================ tuning */

#define SHIP_W       15
#define SHIP_H       13
#define SHIP_HIT      6         /* forgiving: the wings are decoration */
#define SHIP_MARGIN   2
#define SHIP_ACCEL 1200         /* px/s^2 under thrust */
#define SHIP_DRAG   400         /* px/s^2 coasting - deliberately small */
#define SHIP_VMAX   185         /* px/s */
#define QUIT_WARN_MS 1800       /* when to mention the 3 s left-hold */

#define V_BASE       72         /* px/s at zero distance */
#define V_MAX       300
#define V_RAMP      150         /* distance per +1 px/s */

#define MSL_SPEED   420         /* px/s up the screen, screen-relative */

#define MAX_ROCK     26
#define MAX_PUP       3
#define MAX_MSL       8
#define MAX_DEB      44

#define FRAME_MS     30

/* ============================================================== the ship */

static const char *const SHIP_ART[SHIP_H] = {
    ".......A.......",
    "......AHA......",
    "......HCH......",
    ".....AHCHA.....",
    ".....HHCHH.....",
    "....AHHHHHA....",
    "...RAHHHHHAR...",
    "..RRRHHHHHRRR..",
    ".RRRRHHbHHRRRR.",
    "RRR..HbbbH..RRR",
    "......bbb......",
    "......b.b......",
    ".......b.......",
};

static const spal_t SHIP_PAL[] = {
    { 'A', GB_RGB(226, 236, 252) },     /* highlight */
    { 'H', GB_RGB(126, 146, 176) },     /* hull */
    { 'C', GB_RGB( 90, 232, 255) },     /* canopy */
    { 'R', GB_RGB(206,  58,  62) },     /* wing flash */
    { 'b', GB_RGB( 52,  62,  82) },     /* shadow / engine housing */
};
#define SHIP_NPAL ((int)(sizeof SHIP_PAL / sizeof SHIP_PAL[0]))

/* Exhaust and every explosion in the game come off this ramp. */
static const uint16_t FIRE[6] = {
    GB_RGB(255,255,235), GB_RGB(255,232,140), GB_RGB(255,170, 50),
    GB_RGB(240, 96, 24), GB_RGB(150, 40, 20), GB_RGB( 70, 18, 14),
};

/* ============================================================== powerups */
/*
 * ADDING A POWERUP
 *
 *   1. add it to pu_type_t, before PU_COUNT
 *   2. add a row to PU_DEF - tag, spawn weight, glow colour
 *   3. add its art and a case to pu_art()
 *   4. add a row to PU_GRANT, and whatever per-level numbers the effect wants
 *   5. spend it somewhere - that is the only type-specific code left
 *
 * Acquisition is entirely table-driven now: spawning, weighting, the pickup
 * test, the flash, the count, the status bar and the upgrade rule all read the
 * tables and none of them needs a case. Only *spending* a powerup knows what
 * kind it is, because only that differs - a shield is spent by being hit and a
 * missile by being fired.
 *
 * Powerups accumulate. A pickup adds PU_GRANT units to a stock that is carried
 * until it is used, capped at PU_MAX_CARRY so the status bar stays one digit
 * wide. Picking one up with full pockets is not wasted: it raises that
 * powerup's level instead, which is what the per-level tables scale. Levels
 * last for the run.
 */
typedef enum {
    PU_SHIELD = 0,
    PU_MISSILE,
    PU_COUNT
} pu_type_t;

#define PU_MAX_LEVEL 3
#define PU_CARRY_MAX 20         /* the widest any carry cap may be */
#define PU_ART_W     11
#define PU_ART_H     10
#define BAR_H        14         /* the status bar along the bottom */

typedef struct {
    const char *tag;
    uint8_t     weight;
    uint16_t    glow;
} pu_def_t;

static const pu_def_t PU_DEF[PU_COUNT] = {
    { "SHLD", 10, GB_RGB( 90, 210, 255) },
    { "MSL",   8, GB_RGB(255, 150,  60) },
};

/*
 * Eleven pixels is not much to say "shield" or "missile" in, so both icons are
 * solid shapes with an outline rather than line art: at this size an outlined
 * symbol survives being sat on top of a nebula and a hollow one does not.
 */
static const char *const SHIELD_ART[PU_ART_H] = {
    ".WWWWWWWWW.",
    ".WcccccccW.",
    ".WcHHHHHcW.",
    ".WcHHHHHcW.",
    ".WcHHHHHcW.",
    "..WcHHHcW..",
    "...WcHcW...",
    "....WcW....",
    ".....W.....",
    "...........",
};

static const char *const MISSILE_ART[PU_ART_H] = {
    ".....W.....",
    "....WWW....",
    "...WWaWW...",
    "...WaaaW...",
    "..WWaaaWW..",
    "..WaaaaaW..",
    ".WWWaaaWWW.",
    ".W.WWaWW.W.",
    ".....f.....",
    "....fff....",
};

static const spal_t SHIELD_PAL[] = {
    { 'W', GB_RGB(210, 250, 255) },
    { 'c', GB_RGB( 60, 170, 220) },
    { 'H', GB_RGB( 18,  60, 104) },
};
static const spal_t MISSILE_PAL[] = {
    { 'W', GB_RGB(232, 236, 240) },
    { 'a', GB_RGB(210,  70,  50) },
    { 'f', GB_RGB(255, 190,  70) },
};

static const char *const *pu_art(int type, const spal_t **pal, int *npal)
{
    switch (type) {
    case PU_MISSILE:
        *pal = MISSILE_PAL;
        *npal = (int)(sizeof MISSILE_PAL / sizeof MISSILE_PAL[0]);
        return MISSILE_ART;
    case PU_SHIELD:
    default:
        *pal = SHIELD_PAL;
        *npal = (int)(sizeof SHIELD_PAL / sizeof SHIELD_PAL[0]);
        return SHIELD_ART;
    }
}

/*
 * How much of each powerup can be carried, and what one pickup is worth at
 * each level. The two caps are nothing like each other on purpose: four
 * absorbed hits is already a lot of second chances, while a missile is spent
 * in a third of a second and a stock that small would be gone before the
 * player noticed they had it.
 */
static const uint8_t PU_CARRY[PU_COUNT]                   = { 4, 20 };
static const uint8_t PU_GRANT[PU_COUNT][PU_MAX_LEVEL + 1] = {
    { 1, 1, 2, 2 },         /* shield: hits absorbed */
    { 6, 8, 10, 12 },       /* missile: rounds */
};

/* Per-level numbers for the effects themselves. */
static const int16_t MSL_PERIOD[PU_MAX_LEVEL + 1] = { 520, 400, 300, 220 };

/* ============================================================== entities */

typedef struct {
    int32_t x_q8, y_q8;
    int16_t vx;             /* px/s, bounces off the walls */
    int16_t vy;             /* px/s on top of the scroll */
    int32_t rot_q8;
    int16_t spin;           /* turn units per second, 256 = one revolution */
    uint8_t rad;
    uint8_t shape, kind;
    uint8_t alive;
} rock_t;

typedef struct {
    int32_t x_q8, y_q8;
    int16_t vx;
    uint8_t type;
    uint8_t ph;
    uint8_t alive;
} pup_t;

/* No vx: missiles go straight up and nothing steers them. */
typedef struct {
    int32_t x_q8, y_q8;
    uint8_t alive;
} msl_t;

typedef struct {
    int32_t x_q8, y_q8;
    int16_t vx, vy;
    int16_t life, life0;    /* ms */
    uint8_t big;
    uint8_t alive;
} deb_t;

static rock_t s_rock[MAX_ROCK];
static pup_t  s_pup[MAX_PUP];
static msl_t  s_msl[MAX_MSL];
static deb_t  s_deb[MAX_DEB];

/* ================================================================= state */

typedef enum { ST_TITLE = 0, ST_PLAY, ST_DYING, ST_OVER } state_t;

typedef struct {
    uint16_t magic;
    uint16_t rot;           /* 0 or 2 - only consulted in the landscape case */
    uint32_t best;
} save_t;

#define SAVE_MAGIC 0xA57Cu

static save_t   s_cfg = { SAVE_MAGIC, 0, 0 };
static state_t  s_state;

static int32_t  s_sx_q8;        /* ship centre */
static int      s_vx;           /* px/s, signed */
static int      s_ship_y;
static int      s_lean;
static int      s_hold_l;       /* ms the left button has been down */

static uint32_t s_dist;
static int32_t  s_dist_q8;
static int      s_scroll;

static uint8_t  s_lvl[PU_COUNT];        /* potency, raised by a full-pocket pickup */
static uint8_t  s_have[PU_COUNT];       /* stock, spent by use */
static int32_t  s_msl_cool;
static int32_t  s_invuln;
static int32_t  s_pickup_flash;
static uint8_t  s_pickup_type;

static int32_t  s_rock_next, s_pup_next;
static int32_t  s_die_ms;
static uint8_t  s_flame;
static uint8_t  s_pulse;
static int      s_new_best;
static int      s_flip_ok;      /* whether to offer the title-screen flip */

/* ================================================================= util */

static void u32str(char *buf, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

/* label, two spaces, number - the only string building this program does. */
static void labelnum(char *buf, const char *label, uint32_t v)
{
    int n = 0;
    while (*label) buf[n++] = *label++;
    buf[n++] = ' ';
    buf[n++] = ' ';
    u32str(buf + n, v);
}

static int adv(int v_per_s, int dt_ms)
{
    return (v_per_s * dt_ms * 256) / 1000;      /* px/s -> Q8 px */
}

/* ============================================================ entity pool */

static void spawn_debris(int cx, int cy, int n, int speed, int life)
{
    for (int i = 0; i < MAX_DEB && n > 0; i++) {
        deb_t *d = &s_deb[i];
        if (d->alive) continue;
        int a = (int)(rnd() & 255);
        int s = rnd_range(speed / 3, speed);
        d->x_q8 = (int32_t)cx << 8;
        d->y_q8 = (int32_t)cy << 8;
        d->vx   = (int16_t)((icos(a) * s) >> 8);
        d->vy   = (int16_t)((isin(a) * s) >> 8);
        d->life0 = d->life = (int16_t)rnd_range(life / 2, life);
        d->big   = (uint8_t)((rnd() & 3) == 0);
        d->alive = 1;
        n--;
    }
}

static void rock_explode(rock_t *r)
{
    r->alive = 0;
    spawn_debris((int)(r->x_q8 >> 8), (int)(r->y_q8 >> 8),
                 4 + r->rad / 2, 60 + r->rad * 6, 420 + r->rad * 18);
}

static void spawn_rock(void)
{
    int slot = -1;
    for (int i = 0; i < MAX_ROCK; i++)
        if (!s_rock[i].alive) { slot = i; break; }
    if (slot < 0) return;

    int rad = rnd_range(5, 16);

    /*
     * Refuse a placement that walls off the top of the field. With two buttons
     * the player cannot cross the screen in a hurry, so a row of rocks with no
     * gap is not difficulty, it is a dead end.
     */
    int x = 0, ok = 0;
    for (int try = 0; try < 8 && !ok; try++) {
        x = rnd_range(rad + 2, g_w - rad - 3);
        ok = 1;
        for (int i = 0; i < MAX_ROCK; i++) {
            rock_t *o = &s_rock[i];
            if (!o->alive) continue;
            if ((int)(o->y_q8 >> 8) > 64) continue;     /* already past */
            int dx = x - (int)(o->x_q8 >> 8);
            if (dx < 0) dx = -dx;
            if (dx < rad + o->rad + 18) { ok = 0; break; }
        }
    }
    /*
     * Giving up rather than forcing it is what keeps the field fair as the
     * spawn rate climbs: the interval below asks for rocks faster than the
     * screen can fairly hold them, and this is the valve. Density rises until
     * there is nowhere left to put one, and then stops.
     */
    if (!ok) return;

    rock_t *r = &s_rock[slot];
    r->x_q8  = (int32_t)x << 8;
    r->y_q8  = (int32_t)(-rad - 3) << 8;
    r->vx    = (int16_t)rnd_range(-18, 18);
    r->vy    = (int16_t)rnd_range(-8, 26);
    r->rot_q8 = (int32_t)rnd_range(0, 255) << 8;
    r->spin  = (int16_t)rnd_range(-70, 70);
    r->rad   = (uint8_t)rad;
    r->shape = (uint8_t)rnd_range(0, 3);
    r->kind  = (uint8_t)(rnd_range(0, 9) < 6 ? 0 : rnd_range(1, 2));
    r->alive = 1;
}

static void spawn_pup(void)
{
    int slot = -1;
    for (int i = 0; i < MAX_PUP; i++)
        if (!s_pup[i].alive) { slot = i; break; }
    if (slot < 0) return;

    int total = 0;
    for (int i = 0; i < PU_COUNT; i++) total += PU_DEF[i].weight;
    int pick = rnd_range(0, total - 1), type = 0;
    for (int i = 0; i < PU_COUNT; i++) {
        pick -= PU_DEF[i].weight;
        if (pick < 0) { type = i; break; }
    }

    pup_t *p = &s_pup[slot];
    p->x_q8 = (int32_t)rnd_range(PU_ART_W, g_w - PU_ART_W) << 8;
    p->y_q8 = -((int32_t)(PU_ART_H + 2) << 8);
    p->vx   = (int16_t)rnd_range(-12, 12);
    p->type = (uint8_t)type;
    p->ph   = 0;
    p->alive = 1;
}

/* No switch, and there should never be one here: what a powerup is worth is a
 * number in a table, and what it does is somebody else's problem. */
static void pu_apply(int type)
{
    s_pickup_flash = 420;
    s_pickup_type  = (uint8_t)type;

    int cap = PU_CARRY[type];
    if (s_have[type] >= cap) {
        /* Pockets full. Rather than drop it on the floor, the pickup buys
         * potency - which is the only way levels are ever reached. */
        if (s_lvl[type] < PU_MAX_LEVEL) s_lvl[type]++;
        return;
    }

    int n = s_have[type] + PU_GRANT[type][s_lvl[type]];
    s_have[type] = (uint8_t)(n > cap ? cap : n);
}

/* False when every slot is busy, so the caller does not spend a round on a
 * missile that was never launched. */
static int fire_missile(void)
{
    for (int i = 0; i < MAX_MSL; i++) {
        if (s_msl[i].alive) continue;
        s_msl[i].x_q8 = s_sx_q8;
        s_msl[i].y_q8 = (int32_t)(s_ship_y + 2) << 8;
        s_msl[i].alive = 1;
        return 1;
    }
    return 0;
}

/* ================================================================== reset */

static void reset_run(void)
{
    for (int i = 0; i < MAX_ROCK; i++) s_rock[i].alive = 0;
    for (int i = 0; i < MAX_PUP; i++)  s_pup[i].alive = 0;
    for (int i = 0; i < MAX_MSL; i++)  s_msl[i].alive = 0;
    for (int i = 0; i < MAX_DEB; i++)  s_deb[i].alive = 0;
    for (int i = 0; i < PU_COUNT; i++) { s_lvl[i] = 0; s_have[i] = 0; }

    s_ship_y   = g_h - 44;
    s_sx_q8    = (int32_t)(g_w / 2) << 8;
    s_vx       = 0;
    s_lean     = 0;
    s_hold_l   = 0;
    s_dist     = 0;
    s_dist_q8  = 0;
    s_scroll   = V_BASE;
    s_msl_cool = 0;
    s_invuln   = 0;
    s_pickup_flash = 0;
    s_rock_next = 700;
    s_pup_next  = rnd_range(6000, 11000);
    s_die_ms    = 0;
    s_new_best  = 0;
}

static void save_cfg(void)
{
    A->store_put("cfg", &s_cfg, sizeof s_cfg);
}

/* ================================================================= input */

/*
 * Which portrait rotation to use, given what the user has the OS set to. A
 * system already in portrait wins outright; a system in landscape leaves the
 * question open, and the stored local flip answers it.
 */
static uint8_t want_rotation(void)
{
    gb_oscfg_t os;
    A->oscfg_get(&os);
    if ((os.rotation & 1) == 0) return os.rotation;     /* already portrait */
    return (uint8_t)s_cfg.rot;
}

static void resize(void)
{
    g_w = A->width();
    g_h = A->height();
    if (g_w > SCR_MAX_W) g_w = SCR_MAX_W;
    if (g_h > SCR_MAX_H) g_h = SCR_MAX_H;
    /* The band buffer is the guest's; the primitives that fill it are the
     * OS's. This is where the two are introduced, and it has to happen again
     * after a rotation change because the row stride is g_w. */
    gfx_attach(A, g_fb, g_w, BAND_H);
    bg_init();
}

/* Returns 0 when the player asked to leave. */
static int handle_input(void)
{
    gb_event_t e;
    while ((e = A->poll_event()) != GB_EV_NONE) {
        if (e == GB_EV_L_LONG) return 0;

        switch (s_state) {
        case ST_TITLE:
            if (e == GB_EV_R_SHORT || e == GB_EV_R_LONG) {
                reset_run();
                s_state = ST_PLAY;
            } else if (e == GB_EV_L_SHORT && s_flip_ok) {
                s_cfg.rot = (uint16_t)(s_cfg.rot == 0 ? 2 : 0);
                save_cfg();
                A->set_rotation((uint8_t)s_cfg.rot);
                resize();
                reset_run();
            }
            break;

        case ST_PLAY:
            /* Nothing. Steering is read from buttons() in update_ship, so the
             * taps and holds that arrive here are the tail ends of moves that
             * have already been made, and the escape is handled above. */
            break;

        case ST_OVER:
            if (e == GB_EV_R_SHORT || e == GB_EV_R_LONG) {
                reset_run();
                s_state = ST_PLAY;
            } else if (e == GB_EV_L_SHORT) {
                reset_run();
                s_state = ST_TITLE;
            }
            break;

        default:
            break;      /* ST_DYING eats everything but the escape */
        }
    }
    return 1;
}

/* ================================================================ update */

static void die(void)
{
    spawn_debris((int)(s_sx_q8 >> 8), s_ship_y + SHIP_H / 2, 26, 200, 1400);
    /* The launcher went with the ship. Nothing left to home in on, and they
     * would otherwise hang in the air for the rest of the death animation. */
    for (int i = 0; i < MAX_MSL; i++) s_msl[i].alive = 0;
    bg_flash();
    s_state  = ST_DYING;
    s_die_ms = 1100;

    if (s_dist > s_cfg.best) {
        s_cfg.best = s_dist;
        s_new_best = 1;
        save_cfg();
    }
}

static void update_ship(uint32_t dt)
{
    uint8_t b = A->buttons();
    int want = 0;
    if (b & GB_BTN_L) want--;
    if (b & GB_BTN_R) want++;

    s_hold_l = (b & GB_BTN_L) ? s_hold_l + (int)dt : 0;

    if (want == 0) {
        /* Drag, not braking. At 400 px/s^2 a ship at full tilt takes most of
         * half a second to come to rest, so releasing the button is a decision
         * about where you will end up rather than a way to stop. */
        int dv = (SHIP_DRAG * (int)dt) / 1000;
        if (s_vx > dv)       s_vx -= dv;
        else if (s_vx < -dv) s_vx += dv;
        else                 s_vx  = 0;
    } else {
        /* Reversing gets the drag as well as the thrust, so counter-firing is
         * always stronger than accelerating and the ship stays steerable. */
        int a = (want * s_vx < 0) ? SHIP_ACCEL + SHIP_DRAG : SHIP_ACCEL;
        s_vx += want * ((a * (int)dt) / 1000);
        if (s_vx >  SHIP_VMAX) s_vx =  SHIP_VMAX;
        if (s_vx < -SHIP_VMAX) s_vx = -SHIP_VMAX;
    }

    s_sx_q8 += adv(s_vx, (int)dt);

    int32_t lo = (int32_t)(SHIP_W / 2 + SHIP_MARGIN) << 8;
    int32_t hi = (int32_t)(g_w - SHIP_W / 2 - SHIP_MARGIN) << 8;
    /* The walls absorb rather than bounce: a bounce at this speed throws you
     * back into the field you were trying to get away from. */
    if (s_sx_q8 < lo) { s_sx_q8 = lo; if (s_vx < 0) s_vx = 0; }
    if (s_sx_q8 > hi) { s_sx_q8 = hi; if (s_vx > 0) s_vx = 0; }

    s_lean = s_vx > 45 ? 1 : (s_vx < -45 ? -1 : 0);
}

static void update_rocks(uint32_t dt)
{
    for (int i = 0; i < MAX_ROCK; i++) {
        rock_t *r = &s_rock[i];
        if (!r->alive) continue;

        r->y_q8 += adv(s_scroll + r->vy, (int)dt);
        r->x_q8 += adv(r->vx, (int)dt);
        r->rot_q8 += ((int32_t)r->spin * (int)dt * 256) / 1000;

        int x = (int)(r->x_q8 >> 8);
        if (x < r->rad && r->vx < 0)             r->vx = (int16_t)-r->vx;
        if (x > g_w - r->rad - 1 && r->vx > 0)   r->vx = (int16_t)-r->vx;

        if ((int)(r->y_q8 >> 8) - r->rad > g_h) r->alive = 0;
    }

    s_rock_next -= (int)dt;
    if (s_rock_next <= 0) {
        spawn_rock();
        int base = 600 - (int)(s_dist / 80);
        if (base < 150) base = 150;
        s_rock_next = rnd_range((base * 7) / 10, (base * 13) / 10);
    }
}

static void update_pups(uint32_t dt)
{
    for (int i = 0; i < MAX_PUP; i++) {
        pup_t *p = &s_pup[i];
        if (!p->alive) continue;
        p->y_q8 += adv(s_scroll - 12, (int)dt);
        p->x_q8 += adv(p->vx, (int)dt);
        p->ph = (uint8_t)(p->ph + ((dt * 3) >> 4));     /* ~1.7 s a cycle */

        int x = (int)(p->x_q8 >> 8);
        if (x < PU_ART_W / 2 || x > g_w - PU_ART_W / 2) p->vx = (int16_t)-p->vx;
        if ((int)(p->y_q8 >> 8) - PU_ART_H > g_h) p->alive = 0;
    }

    s_pup_next -= (int)dt;
    if (s_pup_next <= 0) {
        spawn_pup();
        s_pup_next = rnd_range(9000, 16000);
    }
}

static void update_missiles(uint32_t dt)
{
    /*
     * Auto-fire, but only with something in the column ahead. A forward-only
     * missile fired at empty sky is a round gone, and a stock that empties
     * itself while the screen is clear is a stock the player never gets to
     * decide anything about. This is trigger discipline, not aiming: the
     * missile still goes straight up and still misses if the rock moves.
     */
    if (s_have[PU_MISSILE] > 0) {
        s_msl_cool -= (int)dt;
        if (s_msl_cool <= 0) {
            int sx = (int)(s_sx_q8 >> 8), target = 0;
            for (int k = 0; k < MAX_ROCK && !target; k++) {
                const rock_t *r = &s_rock[k];
                if (!r->alive) continue;
                if ((int)(r->y_q8 >> 8) > s_ship_y) continue;   /* behind */
                int dx = sx - (int)(r->x_q8 >> 8);
                if (dx < 0) dx = -dx;
                if (dx <= r->rad + 4) target = 1;
            }
            if (target && fire_missile()) {
                s_have[PU_MISSILE]--;
                s_msl_cool = MSL_PERIOD[s_lvl[PU_MISSILE]];
            }
        }
    } else {
        s_msl_cool = 0;         /* first round of the next pickup goes at once */
    }

    for (int i = 0; i < MAX_MSL; i++) {
        msl_t *m = &s_msl[i];
        if (!m->alive) continue;

        /* Straight up, and nothing steers it. Homing missiles removed the
         * reason to line the ship up, which is the only aiming two buttons and
         * a lot of momentum can offer. */
        m->y_q8 -= adv(MSL_SPEED, (int)dt);
        if ((int)(m->y_q8 >> 8) < -8) { m->alive = 0; continue; }

        int mx = (int)(m->x_q8 >> 8), my = (int)(m->y_q8 >> 8);
        for (int k = 0; k < MAX_ROCK; k++) {
            rock_t *r = &s_rock[k];
            if (!r->alive) continue;
            int dx = mx - (int)(r->x_q8 >> 8);
            int dy = my - (int)(r->y_q8 >> 8);
            int rr = r->rad + 3;
            if (dx * dx + dy * dy < rr * rr) {
                rock_explode(r);
                m->alive = 0;
                break;
            }
        }
    }
}

static void update_debris(uint32_t dt)
{
    for (int i = 0; i < MAX_DEB; i++) {
        deb_t *d = &s_deb[i];
        if (!d->alive) continue;
        d->x_q8 += adv(d->vx, (int)dt);
        d->y_q8 += adv(d->vy + s_scroll / 2, (int)dt);
        d->life -= (int16_t)dt;
        if (d->life <= 0) d->alive = 0;
    }
}

static void update_collisions(uint32_t dt)
{
    int scx = (int)(s_sx_q8 >> 8);
    int scy = s_ship_y + SHIP_H / 2;

    for (int i = 0; i < MAX_PUP; i++) {
        pup_t *p = &s_pup[i];
        if (!p->alive) continue;
        int dx = scx - (int)(p->x_q8 >> 8);
        int dy = scy - (int)(p->y_q8 >> 8);
        if (dx * dx + dy * dy < (SHIP_HIT + 7) * (SHIP_HIT + 7)) {
            pu_apply(p->type);
            p->alive = 0;
        }
    }

    if (s_invuln > 0) { s_invuln -= (int)dt; return; }

    for (int i = 0; i < MAX_ROCK; i++) {
        rock_t *r = &s_rock[i];
        if (!r->alive) continue;
        int dx = scx - (int)(r->x_q8 >> 8);
        int dy = scy - (int)(r->y_q8 >> 8);
        int rr = SHIP_HIT + (r->rad * 8) / 10;
        if (dx * dx + dy * dy >= rr * rr) continue;

        if (s_have[PU_SHIELD] > 0) {
            s_have[PU_SHIELD]--;
            /* A moment of grace, or a tight cluster strips every charge in
             * the same frame and the shield never feels like it did anything. */
            s_invuln = 500;
            rock_explode(r);
        } else {
            die();
        }
        return;
    }
}

static void update(uint32_t dt)
{
    /* Divided, not masked. A phase byte advanced by dt*5 wraps twice a frame
     * at 30 ms and every pulse in the game becomes a strobe. */
    s_pulse = (uint8_t)(s_pulse + ((dt * 5) >> 5));
    s_flame = (uint8_t)rnd_range(0, 2);
    if (s_pickup_flash > 0) s_pickup_flash -= (int)dt;

    switch (s_state) {
    case ST_TITLE:
        s_scroll = V_BASE;
        break;

    case ST_PLAY:
        s_scroll = V_BASE + (int)(s_dist / V_RAMP);
        if (s_scroll > V_MAX) s_scroll = V_MAX;
        s_dist_q8 += adv(s_scroll, (int)dt);
        s_dist = (uint32_t)(s_dist_q8 >> 8);

        update_ship(dt);
        update_rocks(dt);
        update_pups(dt);
        update_missiles(dt);
        update_collisions(dt);
        break;

    case ST_DYING:
        update_rocks(dt);
        update_pups(dt);
        s_die_ms -= (int)dt;
        if (s_die_ms <= 0) s_state = ST_OVER;
        break;

    case ST_OVER:
        /* The field keeps going without you. Freezing it instead would park a
         * dozen rocks in mid-air behind the panel, which reads as a crash of
         * the other kind. */
        s_scroll = V_BASE / 2;
        update_rocks(dt);
        update_pups(dt);
        break;
    }

    update_debris(dt);
    bg_update(dt, s_scroll);
}

/* ================================================================ drawing */

/*
 * Two cones, the hot one inside the cool one, with the length jittered once
 * per frame. Drawing it per band would re-roll the jitter and the flame would
 * come out in slices, which is why s_flame is picked in update().
 */
static void draw_flame(int cx, int y)
{
    int len = 5 + s_flame;
    for (int i = 0; i < len; i++) {
        int hw = ((len - i) * 2) / 3;
        if (hw > 2) hw = 2;
        fb_hspan(cx - hw, cx + hw, y + i, FIRE[2 + (i * 3) / len]);
    }
    for (int i = 0; i < len - 2; i++)
        fb_hspan(cx - (i < 2 ? 1 : 0), cx + (i < 2 ? 1 : 0), y + i,
                 FIRE[i < 2 ? 0 : 1]);
}

static void draw_ship(void)
{
    int cx = (int)(s_sx_q8 >> 8);
    int x  = cx - SHIP_W / 2;

    draw_flame(cx, s_ship_y + SHIP_H);
    draw_sprite(SHIP_ART, SHIP_H, SHIP_W, x, s_ship_y,
                SHIP_PAL, SHIP_NPAL, s_lean);

    if (s_have[PU_SHIELD] > 0) {
        /* The bubble breathes, and beats faster on the last charge. */
        int t = s_pulse < 128 ? s_pulse : 255 - s_pulse;
        int r = 13 + (t >> 6);
        uint16_t col = mix565(GB_RGB(20, 70, 110), GB_RGB(150, 240, 255),
                              64 + ((t * 3) >> 1));
        if (s_invuln > 0 && ((s_invuln / 60) & 1)) col = GB_RGB(255, 255, 255);
        fb_ellipse(cx, s_ship_y + SHIP_H / 2, r, r - 2, col);
    }
}

static void draw_world(void)
{
    for (int i = 0; i < MAX_ROCK; i++) {
        const rock_t *r = &s_rock[i];
        if (!r->alive) continue;
        draw_rock((int)(r->x_q8 >> 8), (int)(r->y_q8 >> 8), r->rad,
                  (int)((r->rot_q8 >> 8) & 255), r->shape, r->kind);
    }

    for (int i = 0; i < MAX_PUP; i++) {
        const pup_t *p = &s_pup[i];
        if (!p->alive) continue;
        int x = (int)(p->x_q8 >> 8), y = (int)(p->y_q8 >> 8);
        int t = p->ph < 128 ? p->ph : 255 - p->ph;

        /* Dark disc, icon, then a halo that pulses well clear of the icon.
         * A ring drawn tight against the art merges with it and the whole
         * thing reads as a doughnut with something in the middle. */
        fb_disc(x, y, 7, GB_RGB(6, 8, 18));
        const spal_t *pal; int npal;
        const char *const *art = pu_art(p->type, &pal, &npal);
        draw_sprite(art, PU_ART_H, PU_ART_W,
                    x - PU_ART_W / 2, y - PU_ART_H / 2 - 1, pal, npal, 0);
        int hr = 9 + (t >> 6);
        fb_ellipse(x, y, hr, hr, mix565(GB_RGB(8, 10, 20),
                                        PU_DEF[p->type].glow, 96 + t));
    }

    for (int i = 0; i < MAX_MSL; i++) {
        const msl_t *m = &s_msl[i];
        if (!m->alive) continue;
        int x = (int)(m->x_q8 >> 8), y = (int)(m->y_q8 >> 8);
        fb_px(x, y, FIRE[0]);
        fb_px(x, y + 1, FIRE[1]);
        fb_px(x, y + 2, FIRE[2]);
        fb_px(x, y + 3, FIRE[3]);
        fb_px(x, y + 4, FIRE[4]);
    }

    for (int i = 0; i < MAX_DEB; i++) {
        const deb_t *d = &s_deb[i];
        if (!d->alive) continue;
        int idx = ((d->life0 - d->life) * 6) / (d->life0 ? d->life0 : 1);
        if (idx < 0) idx = 0; else if (idx > 5) idx = 5;
        int x = (int)(d->x_q8 >> 8), y = (int)(d->y_q8 >> 8);
        if (d->big) fb_box(x, y, 2, 2, FIRE[idx]);
        else        fb_px(x, y, FIRE[idx]);
    }

    if (s_state == ST_PLAY) draw_ship();
}

/* ---- HUD ---- */

/*
 * The status bar along the bottom: what is in the hold, and how much of it.
 *
 * Every powerup gets a slot whether or not any have been collected, dimmed
 * into the bar when the count is zero. A HUD element that appears only once
 * you already have the thing cannot tell you the thing exists, and with two
 * buttons and no menu there is nowhere else to find out.
 *
 * The whole row is centred as a group rather than pinned to an edge, so a
 * third and fourth powerup will lay themselves out without touching this.
 */
static void draw_statusbar(void)
{
    const int y0 = g_h - BAR_H;
    /* The count field is sized for two digits whatever the cap happens to be,
     * so the row does not shuffle sideways as a stock crosses ten. */
    const int ew = PU_ART_W + 2 + 14;
    const int gap = 9;

    /* Translucent, not a slab: the field keeps moving behind it. A solid black
     * strip at the bottom of a starfield reads as the panel ending early. */
    for (int y = y0 + 1; y < g_h; y++) {
        int by = y - g_band_y0;
        if (by < 0 || by >= g_band_h) continue;
        uint16_t *q = &g_fb[by * g_w];
        for (int x = 0; x < g_w; x++) q[x] = mix565(q[x], GB_RGB(2, 3, 12), 200);
    }
    fb_hspan(0, g_w - 1, y0, GB_RGB(44, 56, 80));

    int x = (g_w - (PU_COUNT * ew + (PU_COUNT - 1) * gap)) / 2;

    for (int t = 0; t < PU_COUNT; t++) {
        int n = s_have[t];

        const spal_t *pal;
        int npal;
        const char *const *art = pu_art(t, &pal, &npal);

        gb_sprite_t sp = {
            .rows = art, .pal = pal,
            .nrows = PU_ART_H, .ncols = PU_ART_W, .npal = (int16_t)npal,
            .scale = 1, .lean = 0,
            .tint_to = GB_RGB(6, 8, 18), .tint = (int16_t)(n ? 0 : 165),
        };
        fb_sprite(&sp, x, y0 + 2);

        char b[4];
        u32str(b, (uint32_t)n);
        fb_text_sh(x + PU_ART_W + 2, y0 + 3, b,
                   n ? PU_DEF[t].glow : GB_RGB(64, 74, 92), 2);

        /*
         * A level shows as a meter under the icon, not as a second number:
         * stock and potency are different things, and two digits side by side
         * would invite reading them as one.
         *
         * The meter is neutral rather than the powerup's own colour, and it
         * has a track behind it. Drawn in the glow it sat directly beneath the
         * missile's orange exhaust in almost the same orange, and read as part
         * of the icon rather than as a reading.
         */
        if (s_lvl[t]) {
            fb_box(x, g_h - 2, PU_ART_W, 2, GB_RGB(28, 34, 48));
            fb_box(x, g_h - 2, (PU_ART_W * s_lvl[t]) / PU_MAX_LEVEL, 2,
                   GB_RGB(228, 238, 255));
        }

        x += ew + gap;
    }
}

static void draw_hud(void)
{
    char buf[20];       /* label + two spaces + ten digits + NUL, with room */

    u32str(buf, s_dist);
    fb_text_sh(4, 11, buf, GB_RGB(236, 244, 255), 2);
    fb_text_sh(4, 4, "DIST", GB_RGB(120, 140, 165), 1);

    draw_statusbar();

    /* The OS takes the left button away at three seconds - escape and kill in
     * one gesture, and a guest cannot opt out of it. Saying so is better than
     * letting a run end for what looks like no reason. */
    if (s_hold_l > QUIT_WARN_MS) {
        int on = ((s_hold_l - QUIT_WARN_MS) / 90) & 1;
        fb_text_ctr(s_ship_y - 26, "RELEASE L",
                    on ? GB_RGB(255, 220, 90) : GB_RGB(170, 110, 30), 1);
    }

    if (s_pickup_flash > 0) {
        const char *tag = PU_DEF[s_pickup_type].tag;
        fb_text_ctr(s_ship_y - 22, tag, PU_DEF[s_pickup_type].glow, 2);
    }
}

/* ---- overlays ---- */

static void draw_title(void)
{
    char buf[20];
    int y = g_h / 6;

    fb_text_ctr(y, "ASTRO", GB_RGB(255, 255, 255), 5);
    fb_text_ctr(y + 34, "DEEP FIELD RUN", GB_RGB(130, 190, 230), 1);

    labelnum(buf, "HI", s_cfg.best);
    fb_text_ctr(y + 52, buf, GB_RGB(255, 214, 120), 2);

    /* The ship idles on the title screen so the controls have something to
     * point at, and so the flame is running before the run starts. */
    draw_ship();

    int py = g_h - 30;
    fb_text_ctr(py, "R  START", GB_RGB(120, 235, 160), 2);
    fb_text_ctr(py + 12, s_flip_ok ? "L FLIP   HOLD L EXIT" : "HOLD L  EXIT",
                GB_RGB(110, 125, 150), 1);
}

static void draw_over(void)
{
    char buf[20];
    int bx = 6, bw = g_w - 12;
    int by = g_h / 3, bh = 78;

    fb_box(bx, by, bw, bh, GB_RGB(10, 8, 22));
    fb_frame(bx, by, bw, bh, GB_RGB(90, 40, 50));

    fb_text_ctr(by + 8, "WRECKED", GB_RGB(255, 110, 90), 3);

    labelnum(buf, "DIST", s_dist);
    fb_text_ctr(by + 28, buf, GB_RGB(236, 244, 255), 2);

    labelnum(buf, "HI", s_cfg.best);
    fb_text_ctr(by + 42, buf, GB_RGB(255, 214, 120), 1);

    if (s_new_best)
        fb_text_ctr(by + 52, "NEW RECORD!", GB_RGB(255, 220, 110), 1);

    fb_text_ctr(by + 64, "R RETRY   L MENU", GB_RGB(120, 235, 160), 1);
}

static void render(void)
{
    for (int y = 0; y < g_h; y += BAND_H) {
        gfx_band(y, (y + BAND_H <= g_h) ? BAND_H : (g_h - y));

        bg_draw();
        draw_world();

        switch (s_state) {
        case ST_TITLE: draw_title(); break;
        case ST_OVER:  draw_hud(); draw_over(); break;
        default:       draw_hud(); break;
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
        saved.magic == SAVE_MAGIC && (saved.rot == 0 || saved.rot == 2))
        s_cfg = saved;

    /* Portrait, whatever the system orientation says, then read the panel back
     * rather than assuming what it gave us - the band buffer is sized for
     * either orientation, so a surprise here costs correctness nothing. */
    {
        gb_oscfg_t os;
        api->oscfg_get(&os);
        s_flip_ok = (os.rotation & 1) != 0;
        api->set_rotation(want_rotation());
    }
    resize();

    rnd_seed(api->millis() * 2654435761u + api->unix_time() + 1u);
    reset_run();
    s_state = ST_TITLE;

    uint32_t last = api->millis();

    while (!api->should_stop()) {
        uint32_t now = api->millis();
        uint32_t dt  = now - last;
        last = now;
        /* A long stall - the console writing to NVS, say - must not teleport
         * the world. Clamp rather than let one frame move everything. */
        if (dt > 80) dt = 80;

        if (!handle_input()) break;
        update(dt);
        render();

        /*
         * Hand back at least one tick every frame. The guest task sits at
         * priority 4 and the idle task at 0, and the OS is built with the
         * idle-task watchdog on: a loop that only ever busy-waited on the SPI
         * bus would trip it after five seconds.
         */
        uint32_t spent = api->millis() - now;
        api->sleep_ms(spent >= FRAME_MS - 10 ? 10 : FRAME_MS - spent);
    }

    api->log("exit");
    return 0;
}
