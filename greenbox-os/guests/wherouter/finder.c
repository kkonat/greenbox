/*
 * finder.c - the hunt: one BSSID, the whole panel.
 *
 * The list screen answers "what is out there". This one answers the only
 * question that gets you across a room: is it stronger than it was two steps
 * ago. Everything on screen is in service of that.
 *
 *   L tap    change how much time the trace spans
 *   R tap    forget the best and worst marks and start the trace again
 *   R hold   back to the list
 *   L hold   the OS escape
 *
 * The measurement is promiscuous mode parked on the target's channel, so what
 * arrives is every frame that BSSID transmits - about ten beacons a second on
 * its own, more if anybody is using the network. That is the reason this is
 * a watch and not a repeated scan: a scan of one channel costs a beacon
 * interval to produce one number, and a watch produces every number the AP
 * emits. Ten samples a second is what makes a gradient visible while walking
 * rather than after stopping.
 *
 * Two averages run over those samples, and the difference between them is the
 * whole instrument. The fast one has about half a second of memory and is what
 * the big number shows; the slow one has about three seconds and is what the
 * fast one is compared against. Walk towards the router and the fast average
 * climbs out of the slow one; stop, and they close up again within a few
 * paces' worth of time. Nothing here has to know how fast you are walking or
 * which way you are facing - the two time constants do that work.
 */

#include "wherouter.h"

/* Faster than this and the badge flickers between hotter and colder on the
 * scatter between two beacons; slower and it lags a step behind the feet. */
#define TREND_DB16   24         /* 1.5 dB, in the dBm x16 the averages use */

#define POLL_MS     100
#define LOST_MS    2500u        /* nothing heard: the number stops meaning */
#define HUNT_MS    5000u        /* nothing heard: go and look for it */

static uint8_t f_chan;
static char    f_name[34];

static int16_t f_fast, f_slow;  /* dBm x16 */
static int8_t  f_best, f_worst;
static bool    f_seen;

static uint32_t f_last_hit;
static uint16_t f_rate, f_count;
static uint32_t f_rate_t0;

static bool    f_hunting;
static uint8_t f_hunt_chan;

/* ------------------------------------------------------------- geometry */

static int16_t big_x, big_y;
static uint8_t big_size;
static int16_t unit_x, unit_y;
static int16_t badge_x, badge_y, badge_w, badge_h;
static int16_t dist_x, dist_y;
static int16_t bar_x, bar_y, bar_w, bar_h, tick_y;
static int16_t tr_x, tr_y, tr_w, tr_h;

/*
 * Two shapes, one set of variables. The wide one puts the number and the
 * badge side by side because there is room for them; the tall one stacks
 * them and spends what it saves on the trace, which is the panel this
 * orientation is actually better at - a minute of walking reads more clearly
 * across 118 rows than across 41.
 */
static void span_labels(void);

static void layout(void)
{
    bool wide = (W >= 200);

    big_size = wide ? 4 : 3;
    big_x    = 6;
    big_y    = TITLE_H + 6;

    int16_t bigw = (int16_t)(3 * 6 * big_size);
    int16_t bigh = (int16_t)(8 * big_size);

    unit_x = (int16_t)(big_x + bigw + 4);
    unit_y = (int16_t)(big_y + bigh - 8);

    if (wide) {
        badge_x = (int16_t)(unit_x + 24);
        badge_w = (int16_t)(W - badge_x - 6);
        badge_y = big_y;
        badge_h = 22;
        dist_x  = badge_x;
        dist_y  = (int16_t)(badge_y + badge_h + 6);
    } else {
        badge_x = 6;
        badge_w = (int16_t)(W - 12);
        badge_y = (int16_t)(big_y + bigh + 6);
        badge_h = 20;
        dist_x  = 6;
        dist_y  = (int16_t)(badge_y + badge_h + 5);
    }

    bar_x  = 6;
    bar_w  = (int16_t)(W - 12);
    bar_h  = 8;
    bar_y  = (int16_t)(dist_y + 14);
    tick_y = (int16_t)(bar_y + bar_h + 1);

    tr_x = 6;
    tr_w = bar_w;
    tr_y = (int16_t)(tick_y + 7);
    tr_h = (int16_t)(H - FOOT_H - 2 - tr_y);
    if (tr_h < 12) tr_h = 12;

    span_labels();
}

/* ---------------------------------------------------------------- trace */
/*
 * The trace does not scroll. It wraps, with a two-column gap running ahead of
 * the write head, the way a heart monitor does.
 *
 * Scrolling would mean redrawing every column of the trace on every sample,
 * because this panel cannot be read back - there is no MISO on this board, so
 * "shift the picture left by one" is not an operation, only "draw the whole
 * thing again one pixel over". At ten samples a second across two hundred
 * columns that is the entire frame budget spent on moving pixels sideways.
 * Wrapping costs three narrow writes: erase the head, plot the sample, erase
 * the gap. The price is that the oldest data is to the right of the newest
 * rather than off the end, which takes one glance to learn and no explaining
 * after that.
 *
 * A column is a slice of time rather than one sample, and what it draws is the
 * range - highest and lowest - over that slice, as a vertical stroke. A single
 * dot per column would show the scatter between beacons as noise; the stroke
 * shows it as thickness, which is information: a thick trace is a signal
 * fighting something, a thin one is a clean path.
 */
static const uint16_t SPAN_MS[3] = { 150, 600, 2400 };  /* one column is this long */
static char     s_span_label[3][8];

static uint8_t  s_span;         /* index into SPAN_MS */
static int16_t  t_col;
static uint32_t t_t0;           /* when the current column started */
static int8_t   t_min, t_max;
static bool     t_any;

/*
 * A column is a duration, not a count of samples, and the labels are worked
 * out from the width the orientation left rather than written down. Two
 * hundred and twenty-eight columns of landscape hold nearly twice what a
 * hundred and twenty-three of portrait do, and a footer that claimed
 * otherwise would be wrong on one of them.
 */
static void span_labels(void)
{
    for (int i = 0; i < 3; i++) {
        unsigned secs = ((unsigned)tr_w * SPAN_MS[i]) / 1000u;
        if (secs < 100) A->snprintf(s_span_label[i], sizeof s_span_label[i], "%us", secs);
        else            A->snprintf(s_span_label[i], sizeof s_span_label[i], "%um", (secs + 30) / 60);
    }
}

static int16_t trace_y(int dbm)
{
    return (int16_t)(tr_y + tr_h - 1 - sig_bar(dbm, (int16_t)(tr_h - 1)));
}

/* The grid is redrawn a column at a time as the head passes, because the head
 * has just erased it. Every third column, so it reads as a rule rather than a
 * line the trace has to be picked out of. */
static void trace_grid_col(int16_t col)
{
    static const int8_t LEVEL[3] = { -50, -70, -85 };
    if (col % 3) return;
    for (int i = 0; i < 3; i++)
        A->fill_rect((int16_t)(tr_x + col), trace_y(LEVEL[i]), 1, 1, T.muted);
}

static void trace_clear(void)
{
    A->fill_rect(tr_x, tr_y, tr_w, tr_h, T.bg);
    for (int16_t c = 0; c < tr_w; c++) trace_grid_col(c);
    t_col = 0;
    t_t0  = A->millis();
    t_any = false;
}

static void trace_commit(bool lost)
{
    A->fill_rect((int16_t)(tr_x + t_col), tr_y, 1, tr_h, T.bg);
    trace_grid_col(t_col);

    if (lost) {
        /* Marked at the floor rather than left blank: walking a flat with this
         * thing, the holes are the point of the exercise. */
        A->fill_rect((int16_t)(tr_x + t_col), (int16_t)(tr_y + tr_h - 1), 1, 1, T.warn);
    } else if (t_any) {
        int16_t y0 = trace_y(t_max);            /* strongest sits highest */
        int16_t y1 = trace_y(t_min);
        A->fill_rect((int16_t)(tr_x + t_col), y0, 1, (int16_t)(y1 - y0 + 1),
                     sig_colour((t_min + t_max) / 2));
    } else {
        /* No frame inside this column, but the target has not gone. At the
         * fastest span a column is shorter than the gap between two beacons,
         * so an empty one is the ordinary case, and drawing it as a dropout
         * would put a dead zone in every other pixel. The running average
         * carries the line across. */
        A->fill_rect((int16_t)(tr_x + t_col), trace_y(f_fast / 16), 1, 1,
                     sig_colour(f_fast / 16));
    }

    t_col = (int16_t)((t_col + 1) % tr_w);
    t_any = false;

    /* The gap ahead of the head, so which end is now is never in doubt. */
    for (int i = 0; i < 2; i++)
        A->fill_rect((int16_t)(tr_x + (t_col + i) % tr_w), tr_y, 1, tr_h, T.bg);
}

/* Every frame heard goes in; the column closes on the clock. */
static void trace_sample(int8_t dbm)
{
    if (!t_any) { t_min = t_max = dbm; t_any = true; }
    else { if (dbm < t_min) t_min = dbm; if (dbm > t_max) t_max = dbm; }
}

static void trace_step(uint32_t now, bool lost)
{
    /* A loop rather than an if, because a hunt step parks the program for a
     * dwell and can leave several columns owing. */
    while (now - t_t0 >= SPAN_MS[s_span]) {
        t_t0 += SPAN_MS[s_span];
        trace_commit(lost);
    }
}

/* --------------------------------------------------------------- drawing */

static void draw_title(void)
{
    char cut[34], right[10];

    A->fill_rect(0, 0, W, TITLE_H, T.surface);

    A->snprintf(right, sizeof right, f_hunting ? "hunt c%u" : "ch %u",
                f_hunting ? f_hunt_chan : f_chan);
    int16_t rw = A->text_width(right, 1);
    A->text((int16_t)(W - rw - 4), 3, right,
            f_hunting ? T.warn : T.dim, T.surface, 1);

    fit_text(cut, sizeof cut, f_name, (int16_t)(W - rw - 12), 1);
    A->text(4, 3, cut, T.accent, T.surface, 1);
}

static void draw_number(bool lost)
{
    char buf[6];
    int dbm = f_fast / 16;

    if (lost) A->snprintf(buf, sizeof buf, "---");
    else      A->snprintf(buf, sizeof buf, "%3d", dbm);

    /* Three characters always - the range that reaches this program is -20 to
     * -99 - so the field never has to be cleared before it is written. */
    A->text(big_x, big_y, buf, lost ? T.muted : sig_colour(dbm), T.bg, big_size);
    A->text(unit_x, unit_y, "dBm", T.muted, T.bg, 1);
}

static void draw_badge(bool lost)
{
    const char *word;
    uint16_t colour, ink;

    if (lost) {
        word = "NO SIGNAL"; colour = T.surface; ink = T.warn;
    } else {
        int16_t d = (int16_t)(f_fast - f_slow);
        if (d > TREND_DB16)       { word = "HOTTER"; colour = GB_RGB(255, 116,  36); ink = GB_BLACK; }
        else if (d < -TREND_DB16) { word = "COLDER"; colour = GB_RGB( 56, 136, 240); ink = GB_BLACK; }
        else                      { word = "STEADY"; colour = T.surface;             ink = T.dim;   }
    }

    /* Size 2 where it fits, which is both orientations for six characters and
     * neither for nine, so "NO SIGNAL" drops to size 1 rather than off the
     * edge. */
    uint8_t size = (A->text_width(word, 2) <= badge_w - 8) ? 2 : 1;
    int16_t tw = A->text_width(word, size);
    int16_t th = (int16_t)(8 * size);

    A->fill_rect(badge_x, badge_y, badge_w, badge_h, colour);
    A->text((int16_t)(badge_x + (badge_w - tw) / 2),
            (int16_t)(badge_y + (badge_h - th) / 2), word, ink, colour, size);
}

static void draw_distance(bool lost)
{
    char buf[24];

    A->fill_rect(dist_x, dist_y, (int16_t)(W - dist_x - 6), 8, T.bg);
    if (lost) return;

    uint16_t dm = sig_dist_dm(f_fast / 16);
    if (dm < 100) A->snprintf(buf, sizeof buf, "~%u.%u m est", dm / 10, dm % 10);
    else          A->snprintf(buf, sizeof buf, "~%u m est", dm / 10);
    A->text(dist_x, dist_y, buf, T.muted, T.bg, 1);
}

static void draw_bar(bool lost)
{
    int dbm = f_fast / 16;
    int16_t len = lost ? 0 : sig_bar(dbm, bar_w);

    if (len) A->fill_rect(bar_x, bar_y, len, bar_h, sig_colour(dbm));
    if (len < bar_w)
        A->fill_rect((int16_t)(bar_x + len), bar_y, (int16_t)(bar_w - len),
                     bar_h, T.surface);

    /* The marks are the reason the bar is worth its eight rows: the live
     * length says where you are, and the two ticks say where the best and the
     * worst of this hunt were, which is what tells you whether the last minute
     * of walking was progress. */
    A->fill_rect(bar_x, tick_y, bar_w, 3, T.bg);
    if (f_seen) {
        A->fill_rect((int16_t)(bar_x + sig_bar(f_worst, bar_w) - 1), tick_y, 2, 3, T.muted);
        A->fill_rect((int16_t)(bar_x + sig_bar(f_best,  bar_w) - 1), tick_y, 2, 3, T.accent);
    }
}

static void draw_footer(void)
{
    static const char *const cand[] = {
        "L span   R reset   R-hold list",
        "L span  R reset  R-hold list",
        "L:span R:reset RH:list",
        "L:span R:rst RH:list",
    };
    char right[14];

    A->fill_rect(0, (int16_t)(H - FOOT_H), W, FOOT_H, T.bg);

    A->snprintf(right, sizeof right, "%s %u/s", s_span_label[s_span], f_rate);
    int16_t rw = A->text_width(right, 1);
    A->text((int16_t)(W - rw - 4), (int16_t)(H - FOOT_H + 3), right, T.dim, T.bg, 1);

    A->text(4, (int16_t)(H - FOOT_H + 3),
            pick_fit(cand, (int)(sizeof cand / sizeof cand[0]), (int16_t)(W - rw - 12)),
            T.muted, T.bg, 1);
}

/* Everything that does not change between frames. The trace is not here: it is
 * cleared by reset_marks(), which is the one thing that has to happen both on
 * entry and on an R tap. */
static void draw_static(void)
{
    A->fill(T.bg);
    draw_title();
    draw_footer();
}

/* ----------------------------------------------------------- the samples */

static void reset_marks(void)
{
    f_seen  = false;
    f_best  = RSSI_COLD;
    f_worst = RSSI_HOT;
    trace_clear();
}

static void feed(int8_t dbm)
{
    int16_t t = (int16_t)(dbm * 16);

    if (!f_seen) {
        f_fast = f_slow = t;
        f_best = f_worst = dbm;
        f_seen = true;
    } else {
        f_fast += (int16_t)((t - f_fast) / 4);      /* ~0.5 s of memory */
        f_slow += (int16_t)((t - f_slow) / 32);     /* ~3 s */
        if (dbm > f_best)  f_best  = dbm;
        if (dbm < f_worst) f_worst = dbm;
    }
}

/*
 * Lost the channel. An AP that moves - and they do, on their own, when the
 * band gets busy - takes the watch with it, so rather than sitting on an empty
 * channel the finder walks the band looking for the same BSSID, one channel
 * per frame so that the buttons keep working while it does. Finding it re-arms
 * the watch and nothing else changes: same target, same marks, same trace.
 */
static void hunt_step(const gb_ap_t *target)
{
    static gb_ap_t found[8];

    if (++f_hunt_chan > 13) f_hunt_chan = 1;
    draw_title();       /* the channel counting up is the only progress there is */

    int n = A->wifi_scan(found, (int)(sizeof found / sizeof found[0]), f_hunt_chan, 0);
    for (int i = 0; i < n; i++) {
        if (memcmp(found[i].bssid, target->bssid, GB_BSSID_LEN) != 0) continue;

        f_chan = found[i].channel;
        if (A->wifi_watch(target->bssid, f_chan)) {
            f_hunting   = false;
            f_last_hit  = A->millis();
            draw_title();
        }
        return;
    }
}

/* ------------------------------------------------------------------ main */

bool finder_run(const gb_ap_t *target)
{
    gb_hit_t hits[24];

    f_chan = target->channel;
    ap_label(f_name, sizeof f_name, target);

    layout();
    s_span = 0;
    f_fast = f_slow = (int16_t)(target->rssi * 16);
    f_rate = f_count = 0;
    f_hunting = false;
    f_hunt_chan = f_chan;
    f_last_hit = f_rate_t0 = A->millis();

    draw_static();
    reset_marks();

    if (!A->wifi_watch(target->bssid, f_chan)) {
        A->text(6, (int16_t)(TITLE_H + 20), "cannot watch that channel", T.warn, T.bg, 1);
        A->sleep_ms(1200);
        return true;
    }

    while (!A->should_stop()) {
        uint32_t now = A->millis();

        int n = A->wifi_watch_poll(hits, (int)(sizeof hits / sizeof hits[0]));
        for (int i = 0; i < n; i++) {
            feed(hits[i].rssi);
            trace_sample(hits[i].rssi);
            f_count++;
            f_last_hit = hits[i].t_ms;
        }

        /* Frames per second over a whole second, counted rather than averaged:
         * this is the number that says the radio is still hearing the target
         * at all, and an average would smear the moment it stopped. */
        if (now - f_rate_t0 >= 1000) {
            f_rate    = f_count;
            f_count   = 0;
            f_rate_t0 = now;
        }

        bool lost = (now - f_last_hit) > LOST_MS;

        if (!f_hunting && (now - f_last_hit) > HUNT_MS) {
            f_hunting   = true;
            f_hunt_chan = f_chan;
            draw_title();
        }

        if (f_hunting) hunt_step(target);       /* costs a dwell, ~120 ms */

        draw_number(lost);
        draw_badge(lost);
        draw_distance(lost);
        draw_bar(lost);
        trace_step(now, lost);
        draw_footer();

        gb_event_t e;
        /* Waiting rather than sleeping, so a tap during the frame gap is
         * answered when it is made. The hunt does its own waiting in the scan,
         * which is why the timeout goes to nothing while it runs. */
        e = A->wait_event(f_hunting ? 1 : POLL_MS);
        do {
            switch (e) {
            case GB_EV_L_SHORT:
                s_span = (uint8_t)((s_span + 1) % 3);
                trace_clear();
                break;

            case GB_EV_R_SHORT:
                reset_marks();
                break;

            case GB_EV_R_LONG:
                A->wifi_watch(NULL, 0);
                return true;

            case GB_EV_L_LONG:
                A->wifi_watch(NULL, 0);
                return false;

            default:
                break;
            }
        } while ((e = A->poll_event()) != GB_EV_NONE);
    }

    A->wifi_watch(NULL, 0);
    return false;
}
