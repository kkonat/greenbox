/*
 * harness.c - run the pinball guest on the host, with a stub OS underneath.
 *
 * There is no way to see a physics bug on the board. The ball is seven pixels
 * across on a panel the size of a matchbox, a wall it passes through looks
 * exactly like a wall it bounced off, and the only instrument is a serial log
 * that cannot keep up with a 30 ms frame. So the guest is compiled for the
 * host as well, against the stub gb_api_t below, and driven by a script that
 * plunges and flips: the table then runs ten minutes of play in a second and
 * says whether the ball ever left it.
 *
 * That is not a stand-in for the panel - the colours and the feel still have
 * to be judged on the board - but it is what caught the launch geometry being
 * wrong the first time, where the divider tip sloped uphill and every plunge
 * dropped back into the lane.
 *
 * This includes pinball.c directly rather than linking it, so the checks can
 * see the ball's own coordinates: from outside, a ball off the table and a
 * ball behind the backdrop look the same.
 *
 *   gcc -O1 -std=gnu11 -Wall -Wextra -I../../../abi -I../../gsdk -I..  *       harness.c ../pin_gfx.c ../cast.c -o sim
 *   ./sim 20000                 20000 frames, report escapes at the end
 *   ./sim 1200 0 1200 30        the same, dumping every 30th frame as a PPM
 *   TRACE=1 ./sim 3000          one line per frame: state, position, velocity
 *
 * Frames land in frames/, which has to exist. They are PPM because writing one
 * is eight lines and reading one is a one-liner in anything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "greenbox_abi.h"
#include "osgfx.h"

/* ---- panel ---- */
static uint16_t FB[240 * 135];
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
{ (void)y; (void)s; (void)fg; (void)bg; return x + (int16_t)(strlen(s) * 6 * size); }
static int16_t h_text_width(const char *s, uint8_t size)
{ return (int16_t)(strlen(s) * 6 * size); }
static void h_backlight(bool on) { (void)on; }
static uint8_t h_rot = 0;
static void h_set_rotation(uint8_t r)
{
    h_rot = r;
    if (r & 1) { PW = 240; PH = 135; } else { PW = 135; PH = 240; }
}

/* ---- input: a tiny copy of the OS gesture rules ---- */
static uint32_t T;              /* virtual ms */
static uint8_t  BTN;            /* what the script holds down */
static uint8_t  prev_btn;
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

/* ---- time ---- */
static uint32_t h_millis(void) { return T; }
static void h_get_time(gb_tm_t *o) { memset(o, 0, sizeof *o); }
static uint32_t h_unix_time(void) { return 1700000000u; }
static uint32_t frames;
static int      dump_from = -1, dump_to = -1, dump_every = 1;
static int      escapes;
static int      trace;
static int      max_frames = 4000;
static void h_sleep_ms(uint32_t ms);
static bool h_set_time(const gb_tm_t *t) { (void)t; return true; }

/* ---- misc ---- */
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
    /* The real rasteriser, so the frames this dumps are the pixels the
     * panel would get. */
    .gfx = &g_gb_gfx,
};

/* The guest itself, statics and all. */
#include "pinball.c"

/* ---- the script, and the checks ---- */
static void dump_frame(void)
{
    char name[64];
    snprintf(name, sizeof name, "frames/f%04u.ppm", frames);
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

static unsigned rng = 12345;
static unsigned rr(void) { rng = rng * 1103515245u + 12345u; return rng >> 16; }

static void h_sleep_ms(uint32_t ms)
{
    /* End of a frame: check the ball, dump if asked, then drive the script. */
    int bx = s_bx >> 8, by = s_by >> 8;
    if (s_state == ST_PLAY) {
        if (bx < 3 || bx > 132 || by < TOP_H || by > 244) {
            if (escapes < 8)
                printf("ESCAPE frame %u  state %d  ball (%d,%d) v (%d,%d)\n",
                       frames, s_state, bx, by, s_vx >> 8, s_vy >> 8);
            escapes++;
        }
    }
    if (trace) printf("T %u %d %d %d %d %d\n", frames, s_state, bx, by, (int)(s_vx >> 8), (int)(s_vy >> 8));
    if (dump_from >= 0 && (int)frames >= dump_from && (int)frames <= dump_to &&
        ((int)frames - dump_from) % dump_every == 0)
        dump_frame();

    T += ms;
    frames++;

    /* script: start the game, plunge, then flip at random */
    if (frames < 20)         BTN = 0;
    else if (frames < 60)    BTN = GB_BTN_R;        /* R_LONG starts the game */
    else if (frames < 70)    BTN = 0;
    else if (frames < 130)   BTN = GB_BTN_R;        /* then a full plunge */
    else {
        if ((rr() % 11) == 0) BTN ^= GB_BTN_L;
        if ((rr() % 11) == 0) BTN ^= GB_BTN_R;
        /* never hold left long enough to trip the OS escape */
        if ((BTN & GB_BTN_L) && (rr() % 7) == 0) BTN &= (uint8_t)~GB_BTN_L;
    }
    input_poll();
}


/* ---- flipper bench -------------------------------------------------------
 *
 * Cradle the ball at a series of points along a flipper, from the base to the
 * tip, let it settle, then flip and watch where it goes. A real table throws
 * the ball harder and flatter the nearer the tip it is caught; anything that
 * sends it DOWN, or barely moves it, is a bug in the contact.
 */
static void bench(void)
{
    static const char *SIDE[2] = { "L", "R" };

    for (int side = 0; side < 2; side++) {
        for (int d = 8; d <= 30; d += 2) {
            int a   = FLIP_REST;
            int sgn = side ? -1 : 1;
            int px  = side ? PIVOT_RX : PIVOT_LX;

            /* a point d along the bar, then out along its upper normal by the
             * ball's radius plus the bar's - where a resting ball sits */
            int cx = px + sgn * ((d * icos(a)) >> 8);
            int cy = PIVOT_Y + ((d * isin(a)) >> 8);
            int nx = sgn * isin(a), ny = -icos(a);          /* Q8 unit */

            s_state = ST_PLAY;
            s_save_ms = 0;
            s_mult = 1;
            s_flip_a[0] = s_flip_a[1] = FLIP_REST << 8;
            s_flip_w[0] = s_flip_w[1] = 0;
            s_flip_on[0] = s_flip_on[1] = 0;
            s_bx = (cx << 8) + nx * (BALL_R + FLIP_R);
            s_by = (cy << 8) + ny * (BALL_R + FLIP_R);
            s_vx = s_vy = 0;

            /* settle */
            for (int i = 0; i < 6; i++) update(30);
            int rx = s_bx >> 8, ry = s_by >> 8;

            /* flip */
            s_flip_on[side] = 1;
            int miny = 9999, endx = 0, endy = 0, drained = 0;
            int vx0 = 0, vy0 = 0, got = 0;
            for (int i = 0; i < 70; i++) {
                update(30);
                if (!got && (s_vx | s_vy)) {
                    /* the velocity one frame after contact is the shot */
                    vx0 = s_vx >> 8; vy0 = s_vy >> 8; got = 1;
                }
                if ((s_by >> 8) < miny) miny = s_by >> 8;
                if (s_state != ST_PLAY) { drained = 1; break; }
                if (i == 3) s_flip_on[side] = 0;
            }
            endx = s_bx >> 8; endy = s_by >> 8;

            printf("%s d=%2d  rest(%3d,%3d)  shot v=(%5d,%5d)  peak y=%3d  "
                   "end(%3d,%3d)%s\n",
                   SIDE[side], d, rx, ry, vx0, vy0, miny, endx, endy,
                   drained ? "  DRAINED" : "");
        }
    }
}

/* ---- roll-off bench ------------------------------------------------------
 *
 * The other half of the question: a ball put down mid-flipper rolls towards
 * the tip, and what matters is how long it stays catchable. This drops one at
 * d=10 and flips after a growing delay, so the last delay that still launches
 * it is the width of the window the player actually has.
 */
static void bench_roll(void)
{
    printf("delay(ms)  d_at_press  shot v          peak y  result\n");
    for (int delay = 0; delay <= 1200; delay += 100) {
        int a = FLIP_REST, d0 = 10;
        int cx = PIVOT_LX + ((d0 * icos(a)) >> 8);
        int cy = PIVOT_Y  + ((d0 * isin(a)) >> 8);

        s_state = ST_PLAY; s_save_ms = 0; s_mult = 1;
        s_flip_a[0] = s_flip_a[1] = FLIP_REST << 8;
        s_flip_w[0] = s_flip_w[1] = 0;
        s_flip_on[0] = s_flip_on[1] = 0;
        s_bx = (cx << 8) + isin(a) * (BALL_R + FLIP_R);
        s_by = (cy << 8) - icos(a) * (BALL_R + FLIP_R);
        s_vx = s_vy = 0;

        int t = 0, gone = 0;
        while (t < delay) { update(30); t += 30; if (s_state != ST_PLAY) { gone = 1; break; } }
        if (gone) { printf("%6d     (gone before the press)\n", delay); continue; }

        int dxp = (s_bx >> 8) - PIVOT_LX, dyp = (s_by >> 8) - PIVOT_Y;
        int dnow = (int)isqrt32((uint32_t)(dxp * dxp + dyp * dyp));

        s_flip_on[0] = 1;
        int miny = 9999, vx0 = 0, vy0 = 0, got = 0, drained = 0;
        for (int i = 0; i < 70; i++) {
            update(30);
            if (!got && ((s_vy >> 8) < -60)) { vx0 = s_vx >> 8; vy0 = s_vy >> 8; got = 1; }
            if ((s_by >> 8) < miny) miny = s_by >> 8;
            if (s_state != ST_PLAY) { drained = 1; break; }
            if (i == 3) s_flip_on[0] = 0;
        }
        printf("%6d %10d      (%5d,%5d) %5d   %s\n",
               delay, dnow, vx0, vy0, miny, drained ? "drained" : "in play");
    }
}

/* How long the ball spends in the part of the flipper worth shooting from:
 * from d=18, where the shot first gets real power, to d=25, where it runs out
 * of bar. That span is the window a player actually aims for. */
static void bench_window(void)
{
    int a = FLIP_REST, d0 = 10;
    int cx = PIVOT_LX + ((d0 * icos(a)) >> 8);
    int cy = PIVOT_Y  + ((d0 * isin(a)) >> 8);
    s_state = ST_PLAY; s_save_ms = 0; s_mult = 1;
    s_flip_a[0] = s_flip_a[1] = FLIP_REST << 8;
    s_flip_w[0] = s_flip_w[1] = 0;
    s_flip_on[0] = s_flip_on[1] = 0;
    s_bx = (cx << 8) + isin(a) * (BALL_R + FLIP_R);
    s_by = (cy << 8) - icos(a) * (BALL_R + FLIP_R);
    s_vx = s_vy = 0;

    int t = 0, t18 = -1, t25 = -1;
    while (t < 3000 && s_state == ST_PLAY) {
        update(10);
        t += 10;
        int dx = (s_bx >> 8) - PIVOT_LX, dy = (s_by >> 8) - PIVOT_Y;
        int d = (int)isqrt32((uint32_t)(dx * dx + dy * dy));
        if (t18 < 0 && d >= 18) t18 = t;
        if (t25 < 0 && d >= 25) { t25 = t; break; }
    }
    printf("window: reaches d=18 at %d ms, d=25 at %d ms -> %d ms in the zone\n",
           t18, t25, (t18 >= 0 && t25 >= 0) ? t25 - t18 : -1);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        A = &API;
        h_set_rotation(0);
        g_w = 135; g_h = 240;
        rnd_seed(1);
        s_flip_a[0] = s_flip_a[1] = FLIP_REST << 8;
        bench();
        printf("\n");
        bench_roll();
        bench_window();
        return 0;
    }

    if (argc > 1) max_frames = atoi(argv[1]);
    if (argc > 3) { dump_from = atoi(argv[2]); dump_to = atoi(argv[3]); }
    if (argc > 4) dump_every = atoi(argv[4]);
    if (getenv("TRACE")) trace = 1;

    gb_main(&API);

    printf("frames %u  escapes %d  score %u  ball %d  state %d\n",
           frames, escapes, (unsigned)s_score, s_ball, s_state);
    return escapes ? 1 : 0;
}
