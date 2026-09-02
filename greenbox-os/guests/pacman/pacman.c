/*
 * pacman.c - PACMAN for greenbox: the arcade board, two buttons, and a camera
 * that decides how much of it you can see.
 *
 * ================================================================= controls
 *
 * Two buttons, and Pac-Man never stops moving, so a button cannot mean "go
 * this way" - there are four ways and two buttons. It means "turn":
 *
 *   L         turn left at the next place there is a left turn
 *   R         turn right at the next place there is a right turn
 *   L L       about face, now
 *   R R       the same
 *
 * and the one rule underneath all four: a press is an intention, not a
 * command about this instant.
 *
 * The first version of this made the press mean "turn here", measured against
 * the one tile Pac-Man was about to stand on, and it was unplayable. At seven
 * tiles a second a junction is open for about a tenth of a second, so a press
 * a moment early found no left turn available and did the only other thing it
 * could - it spun him round, away from the corner he was aiming at. The game
 * became an exercise in hitting a frame.
 *
 * So a press is remembered. It looks at the tile ahead, and if the turn is not
 * possible there it stays pending and looks again at every tile centre until
 * it finds one, up to four tiles later. Pressing early is not just forgiven,
 * it is the way the game is meant to be played: aim at the corner from down
 * the corridor and Pac-Man takes it when he gets there.
 *
 * That leaves the about-face, which cannot be buffered - a reversal you wanted
 * a second ago is a reversal into the ghost you were running from - and which
 * the arcade gives you for free with a joystick. Two presses the same way is
 * the answer: it happens immediately, wherever he is, because he is already
 * standing on the line he will travel back down. The two presses are not a
 * special case in the code either; the second press composes with the first
 * exactly as left-and-left-again composes into a half turn.
 *
 * A press the OTHER way cancels a pending one, so a mistimed press costs a
 * tap rather than a corner.
 *
 * When the buffered turn has nowhere to go at all - a straight corridor with
 * no opening within those four tiles, or a wall dead ahead - the press falls
 * back to the about-face immediately. That is the "or 180" in the rule: no way
 * round is a way back.
 *
 * Turning is read from api->buttons() as a press edge rather than from the tap
 * in the event queue. A tap is only known when the button comes UP, which is a
 * hundred milliseconds of nothing after you meant to turn. The menus here
 * still use events, because a menu wants gestures.
 *
 * =================================================================== camera
 *
 * The board is 28x31 tiles. The panel is 135x240. Drawn to fit, a tile is five
 * pixels across, Pac-Man is a two-pixel dot, and the game is unplayable in the
 * specific way where you cannot tell which dot you are.
 *
 * So the view is not fixed. Every frame the camera takes the bounding box of
 * Pac-Man and every ghost that is out on the board, adds a tile and a half of
 * air, and picks the zoom that fits that box in the viewport - clamped so it
 * never goes wider than the whole board and never closer than about six tiles
 * across. Chased into a corner by one ghost, you get a close view of the
 * corner. Spread out across the board, you get the board. Nothing that can
 * reach you is ever off screen, which is the one thing the player cannot be
 * asked to remember.
 *
 * "Out on the board" is doing real work in that sentence: ghosts waiting in
 * the house and eaten ghosts walking home as eyes are left out of the box.
 * Neither can touch you, both sit near the middle of the board, and counting
 * them holds the camera at its widest exactly when there is nothing to see -
 * see cam_targets().
 *
 * Two details make the difference between that and a camera that makes you
 * seasick:
 *
 *   Out is urgent, in is not. A ghost appearing at the far end of a corridor
 *   has to be on screen now, so the zoom follows outwards immediately and
 *   quickly. The room it leaves behind when it goes away is worth nothing, so
 *   zooming back in waits a third of a second to see whether the framing
 *   holds, and then takes its time. Without the wait, every ghost that ducks
 *   behind a wall block pumps the zoom.
 *
 *   Everything moves exponentially, not linearly. Each frame the camera closes
 *   a fixed fraction of the gap between where it is and where it wants to be,
 *   scaled by the frame time so a slow frame does not lurch. Position and zoom
 *   both, which is what makes a chase into a corner feel like one movement
 *   instead of a pan and a zoom that happen to overlap.
 *
 * The zoom floor is the whole board with three quarters of a tile of border,
 * computed from the viewport, so the camera never has to show more than the
 * board and never runs out of room to fit a sprite that is up against an edge.
 * Vertically the board is shorter than the viewport at that zoom, so the centre
 * is pinned; horizontally the board is a cylinder and there is no edge to pin
 * against - see pac_gfx.c for what that does to the tunnel.
 *
 * None of this is taken on trust. sim/harness.c projects every entity through
 * the real transform on every frame and reports the worst excursion past the
 * viewport in pixels; the number the code above is tuned to is zero.
 *
 * ============================================================== performance
 *
 * A frame is a full repaint through the OS rasteriser (api->gfx) into a
 * 16-row band buffer, so about 32,000 pixels of stores plus 65 KB down the SPI
 * bus. The maze is the expensive half - up to 900 tiles, each a filled box and
 * up to four edges - and it lands inside the 30 ms budget with room, because
 * only the tiles that intersect the current band are visited at all.
 */

#include "pacman.h"

/* ================================================================== tuning */

#define FRAME_MS        30

#define PAC_SPEED       1920        /* Q8 tiles/s: 7.5 */
#define GHOST_SPEED     1800
#define FRIGHT_SPEED    1150
#define EYES_SPEED      4200
#define TUNNEL_SPEED     980
#define LVL_SPEED_STEP    48        /* per level, capped */
#define LVL_SPEED_CAP    480

#define MOUTH_MAX         30        /* turn units; 256 to the circle */
#define MOUTH_PERIOD     190        /* ms for a full chomp */

#define READY_MS        1900
#define REREADY_MS      1200
#define DIE_MS          1500
#define CLEAR_MS        1700
#define EAT_FREEZE_MS    550
#define POP_MS           900

#define FRIGHT_BASE     6000
#define FRIGHT_FLASH    2000

#define DOT_SCORE         10
#define POWER_SCORE       50
#define EXTRA_LIFE     10000

#define COLLIDE_Q8       141        /* 0.55 tile, squared below */

/*
 * How many tile centres a pending turn may look at before it gives up, and
 * how far ahead the press looks to decide whether it is a turn at all.
 *
 * Four tiles is a little over half a second of travel. Long enough that
 * aiming at a corner from the far end of a short corridor works; short enough
 * that a press meant as "get me out of here" in a corridor with a side opening
 * three tiles away is still recognisable as a turn rather than as an escape
 * that never came.
 */
#define TURN_LOOKAHEAD     4

#define ZOOM_MAX      (22 * 256)    /* about six tiles across the panel */
#define CAM_PAD        (256 + 128)  /* air around the bounding box */
/*
 * The border left around the whole board at full zoom-out, in Q8 tiles.
 *
 * Fitting the board exactly is one pixel too greedy. Sprites have a floor on
 * their radius - three pixels, so that a ghost at the far zoom is still a
 * ghost - and at four and a half pixels to the tile that floor is bigger than
 * half a tile. A ghost in column 0 then hangs a couple of pixels past the edge
 * of a viewport that has nothing left to give: the zoom is already as far out
 * as it goes.
 *
 * The size of the slack is set by the worst case rather than by taste. On a
 * cylinder the furthest a ghost can be from Pac-Man is half a board in each
 * direction, so the widest box the camera is ever asked to frame is the full
 * 28 tiles - not 27 - plus a sprite radius at each end. A tile and an eighth
 * covers that with a little to spare; it costs about eight per cent of the
 * scale at the widest view, and buys an excursion count of zero rather than of
 * nearly zero.
 */
#define EDGE_PAD          288
#define K_PAN            1800       /* Q8 per second: about 7 */
#define K_ZOOM_OUT       2300
#define K_ZOOM_IN         700
#define ZOOM_IN_HOLD      330       /* ms the roomier framing must hold */

/* =================================================================== state */

typedef enum { ST_TITLE = 0, ST_READY, ST_PLAY, ST_DYING, ST_CLEAR, ST_OVER } state_t;

typedef struct {
    mover_t m;
    int     mouth;          /* turn units, 0 = shut */
    int     chomp;          /* ms into the chomp cycle */
} pac_t;

typedef struct {
    uint16_t magic;
    uint16_t rot;           /* 0 or 2 - only consulted in the landscape case */
    uint32_t best;
} save_t;

#define SAVE_MAGIC 0x9AC1u

static save_t   s_cfg = { SAVE_MAGIC, 0, 0 };
static int      s_flip_ok;
/* True when the panel is rotated 180 degrees from the way the buttons are
 * labelled, which is to say the board is being held the other way up. Only the
 * button hints care. */
static int      s_flipped;

static state_t  s_state;
static uint32_t s_state_ms;

static pac_t    s_pac;
static ghost_t  s_gh[NGHOST];

static uint32_t s_score;
static int      s_lives;
static int      s_level;
static int      s_extra_given;

static int32_t  s_mode_ms;          /* left in this scatter or chase */
static uint8_t  s_wave;             /* index into WAVE */
static uint8_t  s_scatter;
static int32_t  s_fright_ms;
static int      s_chain;            /* ghosts eaten on this energizer */

static int32_t  s_freeze_ms;        /* the pause when a ghost is eaten */
static int32_t  s_pop_ms;
static int32_t  s_pop_x, s_pop_y;
static uint32_t s_pop_val;

static int      s_life_dots;        /* dots eaten since the last reset */
static int32_t  s_idle_ms;          /* since the last dot: the release timer */

static int32_t  s_fruit_ms;         /* fruit on the board */
static int      s_fruit_kind;
static uint8_t  s_fruit_done;       /* how many have appeared this level */

static int32_t  s_blink_ms;
static uint8_t  s_btn_prev;

/* The arcade's wave table: scatter, chase, scatter, chase... and then chase
 * for as long as the level lasts. The lengths are why the first minute of a
 * level feels like a different game from the fourth. */
static const uint16_t WAVE[8] = { 7000, 20000, 7000, 20000, 5000, 20000, 5000, 0 };

/* Scatter corners, one per ghost: the four points they retreat to, which is
 * what breaks up a chase and lets you across the board. */
static const int8_t CORNER_X[NGHOST] = { 25,  2, 27,  0 };
static const int8_t CORNER_Y[NGHOST] = {  0,  0, 30, 30 };

/* How many dots must be eaten before each ghost is let out. Blinky is never in
 * the house to begin with; the others come out over the first half of a life,
 * which is what gives you a board to yourself for a few seconds after dying. */
static const int16_t HOUSE_LIMIT[NGHOST] = { 0, 0, 30, 60 };

static const int32_t HOUSE_SLOT[NGHOST] = { HOUSE_X, HOUSE_X, TQ(12), TQ(16) };

static const uint16_t FRUIT_VALUE[4] = { 100, 300, 500, 700 };

/* ==================================================================== util */

static void u32str(char *buf, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

static void save_cfg(void) { A->store_put("cfg", &s_cfg, sizeof s_cfg); }

static int wrap28(int tx)
{
    tx %= MAZE_W;
    return tx < 0 ? tx + MAZE_W : tx;
}

/* The copy of `v` that is nearest `ref` on a board that joins its own edges.
 * Everything that measures a distance in x has to go through this, including
 * the ghosts' idea of how far away Pac-Man is. */
static int32_t wrap_near(int32_t v, int32_t ref)
{
    int32_t d = v - ref;
    if (d >  TQ(MAZE_W) / 2) v -= TQ(MAZE_W);
    if (d < -TQ(MAZE_W) / 2) v += TQ(MAZE_W);
    return v;
}

static int32_t toward(int32_t v, int32_t t, int32_t d)
{
    if (v < t) { v += d; if (v > t) v = t; }
    else if (v > t) { v -= d; if (v < t) v = t; }
    return v;
}

/* One exponential step: close k*dt/1000 of the gap, clamped so a very long
 * frame lands on the target rather than overshooting past it. */
static int32_t approach(int32_t v, int32_t target, int32_t k, uint32_t dt)
{
    int32_t a = (int32_t)((k * (int32_t)dt) / 1000);
    if (a > 256) a = 256;
    return v + (((target - v) * a) >> 8);
}

/* ================================================================ movement */

typedef void (*decide_fn)(mover_t *m, void *ud);

static int blocked(const mover_t *m, int for_ghost)
{
    int tx = TILE_OF(m->x), ty = TILE_OF(m->y);
    return !maze_passable(tx + DX[m->dir], ty + DY[m->dir], for_ghost);
}

static void wrap_pos(mover_t *m)
{
    if (m->x < 0)              m->x += TQ(MAZE_W);
    if (m->x >= TQ(MAZE_W))    m->x -= TQ(MAZE_W);
}

/*
 * Walk `dist` Q8 tiles along the current heading, stopping at the centre of
 * every tile on the way to ask what to do next.
 *
 * The centre is where every decision in this game is made - it is the only
 * place a lane change is geometrically meaningful - so movement is not "add
 * the velocity and then look around": it advances to the next centre, decides
 * there, and spends what is left of the step on whatever heading came out of
 * that. Which is also what makes a corner feel right, because the leftover
 * distance turns the corner with you instead of being thrown away.
 *
 * The step is chopped at 96 Q8 (three eighths of a tile) so that a single
 * frame can never skip over a centre it should have stopped at, however long
 * the frame was.
 */
static void move_along(mover_t *m, int32_t dist, int for_ghost,
                       decide_fn decide, void *ud)
{
    if (m->stopped) {
        decide(m, ud);
        if (blocked(m, for_ghost)) return;
        m->stopped = 0;
    }

    while (dist > 0) {
        int32_t s = dist > 96 ? 96 : dist;
        dist -= s;

        int horiz = (m->dir & 1) == 0;
        int32_t *c = horiz ? &m->x : &m->y;
        int32_t sgn = (m->dir == D_R || m->dir == D_D) ? 1 : -1;
        int32_t centre = ((*c >> 8) << 8) + 128;
        int32_t nc = *c + sgn * s;

        /* Strictly before the centre, not at it. A mover that has just been
         * put down on a centre - or has just turned on one - has already had
         * its decision; treating that as another crossing would hand back the
         * whole step as leftover distance and the loop would never move. */
        int crossed = sgn > 0 ? (*c < centre && nc >= centre)
                              : (*c > centre && nc <= centre);
        if (!crossed) { *c = nc; wrap_pos(m); continue; }

        *c = centre;
        int32_t rest = sgn > 0 ? nc - centre : centre - nc;
        decide(m, ud);
        if (blocked(m, for_ghost)) { m->stopped = 1; return; }
        dist += rest;
    }
    wrap_pos(m);
}

/* ============================================================ Pac-Man turns */

/*
 * The tile whose centre is the next one Pac-Man will stand on. That is the
 * current tile if he has not passed its centre yet, and the next one along if
 * he has - and it is the tile every button press is measured against, because
 * it is the earliest place a turn could be taken.
 */
static void decision_tile(const mover_t *m, int *otx, int *oty)
{
    int tx = TILE_OF(m->x), ty = TILE_OF(m->y);
    if (!m->stopped) {
        int horiz = (m->dir & 1) == 0;
        int32_t c = horiz ? m->x : m->y;
        int32_t centre = ((c >> 8) << 8) + 128;
        int fwd = (m->dir == D_R || m->dir == D_D);
        if (fwd ? (c > centre) : (c < centre)) {
            tx += DX[m->dir];
            ty += DY[m->dir];
        }
    }
    *otx = wrap28(tx);
    *oty = ty;
}

/*
 * Is there anywhere to take this turn, walking forward from `tx,ty` along
 * `dir`? Returns how many tiles away the first opening is, or -1 if there is
 * none within the window - which includes running into a wall on the way,
 * because a turn that would need Pac-Man to pass through a wall to reach it is
 * not a turn he is going to reach.
 */
static int turn_ahead(int tx, int ty, int dir, int rot)
{
    int nd = (dir + rot + 4) & 3;
    for (int i = 0; i <= TURN_LOOKAHEAD; i++) {
        if (maze_passable(tx + DX[nd], ty + DY[nd], 0)) return i;
        if (!maze_passable(tx + DX[dir], ty + DY[dir], 0)) return -1;
        tx = wrap28(tx + DX[dir]);
        ty += DY[dir];
    }
    return -1;
}

static void pac_decide(mover_t *m, void *ud);

/* rot is -1 for the left button, +1 for the right one. */
static void turn_request(int rot)
{
    mover_t *m = &s_pac.m;

    /* A second press the same way, on top of one still waiting: that is the
     * about-face. Left and left again is a half turn whether it is taken as
     * two corners or as one spin, so this is composition rather than a rule of
     * its own - and it is the only thing here that happens immediately,
     * because he is already on the line he will travel back down. */
    if (m->want_rot == rot) {
        m->dir      = (uint8_t)((m->dir + 2) & 3);
        m->want_rot = 0;
        m->stopped  = 0;
        return;
    }

    /* A press the other way cancels the one waiting. A mistimed press should
     * cost a tap, not a corner. */
    if (m->want_rot == -rot) {
        m->want_rot = 0;
        return;
    }

    int tx, ty;
    decision_tile(m, &tx, &ty);

    if (turn_ahead(tx, ty, m->dir, rot) >= 0) {
        m->want_rot  = (int8_t)rot;
        m->want_left = TURN_LOOKAHEAD;
        /* If the press lands while he is exactly on a centre - which is where
         * a stopped mover always sits - take it now. Nothing else would: the
         * pending turn is tested when a centre is CROSSED, and he is not going
         * to cross this one again. */
        if ((m->x & 255) == 128 && (m->y & 255) == 128) pac_decide(m, 0);
        return;
    }

    /* Nowhere to turn within reach, so the press means the other thing it can
     * mean. This is the "or 180" of the rule, and it is what makes two buttons
     * enough to play with. */
    m->dir      = (uint8_t)((m->dir + 2) & 3);
    m->want_rot = 0;
    m->stopped  = 0;
}

/*
 * At a tile centre, with a turn pending: take it if this is a place it can be
 * taken, and otherwise spend one of the tiles it was given and carry on.
 *
 * The rotation is measured against the CURRENT heading every time it is
 * tested, which is what makes "left" keep meaning left after the corridor has
 * bent underneath it.
 */
static void pac_decide(mover_t *m, void *ud)
{
    (void)ud;
    if (!m->want_rot) return;

    int tx = TILE_OF(m->x), ty = TILE_OF(m->y);
    int nd = (m->dir + m->want_rot + 4) & 3;

    if (maze_passable(tx + DX[nd], ty + DY[nd], 0)) {
        m->dir      = (uint8_t)nd;
        m->want_rot = 0;
        return;
    }

    /* Standing still against a wall is not progress, so it does not cost the
     * pending turn anything: the mover is asked again every frame while it is
     * stopped, and four of those would be a twelfth of a second. */
    if (m->stopped) return;

    if (m->want_left) m->want_left--;
    else              m->want_rot = 0;
}

/*
 * What the L or R button would do if it were pressed this instant - the same
 * decision turn_request() makes, with nothing changed.
 *
 * It is worth the duplication of the conditions rather than the risk of the
 * two drifting apart: an arrow that lies about which way it will send you is
 * worse than no arrow, because it will be believed.
 */
static int press_outcome(int rot)
{
    const mover_t *m = &s_pac.m;

    if (m->want_rot == rot)  return (m->dir + 2) & 3;   /* second press: about face */
    if (m->want_rot == -rot) return m->dir;             /* cancels: carries straight on */

    int tx, ty;
    decision_tile(m, &tx, &ty);
    if (turn_ahead(tx, ty, m->dir, rot) >= 0) return (m->dir + rot + 4) & 3;
    return (m->dir + 2) & 3;
}

/* ================================================================== ghosts */

static int32_t lvl_speed(int32_t base)
{
    int32_t add = (int32_t)(s_level - 1) * LVL_SPEED_STEP;
    if (add > LVL_SPEED_CAP) add = LVL_SPEED_CAP;
    return base + add;
}

static int in_tunnel(const mover_t *m)
{
    int ty = TILE_OF(m->y);
    if (ty != 14) return 0;
    int tx = TILE_OF(m->x);
    return tx < 6 || tx > 21;
}

/*
 * Where each ghost is trying to get to, in tiles. This is the whole of the
 * personality: four targets, one shared rule for walking towards them, and
 * behaviour that looks like an ambush falls out of the arithmetic.
 */
static void ghost_target(const ghost_t *g, int *tx, int *ty)
{
    int px = TILE_OF(s_pac.m.x), py = TILE_OF(s_pac.m.y);
    int pd = s_pac.m.dir;

    if (g->st == GH_EYES || g->st == GH_ENTER) {   /* home, above the door */
        *tx = 13; *ty = 11;
        return;
    }
    if (s_scatter && !g->fright) {
        *tx = CORNER_X[g->kind]; *ty = CORNER_Y[g->kind];
        return;
    }

    switch (g->kind) {
    case 0:                                     /* blinky: straight at him */
        *tx = px; *ty = py;
        break;
    case 1:                                     /* pinky: four tiles ahead */
        *tx = px + 4 * DX[pd];
        *ty = py + 4 * DY[pd];
        break;
    case 2: {                                   /* inky: blinky, doubled */
        int ax = px + 2 * DX[pd], ay = py + 2 * DY[pd];
        int bx = TILE_OF(s_gh[0].m.x), by = TILE_OF(s_gh[0].m.y);
        *tx = ax + (ax - bx);
        *ty = ay + (ay - by);
        break;
    }
    default: {                                  /* clyde: shy up close */
        int dx = TILE_OF(g->m.x) - px, dy = TILE_OF(g->m.y) - py;
        if (dx * dx + dy * dy > 64) { *tx = px; *ty = py; }
        else { *tx = CORNER_X[g->kind]; *ty = CORNER_Y[g->kind]; }
        break;
    }
    }
}

/*
 * At every tile centre: of the ways out that are not the way in, take the one
 * whose next tile is nearest the target as the crow flies. Ties go up, then
 * left, then down, then right, which is the arcade's order and the reason two
 * ghosts on the same target do not walk in step forever.
 *
 * Frightened ghosts pick at random instead, which is why an energizer turns
 * the board from a problem into a scramble.
 */
static void ghost_decide(mover_t *m, void *ud)
{
    ghost_t *g = (ghost_t *)ud;
    static const uint8_t ORDER[4] = { D_U, D_L, D_D, D_R };

    int tx = TILE_OF(m->x), ty = TILE_OF(m->y);
    int back = (m->dir + 2) & 3;
    int door = (g->st == GH_EYES || g->st == GH_ENTER);

    int tgx, tgy;
    ghost_target(g, &tgx, &tgy);

    uint8_t legal[4];
    int nlegal = 0;
    int best = -1;
    int32_t bestd = 0x7FFFFFFF;

    for (int i = 0; i < 4; i++) {
        int d = ORDER[i];
        if (d == back) continue;
        int nx = tx + DX[d], ny = ty + DY[d];
        if (!maze_passable(nx, ny, door)) continue;

        legal[nlegal++] = (uint8_t)d;

        /* x distances are measured the short way round the cylinder, so a
         * ghost at column 1 chasing Pac-Man at column 26 goes through the
         * tunnel rather than all the way back across the board. */
        int32_t ddx = wrap_near(TQ(nx), TQ(tgx)) - TQ(tgx);
        int32_t ddy = TQ(ny - tgy);
        ddx >>= 8; ddy >>= 8;
        int32_t dist = ddx * ddx + ddy * ddy;
        if (dist < bestd) { bestd = dist; best = d; }
    }

    if (nlegal == 0) { m->dir = (uint8_t)back; return; }   /* only in a pocket */
    if (g->fright)   { m->dir = legal[gb_rnd() % (uint32_t)nlegal]; return; }
    m->dir = (uint8_t)(best < 0 ? back : best);
}

/* Out of the house: to the middle, then up through the door. Straight-line
 * movement, because inside the house there is no maze to consult. */
static void leave_house(ghost_t *g, int32_t dist)
{
    if (g->m.y > HOUSE_Y - 4 && (g->m.x != HOUSE_X)) {
        g->m.x = toward(g->m.x, HOUSE_X, dist);
        g->m.y = toward(g->m.y, HOUSE_Y, dist);
        g->m.dir = g->m.x < HOUSE_X ? D_R : D_L;
        return;
    }
    g->m.x   = HOUSE_X;
    g->m.dir = D_U;
    g->m.y   = toward(g->m.y, GATE_Y, dist);
    if (g->m.y == GATE_Y) {
        g->st      = GH_OUT;
        g->m.dir   = D_L;
        g->m.stopped = 0;
        g->m.want_rot = 0;
    }
}

/* Back in through the door, to the slot it will bob in until it is let out. */
static void enter_house(ghost_t *g, int32_t dist)
{
    g->m.dir = D_D;
    g->m.x = toward(g->m.x, HOUSE_X, dist);
    g->m.y = toward(g->m.y, HOUSE_Y, dist);
    if (g->m.y == HOUSE_Y && g->m.x == HOUSE_X) {
        g->st     = GH_HOUSE;
        g->fright = 0;
        g->bob    = 0;
        g->bob_dir = 1;
    }
}

static void ghost_update(ghost_t *g, uint32_t dt)
{
    int32_t base;

    switch (g->st) {
    case GH_HOUSE:
        /* Bobbing, so that a house full of ghosts does not look like a bug. */
        g->bob += g->bob_dir * (int32_t)dt / 6;
        if (g->bob >  90) { g->bob =  90; g->bob_dir = -1; }
        if (g->bob < -90) { g->bob = -90; g->bob_dir =  1; }
        g->m.x = HOUSE_SLOT[g->kind];
        g->m.y = HOUSE_Y + g->bob;
        g->m.dir = g->bob_dir > 0 ? D_D : D_U;
        return;

    case GH_LEAVING:
        leave_house(g, (lvl_speed(GHOST_SPEED) * (int32_t)dt) / 1000);
        return;

    case GH_ENTER:
        enter_house(g, (EYES_SPEED * (int32_t)dt) / 1000);
        return;

    case GH_EYES:
        base = EYES_SPEED;
        break;

    default:
        base = g->fright ? FRIGHT_SPEED : lvl_speed(GHOST_SPEED);
        if (in_tunnel(&g->m) && !g->fright) base = TUNNEL_SPEED;
        break;
    }

    move_along(&g->m, (base * (int32_t)dt) / 1000, 1, ghost_decide, g);

    if (g->st == GH_EYES) {
        int tx = TILE_OF(g->m.x), ty = TILE_OF(g->m.y);
        if (ty == 11 && (tx == 13 || tx == 14)) g->st = GH_ENTER;
    }
}

/* Everyone who is out and awake turns round. The arcade does this on every
 * mode change, and it is the tell that the wave has turned - the board stops
 * converging on you all at once. */
static void reverse_all(void)
{
    for (int i = 0; i < NGHOST; i++) {
        ghost_t *g = &s_gh[i];
        if (g->st != GH_OUT) continue;
        g->m.dir = (uint8_t)((g->m.dir + 2) & 3);
        g->m.stopped = 0;
    }
}

static void house_update(uint32_t dt)
{
    s_idle_ms += (int32_t)dt;

    for (int i = 0; i < NGHOST; i++)
        if (s_gh[i].st == GH_LEAVING) return;      /* one at a time */

    for (int i = 0; i < NGHOST; i++) {
        if (s_gh[i].st != GH_HOUSE) continue;
        if (s_life_dots >= HOUSE_LIMIT[i] || s_idle_ms > 4000) {
            s_gh[i].st = GH_LEAVING;
            s_idle_ms = 0;
        }
        return;                                    /* in order, never skipped */
    }
}

/* ================================================================== camera */

/* The bounding box cam_targets() last worked from, kept because the smoothing
 * that follows is allowed to lag it but not to lose it. */
static int32_t s_bb_x0, s_bb_x1, s_bb_y0, s_bb_y1;

static void cam_targets(void)
{
    int32_t px = s_pac.m.x;

    /* Follow Pac-Man across the seam: keeping the camera on his copy of the
     * board is what makes the tunnel continuous instead of a jump cut. */
    CAM.x = wrap_near(CAM.x, px);

    int32_t x0 = px, x1 = px, y0 = s_pac.m.y, y1 = s_pac.m.y;

    /*
     * Only the ghosts that are actually in the game.
     *
     * A ghost bobbing in the house is not something you can be caught by and
     * not something you have to watch, and framing it costs the whole board:
     * the house is in the middle, you start at the bottom, and counting the
     * three ghosts still waiting in it holds the camera at its widest for the
     * first ten seconds of every life - the ten seconds where there is nothing
     * to see. The same goes for a pair of eyes on its way home: it cannot
     * touch you, it is going somewhere you do not need to watch, and it does
     * it at sixteen tiles a second, so including it pulls the view out over
     * half the board at exactly the moment you are chasing the ghosts that are
     * still blue.
     *
     * So the rule is the ghosts that are out and whole. One comes out of the
     * house and the camera has it immediately - zooming out is the direction
     * that is allowed to be instant.
     *
     * While Pac-Man is dying the box is Pac-Man alone and the camera closes in
     * on him, which is the same rule again with the cast emptied.
     */
    if (s_state != ST_DYING) {
        for (int i = 0; i < NGHOST; i++) {
            if (s_gh[i].st != GH_OUT) continue;
            int32_t gx = wrap_near(s_gh[i].m.x, px);
            int32_t gy = s_gh[i].m.y;
            if (gx < x0) x0 = gx;
            if (gx > x1) x1 = gx;
            if (gy < y0) y0 = gy;
            if (gy > y1) y1 = gy;
        }
    }

    s_bb_x0 = x0; s_bb_x1 = x1; s_bb_y0 = y0; s_bb_y1 = y1;

    int32_t bw = (x1 - x0) + 2 * CAM_PAD;
    int32_t bh = (y1 - y0) + 2 * CAM_PAD;
    if (bw < 256) bw = 256;
    if (bh < 256) bh = 256;

    int32_t zx = ((int32_t)(VIEW_W - 2) << 16) / bw;
    int32_t zy = ((int32_t)(VIEW_H - 2) << 16) / bh;
    int32_t zf = zx < zy ? zx : zy;
    if (zf < CAM.zmin)  zf = CAM.zmin;
    if (zf > ZOOM_MAX)  zf = ZOOM_MAX;

    CAM.tx = (x0 + x1) / 2;
    CAM.ty = (y0 + y1) / 2;

    /* Vertically the board has edges, so do not show the void beyond them. At
     * full zoom-out the board is shorter than the viewport, and then the only
     * sensible centre is the middle of the board. */
    int32_t half = ((int32_t)(VIEW_H / 2 - 1) << 16) / CAM.z;
    if (2 * half >= TQ(MAZE_H)) {
        CAM.ty = TQ(MAZE_H) / 2;
    } else {
        if (CAM.ty < half) CAM.ty = half;
        if (CAM.ty > TQ(MAZE_H) - half) CAM.ty = TQ(MAZE_H) - half;
    }

    /* Out now, in later: see the file header. The dead band stops a ghost
     * jittering on the edge of the box from re-arming the wait forever. */
    int32_t dead = CAM.tz >> 5;
    if (zf < CAM.tz - dead) {
        CAM.tz = zf;
        CAM.hold_ms = 0;
    } else if (zf > CAM.tz + dead) {
        CAM.hold_ms += FRAME_MS;
        if (CAM.hold_ms >= ZOOM_IN_HOLD) CAM.tz = zf;
    } else {
        CAM.hold_ms = 0;
    }
}

/*
 * The promise, enforced.
 *
 * Everything above is smoothing, and smoothing lags: a ghost eaten at one
 * corner of the board comes home at sixteen tiles a second, and an exponential
 * pan that closes a fifth of the gap per frame is still a fifth of the gap
 * behind when it gets there. Measured over ten thousand frames, that was five
 * per cent of them with something up to thirty-five pixels outside the view -
 * which on this panel is a ghost you cannot see arriving.
 *
 * So the smoothing is allowed to be late, and then this refuses to let it be
 * wrong. The box has to fit: if the zoom is closer than it can be with
 * everyone inside, it is pulled back to exactly that; if the view has fallen
 * behind, it is dragged the minimum distance that puts the last of them back
 * on screen. Both are hard sets rather than another approach() - the whole
 * point is that the frame this runs on is already correct.
 *
 * It binds rarely and only for a frame or two, which is what makes it invisible
 * as motion and reliable as a guarantee. The margin is the sprite radius in
 * world units, so what fits is the ghost, not the ghost's centre.
 */
static void cam_contain(void)
{
    /*
     * Worked in pixels, and that is the point rather than a detail.
     *
     * The obvious way to write this is to convert the sprite radius into world
     * units, add it to the box and fit that - and it is wrong, because the
     * radius has a floor of three pixels. Below about seven pixels to the tile
     * the sprite stops shrinking with the zoom, so pulling the zoom out to fit
     * the box makes the sprite BIGGER in world units, and the fit that was
     * just computed no longer holds. It under-corrects by a pixel or two,
     * every time, in exactly the wide shots where the room ran out.
     *
     * Asking the question in pixels removes the circularity: how many pixels
     * are left for the box once the two half-sprites at the ends have had
     * theirs. Lowering the zoom can only make the radius smaller or leave it
     * at the floor, so a fit computed with today's radius is still a fit
     * tomorrow. One pass, no iteration, no fixed point to converge to.
     */
    int r = ent_radius();

    int aw = VIEW_W - 2 - 2 * r;
    int ah = VIEW_H - 2 - 2 * r;
    if (aw < 8) aw = 8;
    if (ah < 8) ah = 8;

    int32_t bw = s_bb_x1 - s_bb_x0;
    int32_t bh = s_bb_y1 - s_bb_y0;
    if (bw < 64) bw = 64;
    if (bh < 64) bh = 64;

    int32_t zx = ((int32_t)aw << 16) / bw;
    int32_t zy = ((int32_t)ah << 16) / bh;
    int32_t zneed = zx < zy ? zx : zy;
    if (zneed < CAM.zmin) zneed = CAM.zmin;
    if (CAM.z > zneed) CAM.z = zneed;

    /* The radius at the zoom that came out of that - never larger, so what
     * follows has at least as much room as the fit assumed. */
    r = ent_radius();

    /* From the centre pixel to the furthest an entity's CENTRE may sit: half
     * the viewport, less one for the centre pixel itself, less the radius that
     * has to fit beyond it. */
    int32_t hw = ((int32_t)(VIEW_W / 2 - 1 - r) << 16) / CAM.z;
    int32_t hh = ((int32_t)(VIEW_H / 2 - 1 - r) << 16) / CAM.z;
    if (hw < 0) hw = 0;
    if (hh < 0) hh = 0;

    if (s_bb_x1 > CAM.x + hw) CAM.x = s_bb_x1 - hw;
    if (s_bb_x0 < CAM.x - hw) CAM.x = s_bb_x0 + hw;
    if (s_bb_y1 > CAM.y + hh) CAM.y = s_bb_y1 - hh;
    if (s_bb_y0 < CAM.y - hh) CAM.y = s_bb_y0 + hh;
}

static void cam_update(uint32_t dt)
{
    cam_targets();

    int32_t kz = (CAM.tz < CAM.z) ? K_ZOOM_OUT : K_ZOOM_IN;
    CAM.z = approach(CAM.z, CAM.tz, kz, dt);
    CAM.x = approach(CAM.x, CAM.tx, K_PAN, dt);
    CAM.y = approach(CAM.y, CAM.ty, K_PAN, dt);

    cam_contain();
}

static void cam_snap(void)
{
    CAM.zmin = ((int32_t)(VIEW_W - 2) << 16) / (TQ(MAZE_W) + 2 * EDGE_PAD);
    int32_t zh = ((int32_t)(VIEW_H - 2) << 16) / (TQ(MAZE_H) + 2 * EDGE_PAD);
    if (zh < CAM.zmin) CAM.zmin = zh;

    CAM.x = s_pac.m.x;
    CAM.y = s_pac.m.y;
    CAM.z = CAM.tz = ZOOM_MAX;
    cam_targets();
    CAM.x = CAM.tx;
    CAM.y = CAM.ty;
    CAM.z = CAM.tz;
    CAM.hold_ms = 0;
    cam_contain();
}

/* ================================================================ the game */

static void reset_positions(void)
{
    s_pac.m.x = PAC_START_X;
    s_pac.m.y = PAC_START_Y;
    s_pac.m.dir = D_L;
    s_pac.m.stopped = 0;
    s_pac.m.want_rot = 0;
    s_pac.mouth = 0;
    s_pac.chomp = 0;

    for (int i = 0; i < NGHOST; i++) {
        ghost_t *g = &s_gh[i];
        g->kind = (uint8_t)i;
        g->fright = 0;
        g->bob = 0;
        g->bob_dir = (i & 1) ? 1 : -1;
        g->m.stopped = 0;
        g->m.want_rot = 0;
        g->m.x = HOUSE_SLOT[i];
        g->m.y = HOUSE_Y;
        g->m.dir = D_U;
        g->st = GH_HOUSE;
    }
    /* Blinky is never in the house at the start of a life; he is the reason
     * you have to move. */
    s_gh[0].st = GH_OUT;
    s_gh[0].m.x = HOUSE_X;
    s_gh[0].m.y = GATE_Y;
    s_gh[0].m.dir = D_L;

    s_wave = 0;
    s_scatter = 1;
    s_mode_ms = WAVE[0];
    s_fright_ms = 0;
    s_chain = 0;
    s_freeze_ms = 0;
    s_life_dots = 0;
    s_idle_ms = 0;

    cam_snap();
}

static void start_level(void)
{
    maze_reset();
    s_fruit_ms = 0;
    s_fruit_done = 0;
    s_fruit_kind = (s_level - 1) & 3;
    reset_positions();
    s_state = ST_READY;
    s_state_ms = READY_MS;
}

static void start_game(void)
{
    s_score = 0;
    s_lives = 3;
    s_level = 1;
    s_extra_given = 0;
    start_level();
}

/*
 * The high score is kept in RAM while a game is on and written to NVS when the
 * game ends - not when it changes.
 *
 * Writing on change is the obvious version and it is wrong twice over. Once
 * you are past the old best, every pellet is ten points and a new record, so
 * a commit lands twenty times a second: NVS is flash with a wear budget, and
 * a commit is guarded, which means it is a stall in the middle of a frame.
 * The value on screen is s_cfg.best either way; only the moment it is made
 * durable changes.
 */
static void add_score(uint32_t n)
{
    s_score += n;
    if (!s_extra_given && s_score >= EXTRA_LIFE) {
        s_extra_given = 1;
        s_lives++;
    }
    if (s_score > s_cfg.best) s_cfg.best = s_score;
}

static int32_t fright_len(void)
{
    int32_t ms = FRIGHT_BASE - (int32_t)(s_level - 1) * 500;
    return ms < 1000 ? 1000 : ms;
}

static void eat_dot(void)
{
    int tx = TILE_OF(s_pac.m.x), ty = TILE_OF(s_pac.m.y);
    uint8_t d = maze_dot(tx, ty);
    if (!d) return;

    maze_eat(tx, ty);
    s_life_dots++;
    s_idle_ms = 0;

    if (d == 1) {
        add_score(DOT_SCORE);
    } else {
        add_score(POWER_SCORE);
        s_fright_ms = fright_len();
        s_chain = 0;
        for (int i = 0; i < NGHOST; i++)
            if (s_gh[i].st == GH_OUT) { s_gh[i].fright = 1; }
        reverse_all();
    }

    int eaten = maze_dots_eaten();
    if ((eaten == 70 && s_fruit_done == 0) || (eaten == 170 && s_fruit_done == 1)) {
        s_fruit_done++;
        s_fruit_ms = 9000;
    }
}

static int touching(const mover_t *a, const mover_t *b)
{
    int32_t dx = wrap_near(a->x, b->x) - b->x;
    int32_t dy = a->y - b->y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > COLLIDE_Q8 || dy > COLLIDE_Q8) return 0;
    return (dx * dx + dy * dy) < (COLLIDE_Q8 * COLLIDE_Q8);
}

static void die(void)
{
    s_state = ST_DYING;
    s_state_ms = DIE_MS;
    s_pac.mouth = 0;
    s_fright_ms = 0;
    s_fruit_ms = 0;
}

static void check_ghosts(void)
{
    for (int i = 0; i < NGHOST; i++) {
        ghost_t *g = &s_gh[i];
        if (g->st != GH_OUT && g->st != GH_LEAVING) continue;
        if (!touching(&s_pac.m, &g->m)) continue;

        if (g->fright) {
            g->fright = 0;
            g->st = GH_EYES;
            g->m.stopped = 0;
            uint32_t val = 200u << (s_chain < 3 ? s_chain : 3);
            if (s_chain < 3) s_chain++;
            add_score(val);
            s_freeze_ms = EAT_FREEZE_MS;
            s_pop_ms = POP_MS;
            s_pop_x = g->m.x;
            s_pop_y = g->m.y;
            s_pop_val = val;
        } else {
            die();
        }
        return;
    }
}

static void update_play(uint32_t dt)
{
    /* The pause when a ghost is eaten: the board holds still and the score
     * hangs where the ghost was. Everything below is skipped, including the
     * camera's targets, so the view holds still too. */
    if (s_freeze_ms > 0) {
        s_freeze_ms -= (int32_t)dt;
        return;
    }

    /* Waves, unless an energizer has the board frightened - the arcade holds
     * the wave timer while blue, so the chase you interrupted resumes. */
    if (s_fright_ms > 0) {
        s_fright_ms -= (int32_t)dt;
        if (s_fright_ms <= 0) {
            s_fright_ms = 0;
            for (int i = 0; i < NGHOST; i++) s_gh[i].fright = 0;
        }
    } else if (WAVE[s_wave]) {
        s_mode_ms -= (int32_t)dt;
        if (s_mode_ms <= 0) {
            s_wave++;
            s_scatter = !s_scatter;
            s_mode_ms = WAVE[s_wave];
            reverse_all();
        }
    }

    int32_t d = (lvl_speed(PAC_SPEED) * (int32_t)dt) / 1000;
    move_along(&s_pac.m, d, 0, pac_decide, 0);

    /* The mouth is shut when he is not going anywhere, which is the only
     * feedback that says a corridor has run out. */
    if (s_pac.m.stopped) {
        s_pac.mouth = 0;
    } else {
        s_pac.chomp += (int32_t)dt;
        while (s_pac.chomp >= MOUTH_PERIOD) s_pac.chomp -= MOUTH_PERIOD;
        int half = MOUTH_PERIOD / 2;
        int t = s_pac.chomp < half ? s_pac.chomp : MOUTH_PERIOD - s_pac.chomp;
        s_pac.mouth = (t * MOUTH_MAX) / half;
    }

    eat_dot();
    check_ghosts();
    if (s_state != ST_PLAY) return;             /* died in there */

    house_update(dt);
    for (int i = 0; i < NGHOST; i++) ghost_update(&s_gh[i], dt);
    check_ghosts();

    if (s_fruit_ms > 0) {
        s_fruit_ms -= (int32_t)dt;
        if (s_fruit_ms < 0) s_fruit_ms = 0;
        if (s_fruit_ms > 0) {
            mover_t f = { FRUIT_X, FRUIT_Y, 0, 0, 0, 0 };
            if (touching(&s_pac.m, &f)) {
                uint32_t val = FRUIT_VALUE[s_fruit_kind & 3];
                add_score(val);
                s_fruit_ms = 0;
                s_pop_ms = POP_MS;
                s_pop_x = FRUIT_X;
                s_pop_y = FRUIT_Y;
                s_pop_val = val;
            }
        }
    }

    if (maze_dots_left() == 0) {
        s_state = ST_CLEAR;
        s_state_ms = CLEAR_MS;
    }
}

static void update(uint32_t dt)
{
    s_blink_ms += (int32_t)dt;
    if (s_blink_ms > 220) { s_blink_ms = 0; g_blink = !g_blink; }
    if (s_pop_ms > 0) s_pop_ms -= (int32_t)dt;

    switch (s_state) {
    case ST_TITLE:
        /* The title screen is drawn at a fixed zoom over the middle of the
         * board, so the camera has nothing to do but sit there. */
        break;

    case ST_READY:
        s_state_ms -= (int32_t)dt;
        if ((int32_t)s_state_ms <= 0) s_state = ST_PLAY;
        cam_update(dt);
        break;

    case ST_PLAY:
        update_play(dt);
        cam_update(dt);
        break;

    case ST_DYING:
        s_state_ms -= (int32_t)dt;
        /* One variable: the mouth opens from shut to all the way round. */
        s_pac.mouth = (int)(128 - (int32_t)s_state_ms * 128 / DIE_MS);
        if (s_pac.mouth > 128) s_pac.mouth = 128;
        if ((int32_t)s_state_ms <= 0) {
            if (--s_lives <= 0) {
                s_state = ST_OVER;
                s_state_ms = 4000;
                save_cfg();             /* the one place a record is made durable */
            } else {
                reset_positions();
                s_state = ST_READY;
                s_state_ms = REREADY_MS;
            }
        }
        cam_update(dt);
        break;

    case ST_CLEAR:
        s_state_ms -= (int32_t)dt;
        g_flash = ((s_state_ms / 200) & 1) != 0;
        if ((int32_t)s_state_ms <= 0) {
            g_flash = 0;
            s_level++;
            start_level();
        }
        cam_update(dt);
        break;

    case ST_OVER:
        s_state_ms -= (int32_t)dt;
        if ((int32_t)s_state_ms <= 0) s_state = ST_TITLE;
        break;
    }
}

/* =================================================================== input */

static uint8_t want_rotation(void)
{
    gb_oscfg_t os;
    A->oscfg_get(&os);
    if ((os.rotation & 1) == 0) return os.rotation;     /* already portrait */
    return (uint8_t)s_cfg.rot;
}

static void resize(void)
{
    s_flipped = (want_rotation() & 2) != 0;
    g_w = A->width();
    g_h = A->height();
    if (g_w > SCR_MAX_W) g_w = SCR_MAX_W;
    if (g_h > SCR_MAX_H) g_h = SCR_MAX_H;
    gfx_attach(A, g_fb, g_w, BAND_H);
    cam_snap();
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
                start_game();
            } else if (e == GB_EV_L_SHORT && s_flip_ok) {
                s_cfg.rot = (uint16_t)(s_cfg.rot == 0 ? 2 : 0);
                save_cfg();
                A->set_rotation((uint8_t)s_cfg.rot);
                resize();
            }
            break;

        case ST_OVER:
            if (e == GB_EV_R_SHORT || e == GB_EV_R_LONG) start_game();
            else if (e == GB_EV_L_SHORT) s_state = ST_TITLE;
            break;

        default:
            break;      /* play reads the buttons, not the gestures */
        }
    }

    /*
     * Turning, on the press rather than on the tap. See the file header: a tap
     * is not known until the button comes up, and a turn that arrives on
     * release arrives after the junction.
     */
    uint8_t b = A->buttons();
    uint8_t edge = (uint8_t)(b & ~s_btn_prev);
    s_btn_prev = b;

    if (s_state == ST_PLAY && s_freeze_ms <= 0) {
        if (edge & GB_BTN_L) turn_request(-1);
        if (edge & GB_BTN_R) turn_request(+1);
    }
    return 1;
}

/* =================================================================== HUD */

static void draw_hud(void)
{
    char buf[16];

    /* Top strip: what you have, and what the board has ever given anyone. */
    fb_box(0, 0, g_w, HUD_TOP, C_BG);
    u32str(buf, s_score);
    pac_text(3, 2, buf, C_TEXT, 2);

    u32str(buf, s_cfg.best);
    int w = fb_text_w(buf, 1);
    pac_text(g_w - 3 - w, 4, buf, C_DIM, 1);
    pac_text(g_w - 3 - w - fb_text_w("HI ", 1), 4, "HI", C_DIM, 1);

    /* Bottom strip: lives, and which level they are being spent on. */
    int y = g_h - HUD_BOT;
    fb_box(0, y, g_w, HUD_BOT, C_BG);
    for (int i = 0; i < s_lives - 1 && i < 5; i++)
        draw_life_icon(6 + i * 10, y + 5, 3);

    buf[0] = 'L'; u32str(buf + 1, (uint32_t)s_level);
    w = fb_text_w(buf, 1);
    pac_text(g_w - 3 - w, y + 3, buf, C_DIM, 1);
}

static void draw_popup(void)
{
    if (s_pop_ms <= 0) return;
    char buf[12];
    u32str(buf, s_pop_val);
    gfx_text_ctrx(cam_sx(s_pop_x), cam_sy(s_pop_y) - 3, buf, C_SCORE, 1, C_SHADOW);
}

static void draw_overlay(void)
{
    int cy = VIEW_CY;

    switch (s_state) {
    case ST_READY:
        pac_text_ctr(cy - 4, "READY!", C_PAC, 2);
        break;
    case ST_OVER:
        pac_text_ctr(cy - 4, "GAME OVER", GB_RGB(255, 60, 60), 2);
        break;
    default:
        break;
    }
}

/*
 * The title screen. It is drawn over the board at whatever the camera was
 * left at, because a still of the maze behind the letters says more about the
 * program than a black rectangle does.
 */
static void draw_title(void)
{
    char buf[16];
    int cy = VIEW_CY;

    fb_box(0, cy - 42, g_w, 84, GB_RGB(0, 0, 12));
    fb_hspan(0, g_w - 1, cy - 42, C_EDGE);
    fb_hspan(0, g_w - 1, cy + 41, C_EDGE);

    pac_text_ctr(cy - 36, "PACMAN", C_PAC, 3);

    /* The controls are the program, so they go on the front - and the second
     * line is the one that matters, because a player who does not know the
     * press is remembered will play it like a reflex test. */
    pac_text_ctr(cy - 16, "L LEFT     R RIGHT", C_TEXT, 1);
    pac_text_ctr(cy - 6,  "PRESS EARLY - IT WAITS", C_PAC, 1);
    pac_text_ctr(cy + 4,  "SAME TWICE TO", C_DIM, 1);
    pac_text_ctr(cy + 13, "TURN AROUND", C_DIM, 1);

    pac_text_ctr(cy + 28, s_flip_ok ? "R START   L FLIP" : "R START", C_PAC, 1);

    u32str(buf, s_cfg.best);
    pac_text_ctr(cy + 50, "HIGH", C_DIM, 1);
    pac_text_ctr(cy + 60, buf, C_TEXT, 2);
}

/* ================================================================== render */

static void render(void)
{
    for (int y = 0; y < g_h; y += BAND_H) {
        int bh = (y + BAND_H <= g_h) ? BAND_H : (g_h - y);
        gfx_band(y, bh);

        cam_unclip();
        fb_box(0, y, g_w, bh, C_BG);

        cam_clip();
        draw_maze();

        if (s_fruit_ms > 0) draw_fruit(FRUIT_X, FRUIT_Y, s_fruit_kind);

        if (s_state != ST_DYING) {
            int flash = 0;
            if (s_fright_ms > 0 && s_fright_ms < FRIGHT_FLASH)
                flash = ((s_fright_ms / 180) & 1) == 0;
            for (int i = 0; i < NGHOST; i++) draw_ghost(&s_gh[i], flash);
        }

        if (s_state != ST_OVER)
            draw_pac(s_pac.m.x, s_pac.m.y, s_pac.m.dir, s_pac.mouth, C_PAC);

        draw_popup();

        if (s_state == ST_PLAY || s_state == ST_READY)
            draw_hints(press_outcome(-1), press_outcome(+1),
                       s_pac.m.want_rot, s_flipped);

        cam_unclip();
        if (s_state == ST_TITLE) draw_title();
        else                     draw_overlay();
        draw_hud();

        A->blit(0, (int16_t)y, g_w, (int16_t)bh, g_fb);
    }
}

/* ==================================================================== main */

int gb_main(const gb_api_t *api)
{
    A = api;

    save_t saved;
    if (api->store_get("cfg", &saved, sizeof saved) == (int)sizeof saved &&
        saved.magic == SAVE_MAGIC && (saved.rot == 0 || saved.rot == 2))
        s_cfg = saved;

    /* Portrait, whatever the system orientation says - 28 tiles of board on
     * the short axis of a landscape panel is four pixels a tile before the
     * camera has decided anything. If the board is already portrait that is
     * the one used, so the user's choice of which way up survives. */
    {
        gb_oscfg_t os;
        api->oscfg_get(&os);
        s_flip_ok = (os.rotation & 1) != 0;
        api->set_rotation(want_rotation());
    }

    /* resize() first: it reads the panel back and hands the band buffer to
     * api->gfx, and everything after it - the camera above all - is measured
     * against a viewport that has to exist by then. */
    resize();

    maze_reset();
    s_level = 1;
    s_lives = 3;
    reset_positions();

    gb_rnd_seed(api->millis() * 2654435761u + api->unix_time() + 7u);

    s_state = ST_TITLE;
    s_btn_prev = api->buttons();

    uint32_t last = api->millis();

    while (!api->should_stop()) {
        uint32_t now = api->millis();
        uint32_t dt = now - last;
        last = now;
        /* A long stall - the OS committing a high score to NVS, say - must not
         * teleport four ghosts through a wall. */
        if (dt > 80) dt = 80;

        if (!handle_input()) break;
        update(dt);
        render();

        /* Hand back at least one tick every frame: the guest task runs at
         * priority 4 with the idle task at 0, and the OS builds with the
         * idle-task watchdog on. */
        uint32_t spent = api->millis() - now;
        api->sleep_ms(spent >= FRAME_MS - 10 ? 10 : FRAME_MS - spent);
    }

    /* A run abandoned with the left hold still counts: the OS gives a guest a
     * few hundred milliseconds to return from gb_main, which is time enough
     * for one commit. */
    save_cfg();

    api->log("exit");
    return 0;
}
