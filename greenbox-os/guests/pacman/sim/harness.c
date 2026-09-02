/*
 * harness.c - run the pacman guest on the host, with a stub OS underneath.
 *
 * Three things about this program cannot be judged on the board, and all three
 * are why it exists.
 *
 *   Whether the two-button turn rule can actually drive Pac-Man everywhere.
 *   The auto player below is a breadth-first search to the nearest pellet that
 *   steers with nothing but the same two presses a person has - including the
 *   double press for an about-face - so if it clears a board, the control
 *   scheme reaches every tile of it. On the panel that question takes a
 *   patient human twenty minutes and still only proves one route.
 *
 *   Whether the camera keeps its promise. Every frame, every entity is
 *   projected through the real transform and checked against the viewport;
 *   what comes out is the worst excursion in pixels over thousands of frames,
 *   which is a number rather than an impression. A ghost that slid off the top
 *   for four frames during a chase is invisible on a matchbox screen and
 *   obvious here.
 *
 *   Whether anything ever ends up inside a wall. Half a tile of drift at seven
 *   tiles a second looks exactly like a ghost that was always there.
 *
 * It includes pacman.c directly rather than linking it, so the checks can read
 * the cast's own coordinates, and it compiles the OS's rasteriser (osgfx.c)
 * rather than a stand-in, so the frames it dumps are the pixels the panel
 * would get.
 *
 *   gcc -O1 -std=gnu11 -Wall -Wextra -I../../../abi -I../../gsdk \
 *       -I../../../os/main -I.. harness.c ../maze.c ../pac_gfx.c \
 *       ../../gsdk/gb_gfx.c ../../../os/main/osgfx.c -o sim
 *
 *   ./sim 6000                  6000 frames, report at the end
 *   ./sim 6000 400 800 40       the same, dumping every 40th frame as a PPM
 *   EASY=1 ./sim 20000          ghosts parked: how long a clean sweep takes
 *   WHY=1   ./sim 4000          detail on any frame that fails a check
 *   TRACE=1 ./sim 600           a line per frame
 *
 * Frames land in frames/, which has to exist.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "greenbox_abi.h"
#include "osgfx.h"

/* ---- panel ---- */
static uint16_t FB[240 * 240];
static int PW = 135, PH = 240;

static int16_t h_width(void)  { return (int16_t)PW; }
static int16_t h_height(void) { return (int16_t)PH; }
static void h_fill(uint16_t c) { for (int i = 0; i < PW * PH; i++) FB[i] = c; }
static void h_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int px = x + i, py = y + j;
            if (px >= 0 && px < PW && py >= 0 && py < PH) FB[py * PW + px] = c;
        }
}
static void h_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c)
{ h_fill_rect(x, y, w, 1, c); h_fill_rect(x, y + h - 1, w, 1, c);
  h_fill_rect(x, y, 1, h, c); h_fill_rect(x + w - 1, y, 1, h, c); }
static void h_pixel(int16_t x, int16_t y, uint16_t c) { h_fill_rect(x, y, 1, 1, c); }
static void h_blit(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int dx = x + i, dy = y + j;
            if (dx >= 0 && dx < PW && dy >= 0 && dy < PH)
                FB[dy * PW + dx] = px[j * w + i];
        }
}
static int16_t h_text(int16_t x, int16_t y, const char *s, uint16_t fg,
                      uint16_t bg, uint8_t size)
{ (void)y; (void)fg; (void)bg; return x + (int16_t)(strlen(s) * 6 * size); }
static int16_t h_text_width(const char *s, uint8_t size)
{ return (int16_t)(strlen(s) * 6 * size); }
static void h_backlight(bool on) { (void)on; }
static void h_set_rotation(uint8_t r)
{ if (r & 1) { PW = 240; PH = 135; } else { PW = 135; PH = 240; } }

/* ---- input: a small copy of the OS gesture rules ---- */
static uint32_t T;
static uint8_t  BTN, prev_btn;
static uint32_t down_at[2];
static int      fired[2];
static gb_event_t queue[16];
static int qh, qt;

static void qpush(gb_event_t e) { queue[qt] = e; qt = (qt + 1) & 15; }

static void input_poll(void)
{
    for (int i = 0; i < 2; i++) {
        uint8_t m = i ? GB_BTN_R : GB_BTN_L;
        int now = (BTN & m) != 0, was = (prev_btn & m) != 0;
        if (now && !was) { down_at[i] = T; fired[i] = 0; }
        if (now && !fired[i]) {
            uint32_t hold = T - down_at[i];
            if (i == 1 && hold >= 1000) { qpush(GB_EV_R_LONG); fired[i] = 1; }
            if (i == 0 && hold >= 3000) { qpush(GB_EV_L_LONG); fired[i] = 1; }
        }
        if (!now && was && !fired[i])
            qpush(i ? GB_EV_R_SHORT : GB_EV_L_SHORT);
    }
    prev_btn = BTN;
}

static gb_event_t h_poll_event(void)
{
    if (qh == qt) return GB_EV_NONE;
    gb_event_t e = queue[qh];
    qh = (qh + 1) & 15;
    return e;
}
static gb_event_t h_wait_event(uint32_t ms) { (void)ms; return h_poll_event(); }
static uint8_t h_buttons(void) { return BTN; }

/* ---- the rest of the table ---- */
static uint32_t frames;
static int      max_frames = 4000;
static int      dump_from = -1, dump_to = -1, dump_every = 1;
static int      trace, easy, why;

static uint32_t h_millis(void) { return T; }
static void h_get_time(gb_tm_t *o) { memset(o, 0, sizeof *o); }
static uint32_t h_unix_time(void) { return 1700000000u; }
static void h_sleep_ms(uint32_t ms);
static bool h_set_time(const gb_tm_t *t) { (void)t; return true; }
static bool h_should_stop(void) { return frames >= (uint32_t)max_frames; }
static void h_log(const char *m) { printf("[guest] %s\n", m); }
static int  h_snprintf(char *b, size_t n, const char *f, ...)
{ va_list a; va_start(a, f); int r = vsnprintf(b, n, f, a); va_end(a); return r; }
static void *h_alloc(size_t n) { return malloc(n); }
static void  h_free(void *p) { free(p); }
static int   h_store_get(const char *k, void *b, size_t n)
{ (void)k; (void)b; (void)n; return -1; }
static int   h_store_put(const char *k, const void *b, size_t n)
{ (void)k; (void)b; return (int)n; }
static void  h_oscfg_get(gb_oscfg_t *o) { o->rotation = 0; o->theme = 0; }
static bool  h_oscfg_set(const gb_oscfg_t *i) { (void)i; return true; }
static uint8_t h_theme_count(void) { return 1; }
static bool  h_theme_get(uint8_t i, gb_theme_t *o)
{ if (i) return false; memset(o, 0, sizeof *o); return true; }
static bool h_wifi_power(bool on) { (void)on; return false; }
static int  h_wifi_scan(gb_ap_t *o, int m, uint8_t c, uint16_t d)
{ (void)o; (void)m; (void)c; (void)d; return -1; }
static bool h_wifi_watch(const uint8_t *b, uint8_t c) { (void)b; (void)c; return false; }
static int  h_wifi_watch_poll(gb_hit_t *o, int m) { (void)o; (void)m; return -1; }

static const gb_api_t API = {
    .abi_version = GB_ABI_VERSION,
    .width = h_width, .height = h_height, .fill = h_fill,
    .fill_rect = h_fill_rect, .rect = h_rect, .pixel = h_pixel,
    .blit = h_blit, .text = h_text, .text_width = h_text_width,
    .backlight = h_backlight, .set_rotation = h_set_rotation,
    .poll_event = h_poll_event, .wait_event = h_wait_event,
    .buttons = h_buttons,
    .millis = h_millis, .get_time = h_get_time, .unix_time = h_unix_time,
    .sleep_ms = h_sleep_ms, .set_time = h_set_time,
    .should_stop = h_should_stop, .log = h_log, .snprintf = h_snprintf,
    .alloc = h_alloc, .free = h_free,
    .store_get = h_store_get, .store_put = h_store_put,
    .oscfg_get = h_oscfg_get, .oscfg_set = h_oscfg_set,
    .theme_count = h_theme_count, .theme_get = h_theme_get,
    .wifi_power = h_wifi_power, .wifi_scan = h_wifi_scan,
    .wifi_watch = h_wifi_watch, .wifi_watch_poll = h_wifi_watch_poll,
    .gfx = &g_gb_gfx,
};

/* The guest itself, statics and all. */
#include "pacman.c"

/* ======================================================= the auto player */
/*
 * Breadth-first search from the tile Pac-Man is about to stand on to the
 * nearest thing worth eating, and then the only question this program is
 * allowed to answer: which of the two buttons gets him facing that way.
 *
 * It never sets a direction. It presses L or R, exactly as a thumb would, and
 * an about-face is two presses of the same button in one frame - which works
 * only because a second press is measured against the first. If that rule were
 * wrong this player would walk into walls instead of turning round, and the
 * frames-to-clear number below would never arrive.
 */
static int bfs_step(int sx, int sy)
{
    static int16_t from[MAZE_H][MAZE_W];
    static int16_t qx[MAZE_H * MAZE_W], qy[MAZE_H * MAZE_W];
    int head = 0, tail = 0;

    for (int y = 0; y < MAZE_H; y++)
        for (int x = 0; x < MAZE_W; x++) from[y][x] = -1;

    from[sy][sx] = 4;                       /* 4 marks the root */
    qx[tail] = (int16_t)sx; qy[tail] = (int16_t)sy; tail++;

    while (head < tail) {
        int x = qx[head], y = qy[head]; head++;
        if (maze_dot(x, y)) {
            int d = from[y][x];
            while (d != 4) {                /* walk back; the first move wins */
                int px = wrap28(x - DX[d]), py = y - DY[d];
                if (from[py][px] == 4) return d;
                x = px; y = py; d = from[y][x];
            }
            return -1;
        }
        for (int d = 0; d < 4; d++) {
            int nx = wrap28(x + DX[d]), ny = y + DY[d];
            if (ny < 0 || ny >= MAZE_H) continue;
            if (!maze_passable(nx, ny, 0)) continue;
            if (from[ny][nx] != -1) continue;
            from[ny][nx] = (int16_t)d;
            qx[tail] = (int16_t)nx; qy[tail] = (int16_t)ny; tail++;
        }
    }
    return -1;
}

static int last_press_tile = -1;

static void auto_play(void)
{
    if (s_state != ST_PLAY || s_freeze_ms > 0) return;

    int tx, ty;
    decision_tile(&s_pac.m, &tx, &ty);

    /* One decision per tile: a thumb does not press eight times crossing one
     * square, and pressing again before the queued turn has been taken would
     * compose with it rather than replace it. */
    int key = ty * MAZE_W + tx;
    if (key == last_press_tile) return;

    int want = bfs_step(tx, ty);
    if (want < 0) return;

    int cur = s_pac.m.dir;
    int delta = (want - cur + 4) & 3;
    if (delta == 0) return;

    last_press_tile = key;
    if (delta == 1)      BTN |= GB_BTN_R;           /* a right turn */
    else if (delta == 3) BTN |= GB_BTN_L;           /* a left one */
    else {
        /* About face: the same button twice. The first press may already have
         * done it - a corridor with nothing to turn into reverses on one - so
         * the second only goes in if he is still pointing the old way. */
        int before = s_pac.m.dir;
        turn_request(-1);
        if (s_pac.m.dir == before) turn_request(-1);
    }
}

/* ====================================================== checks and output */

static int      off_screen_frames, worst_off;
static int      in_wall_frames;
static int      deaths, clears;
static uint32_t best_score;
static int      clear_frame = -1;

static void dump_frame(void)
{
    char name[64];
    snprintf(name, sizeof name, "frames/f%05u.ppm", frames);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", PW, PH);
    for (int i = 0; i < PW * PH; i++) {
        uint16_t c = FB[i];
        unsigned char rgb[3] = {
            (unsigned char)(((c >> 11) & 31) * 255 / 31),
            (unsigned char)(((c >> 5) & 63) * 255 / 63),
            (unsigned char)((c & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* Is (wx,wy) inside the viewport, through the real transform? */
static int on_screen(int32_t wx, int32_t wy, int *excess)
{
    int x = cam_sx(wx), y = cam_sy(wy), r = ent_radius();
    int e = 0, t;
    if (x - r < 0)      e = -(x - r);
    if (x + r > PW - 1) { t = (x + r) - (PW - 1); if (t > e) e = t; }
    if (y - r < VIEW_Y0) { t = VIEW_Y0 - (y - r); if (t > e) e = t; }
    if (y + r > VIEW_Y0 + VIEW_H - 1)
        { t = (y + r) - (VIEW_Y0 + VIEW_H - 1); if (t > e) e = t; }
    *excess = e;
    return e == 0;
}

static void checks(void)
{
    if (s_state != ST_PLAY) return;

    /* Nothing may ever be standing in a wall. */
    int bad = !maze_passable(TILE_OF(s_pac.m.x), TILE_OF(s_pac.m.y), 0);
    for (int i = 0; i < NGHOST; i++)
        if (!maze_passable(TILE_OF(s_gh[i].m.x), TILE_OF(s_gh[i].m.y), 1)) bad = 1;
    if (bad) {
        if (in_wall_frames < 4)
            printf("IN WALL frame %u  pac tile %d,%d\n", frames,
                   TILE_OF(s_pac.m.x), TILE_OF(s_pac.m.y));
        in_wall_frames++;
    }

    /* The camera's whole promise, measured. */
    int worst = 0, e, who = -1;
    if (!on_screen(s_pac.m.x, s_pac.m.y, &e) && e > worst) { worst = e; who = 9; }
    /* The same set the camera frames: a ghost in the house or a pair of eyes
     * on its way home is allowed off screen, because neither is in the game. */
    for (int i = 0; i < NGHOST; i++) {
        if (s_gh[i].st != GH_OUT) continue;
        if (!on_screen(s_gh[i].m.x, s_gh[i].m.y, &e) && e > worst) { worst = e; who = i; }
    }
    if (worst) {
        off_screen_frames++;
        if (worst > worst_off) worst_off = worst;
        if (why && off_screen_frames < 12) {
            int gs = (who >= 0 && who < NGHOST) ? (int)s_gh[who].st : -1;
            int32_t wx = who == 9 ? s_pac.m.x : s_gh[who].m.x;
            int32_t wy = who == 9 ? s_pac.m.y : s_gh[who].m.y;
            printf("OFF f%u who %d st%d by %dpx  z %d  cam %d,%d"
                   "  ent -> px %d,%d r%d  bb x[%d,%d] y[%d,%d]\n",
                   frames, who, gs, worst, (int)CAM.z, (int)CAM.x, (int)CAM.y,
                   cam_sx(wx), cam_sy(wy), ent_radius(),
                   (int)s_bb_x0, (int)s_bb_x1, (int)s_bb_y0, (int)s_bb_y1);
        }
    }
}

static void h_sleep_ms(uint32_t ms)
{
    static state_t prev_state = ST_TITLE;
    static int prev_lives = 3;

    checks();

    if (s_state == ST_CLEAR && prev_state != ST_CLEAR) {
        clears++;
        if (clear_frame < 0) clear_frame = (int)frames;
    }
    if (s_lives < prev_lives) deaths++;
    if (s_score > best_score) best_score = s_score;
    prev_state = s_state;
    prev_lives = s_lives;

    if (trace)
        printf("T %5u st %d pac %3d,%3d d%d z %4d dots %3d score %6u\n",
               frames, s_state, TILE_OF(s_pac.m.x), TILE_OF(s_pac.m.y),
               s_pac.m.dir, (int)CAM.z, maze_dots_left(), (unsigned)s_score);

    if (dump_from >= 0 && (int)frames >= dump_from && (int)frames <= dump_to &&
        ((int)frames - dump_from) % dump_every == 0)
        dump_frame();

    T += ms;
    frames++;

    /* Ghosts parked in the house, for the question "can this control scheme
     * eat every pellet on the board, and how long does it take". */
    if (easy)
        for (int i = 0; i < NGHOST; i++) {
            s_gh[i].st = GH_HOUSE;
            s_gh[i].fright = 0;
        }

    BTN = 0;
    if (s_state == ST_TITLE || s_state == ST_OVER) {
        if ((frames % 20) < 6) BTN = GB_BTN_R;      /* start, or start again */
    } else {
        auto_play();
    }
    input_poll();
}

/* ================================================== the press-early bench */
/*
 * The regression test for the thing that made the first version unplayable.
 *
 * Row 20 is a long corridor; the only way up out of it between columns 1 and 6
 * is at column 6. So: put Pac-Man on that row heading right, press the left
 * button from a series of distances before the corner, and print what he
 * actually did. What should come out is "took the corner" for every distance
 * inside the four-tile window and an immediate about-face outside it - never
 * an about-face inside it, which is what the old code did with any press that
 * was not on the junction tile itself.
 */
static void bench_turn(void)
{
    printf("press distance before the corner at (6,20), heading right:\n");

    for (int q8 = 0; q8 <= 5 * 256; q8 += 64) {
        memset(&s_pac, 0, sizeof s_pac);
        s_pac.m.x   = CENTRE_OF(6) - q8;
        s_pac.m.y   = CENTRE_OF(20);
        s_pac.m.dir = D_R;
        s_level     = 1;
        maze_reset();

        int dir0 = s_pac.m.dir;
        turn_request(-1);                       /* the left button, this early */

        /* The press may have been answered before he moved at all: on a tile
         * centre the turn is taken there and then, and standing on one is
         * exactly the case the zero-distance row tests. */
        int turned_at = (s_pac.m.dir != dir0) ? TILE_OF(s_pac.m.x) : -1;
        int reversed  = (s_pac.m.dir == ((dir0 + 2) & 3));

        for (int f = 0; f < 40 && turned_at < 0 && !reversed; f++) {
            int before = s_pac.m.dir;
            move_along(&s_pac.m, (PAC_SPEED * 30) / 1000, 0, pac_decide, 0);
            if (s_pac.m.dir != before) {
                turned_at = TILE_OF(s_pac.m.x);
                reversed = (s_pac.m.dir == ((dir0 + 2) & 3));
            }
        }

        printf("  %4d/256 tile (%d.%02d): ", q8, q8 / 256, (q8 % 256) * 100 / 256);
        if (reversed)            printf("about face on the spot\n");
        else if (turned_at == 6) printf("took the corner at column 6\n");
        else if (turned_at >= 0) printf("turned at column %d\n", turned_at);
        else                     printf("NOTHING HAPPENED\n");
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) max_frames = atoi(argv[1]);
    if (argc > 4) { dump_from = atoi(argv[2]); dump_to = atoi(argv[3]);
                    dump_every = atoi(argv[4]); }
    trace = getenv("TRACE") != NULL;
    easy  = getenv("EASY") != NULL;
    why   = getenv("WHY") != NULL;

    if (getenv("TURNBENCH")) {
        /* The bench drives the mover directly, so the panel is never touched
         * and gb_main is never entered. */
        g_w = 135; g_h = 240;
        bench_turn();
        return 0;
    }

    gb_main(&API);

    printf("\n--- %u frames (%u s of play) ---\n", frames, T / 1000);
    printf("score %u   deaths %d   levels cleared %d%s\n",
           (unsigned)best_score, deaths, clears,
           clear_frame >= 0 ? "" : "  (none)");
    if (clear_frame >= 0)
        printf("first clear at frame %d (%d s)\n", clear_frame, clear_frame * 30 / 1000);
    printf("in-wall frames   %d\n", in_wall_frames);
    printf("off-screen       %d frames, worst %d px outside the viewport\n",
           off_screen_frames, worst_off);
    return (in_wall_frames || worst_off > 0) ? 1 : 0;
}
