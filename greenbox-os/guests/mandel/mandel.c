/*
 * mandel.c - MANDEL, a fractal viewer for greenbox.
 *
 * Three fractals - the Mandelbrot set, a Julia set, and a Lyapunov diagram -
 * each dropped somewhere worth looking at, in a palette invented on the spot.
 *
 *   L tap     another palette, on the same view
 *   R tap     another place, in the same fractal
 *   R hold    the next fractal
 *   L hold    back to the launcher
 *
 * There is no zoom control and no panning, which is a decision rather than an
 * omission: two buttons yield four gestures, one of which has to be "leave",
 * and a viewer that can be steered badly is worse than one that only ever
 * arrives somewhere good. What would have been navigation is spent on the
 * search in fractal.c instead - every R tap costs a fraction of a second of
 * looking before it costs a second of drawing, and what comes back has been
 * measured rather than guessed.
 *
 * The picture is drawn three times over, each pass better than the one before.
 *
 * A COARSE pass fills the screen in 4x4 blocks about a tenth of a second in,
 * so that something is on the panel long before anything else finishes. The
 * FINE pass then walks down a row at a time at one sample per pixel, and what
 * it leaves is a complete and honest picture. The REFINE pass goes back over
 * the pixels where the picture steps between neighbours - the fringe beside a
 * filament, the edge of the set - and samples those five ways instead of one.
 *
 * Every pass checks the buttons between rows, so a press during a render is
 * answered when it is made rather than at the end. That ordering is also what
 * makes supersampling affordable: the refinement wants five samples on between
 * a tenth and two thirds of the screen, and folded into the fine pass it would
 * have quadrupled the wait before anything appeared. Done afterwards it costs
 * nothing that is being waited on, and an interrupted refinement leaves a
 * picture that is merely less smooth than it was going to be.
 *
 * The fine pass keeps one byte per pixel - a palette index, not a colour - and
 * that byte is what makes L tap instant: a new palette is a new lookup table
 * over the same 32 KB of indices, so the screen repaints without iterating
 * anything. A refined pixel is a blend of five colours and has no index, so
 * refinement paints to the panel and not into the buffer; a recolour therefore
 * repaints from the buffer at once and then refines again, in that order,
 * which is the same two things the eye wants in the order it wants them. The
 * allocation is optional: without it the program re-renders instead of
 * repainting, and shows what the fine pass drew.
 *
 * The panel is used in whatever orientation the OS is set to. A fractal has no
 * up, and the view is fitted to the panel rather than the other way round, so
 * declining the system setting here would be rude for no gain.
 */

#include "mandel.h"

const gb_api_t *A;
int16_t g_w, g_h;

static gb_theme_t g_theme;
static scene_t    g_scene;
static uint8_t   *g_buf;            /* one palette index per pixel, or NULL */
static int        g_buf_ok;         /* and whether it holds this picture */
static int        g_pal_no = 1;     /* shown in the label, so palettes differ */

/* One coarse block row, which doubles as the single-row buffer of the fine
 * pass. api->pixel() would be a windowed SPI transaction per pixel; whole rows
 * are the only sensible unit on this panel. */
#define CO 4
static uint16_t g_blk[SCR_MAX * CO];

/*
 * What the program is in the middle of. Painting is driven from the main loop
 * rather than from wherever a button happened to be handled, so that every
 * pass is interruptible the same way and none can nest inside another.
 */
enum { JOB_NONE = 0, JOB_RENDER, JOB_RECOLOUR, JOB_REFINE };
static uint8_t s_job;

/* What the last picture cost, reported to the console when it is finished.
 * The three passes are worth telling apart: the first two are the wait before
 * there is anything to look at, the third is only how long it keeps improving.
 */
static uint32_t s_t_start, s_ms_fine, s_ss_px;

/* The label sits on the bottom rows for a moment after anything changes. It is
 * 18 rows because the OS paints its own quit bar over the bottom 13 during a
 * left hold, and a strip that covers that one is a strip that can put it back
 * in a single repaint. */
#define LABEL_H   18
#define LABEL_MS  2200
static uint32_t s_label_until;

/* ================================================================== drawing */

static gb_event_t pump(void)
{
    if (A->should_stop()) return GB_EV_L_LONG;
    return A->poll_event();
}

/*
 * A range of rows at one sample per pixel, blitted as they are computed and
 * remembered in the index buffer. `poll` is off for the small repaints behind
 * the label, where there is nothing worth interrupting.
 */
static gb_event_t paint_span(int y0, int rows, int poll)
{
    if (y0 < 0) { rows += y0; y0 = 0; }
    int y1 = y0 + rows;
    if (y1 > g_h) y1 = g_h;

    for (int y = y0; y < y1; y++) {
        uint8_t *dst = g_buf ? g_buf + (size_t)y * g_w : NULL;

        for (int x = 0; x < g_w; x++) {
            uint8_t s = pal_shade(fr_at(x, y));
            g_blk[x] = g_pal[s];
            if (dst) dst[x] = s;
        }
        A->blit(0, (int16_t)y, g_w, 1, g_blk);

        if (poll) {
            gb_event_t e = pump();
            if (e != GB_EV_NONE) return e;
        }
    }
    return GB_EV_NONE;
}

/*
 * One supersampled pixel: the centre colour the caller already has, blended
 * with the colours of the four quarter-points.
 *
 * The blend is of colours and not of values, and that is the whole reason it
 * helps. Halfway along a filament the five values are scattered across the
 * range, and the colour of their average is not the average of their colours -
 * it is just another arbitrary colour, which is what the speckle already was.
 * Blending what is actually shown also softens the edge of the set itself,
 * where the samples that land inside have no value to average at all.
 *
 * The divide by five is a multiply: 205/1024 is within a part in five hundred,
 * and this runs on every refined pixel.
 */
static uint16_t ss_colour(int x, int y, uint16_t base)
{
    int32_t v[4];
    fr_at_ss4(x, y, v);

    int r = (base >> 11) & 31, g = (base >> 5) & 63, b = base & 31;
    for (int i = 0; i < 4; i++) {
        uint16_t c = g_pal[pal_shade(v[i])];
        r += (c >> 11) & 31;
        g += (c >>  5) & 63;
        b +=  c        & 31;
    }
    return (uint16_t)((((r * 205) >> 10) << 11) |
                      (((g * 205) >> 10) <<  5) |
                       ((b * 205) >> 10));
}

/*
 * The third pass, over a range of rows: find the pixels where the picture
 * steps between neighbours and sample those five ways.
 *
 * It reads the index buffer and never writes it, so the neighbours it asks
 * about are always the ones the fine pass drew - no lag rows, no half-refined
 * comparisons - and a recolour can still repaint the whole screen from the
 * same bytes. What that costs is that refinement does not survive into the
 * buffer, so anything repainted from it comes back unrefined and has to be
 * refined again. Which is why every repaint here is followed by a refine of
 * the same rows.
 *
 * A row is blitted only if something in it actually changed.
 */
static gb_event_t refine_span(int y0, int rows, int poll)
{
    if (!g_buf_ok) return GB_EV_NONE;

    if (y0 < 0) { rows += y0; y0 = 0; }
    int y1 = y0 + rows;
    if (y1 > g_h) y1 = g_h;

    for (int y = y0; y < y1; y++) {
        const uint8_t *me = g_buf + (size_t)y * g_w;
        const uint8_t *up = y > 0       ? me - g_w : NULL;
        const uint8_t *dn = y + 1 < g_h ? me + g_w : NULL;
        int changed = 0;

        for (int x = 0; x < g_w; x++) {
            uint8_t  s   = me[x];
            uint16_t col = g_pal[s];

            if ((x > 0       && fr_jump(s, me[x - 1])) ||
                (x + 1 < g_w && fr_jump(s, me[x + 1])) ||
                (up          && fr_jump(s, up[x]))     ||
                (dn          && fr_jump(s, dn[x]))) {
                col = ss_colour(x, y, col);
                changed = 1;
                s_ss_px++;
            }
            g_blk[x] = col;
        }

        if (changed) A->blit(0, (int16_t)y, g_w, 1, g_blk);

        /* The kill is honoured even on a short span - a guest gets 400 ms to
         * notice one - but only a full-screen pass takes button events, since
         * a short span is over sooner than the wait it would have returned to.
         */
        if (A->should_stop()) return GB_EV_L_LONG;
        if (poll) {
            gb_event_t e = A->poll_event();
            if (e != GB_EV_NONE) return e;
        }
    }
    return GB_EV_NONE;
}

/* The same rows, from the index buffer: no iteration, just a lookup. */
static void repaint_rows(int y0, int rows)
{
    for (int y = y0; y < y0 + rows && y < g_h; y++) {
        if (y < 0) continue;
        const uint8_t *src = g_buf + (size_t)y * g_w;
        for (int x = 0; x < g_w; x++) g_blk[x] = g_pal[src[x]];
        A->blit(0, (int16_t)y, g_w, 1, g_blk);
    }
}

/* Put back whatever the label or the OS quit bar covered. */
static void restore_rows(int y0, int rows)
{
    if (g_buf_ok) {
        repaint_rows(y0, rows);
        refine_span(y0, rows, 0);
    } else {
        paint_span(y0, rows, 0);
    }
}

/*
 * The whole picture: blocks first, then rows. Returns the event that
 * interrupted it, or GB_EV_NONE if it finished.
 */
static gb_event_t render(void)
{
    s_t_start = A->millis();
    s_ss_px   = 0;
    fr_scene(&g_scene);
    fr_view(g_w, g_h);
    pal_map(&g_scene);
    g_buf_ok = 0;

    for (int by = 0; by < g_h; by += CO) {
        int bh = g_h - by;
        if (bh > CO) bh = CO;

        for (int bx = 0; bx < g_w; bx += CO) {
            int bw = g_w - bx;
            if (bw > CO) bw = CO;
            uint16_t c = g_pal[pal_shade(fr_at(bx + (bw >> 1), by + (bh >> 1)))];
            for (int i = 0; i < bw; i++) g_blk[bx + i] = c;
        }
        for (int r = 1; r < bh; r++)
            memcpy(&g_blk[r * g_w], g_blk, (size_t)g_w * 2);

        A->blit(0, (int16_t)by, g_w, (int16_t)bh, g_blk);

        gb_event_t e = pump();
        if (e != GB_EV_NONE) return e;
    }

    gb_event_t e = paint_span(0, g_h, 1);
    if (e != GB_EV_NONE) return e;           /* the buffer stays invalid */

    /* Complete, and true at one sample per pixel. Everything past here only
     * makes it smoother, so the buffer counts as good from this point on. */
    g_buf_ok = g_buf != NULL;
    s_ms_fine = A->millis() - s_t_start;
    return GB_EV_NONE;
}

/* The same picture in a new palette, which is a lookup rather than a render. */
static gb_event_t recolour(void)
{
    if (!g_buf_ok) return render();
    s_t_start = A->millis();
    s_ss_px   = 0;
    repaint_rows(0, g_h);
    s_ms_fine = A->millis() - s_t_start;
    return GB_EV_NONE;
}

/* ==================================================================== label */

/* 1.6 / hw, in hundredths: how far in this view is, compared with the one the
 * whole set fits in. */
static void zoom_str(char *buf, size_t n, fx hw)
{
    uint32_t z = (uint32_t)((((int64_t)FX(1.6)) * 100) / (hw > 0 ? hw : 1));

    if (z < 1000)          A->snprintf(buf, n, "%u.%ux", z / 100, (z / 10) % 10);
    else if (z < 1000000)  A->snprintf(buf, n, "%ux", z / 100);
    else                   A->snprintf(buf, n, "%uk", z / 100000);
}

static void status_str(char *buf, size_t n)
{
    char z[12];

    switch (g_scene.mode) {
    case MODE_LYAP: {
        char w[SEQ_MAX + 1];
        int len = g_scene.seq_len > SEQ_MAX ? SEQ_MAX : g_scene.seq_len;
        for (int i = 0; i < len; i++) w[i] = (char)('a' + (g_scene.seq[i] & 1));
        w[len] = 0;
        A->snprintf(buf, n, "lyapunov %s", w);
        break;
    }
    case MODE_JULIA:
        zoom_str(z, sizeof z, g_scene.hw);
        A->snprintf(buf, n, "julia %s", z);
        break;
    default:
        zoom_str(z, sizeof z, g_scene.hw);
        A->snprintf(buf, n, "mandelbrot %s", z);
        break;
    }
}

static void label_two(const char *l1, const char *l2)
{
    int16_t y = (int16_t)(g_h - LABEL_H);
    A->fill_rect(0, y, g_w, LABEL_H, g_theme.bg);
    A->text(3, (int16_t)(y + 2), l1, g_theme.fg, g_theme.bg, 1);
    if (l2) A->text(3, (int16_t)(y + 10), l2, g_theme.muted, g_theme.bg, 1);
}

static void label_draw(void)
{
    char l1[40], l2[40];

    status_str(l1, sizeof l1);
    /* The panel is 40 characters wide in landscape and 22 in portrait, and the
     * hint is the line that has to give. */
    if (g_w >= 200) {
        A->snprintf(l2, sizeof l2, "L palette  R place  R-hold mode  #%u",
                    (unsigned)g_pal_no);
        label_two(l1, l2);
    } else {
        A->snprintf(l2, sizeof l2, "L pal  R place  R=mode");
        label_two(l1, l2);
    }
}

static void label_show(void)
{
    label_draw();
    s_label_until = A->millis() + LABEL_MS;
    if (!s_label_until) s_label_until = 1;      /* 0 means "no label" */
}

static void label_hide(void)
{
    s_label_until = 0;
    restore_rows(g_h - LABEL_H, LABEL_H);
}

/* ============================================================== persistence */
/*
 * The view is worth keeping. Finding a good one takes up to a second of
 * searching, and coming back to a program that has forgotten where it was
 * makes that second look like it bought nothing.
 *
 * Only the seed of the palette is stored, not the table: the generator is
 * reproducible, so 4 bytes stand in for 512. The generator state goes in too,
 * so that two launches in a row do not offer the same "random" palette.
 */
#define SAVE_VER  1u
#define SAVE_KEY  "view"

typedef struct {
    uint8_t  ver;
    uint8_t  pad[3];
    scene_t  scene;
    uint32_t pal;
    uint32_t rng;
} save_t;

static void save(void)
{
    save_t s;
    memset(&s, 0, sizeof s);
    s.ver   = SAVE_VER;
    s.scene = g_scene;
    s.pal   = pal_seed();
    s.rng   = rnd_state();
    A->store_put(SAVE_KEY, &s, sizeof s);
}

static int load(void)
{
    save_t s;

    if (A->store_get(SAVE_KEY, &s, sizeof s) != (int)sizeof s) return 0;
    if (s.ver != SAVE_VER || !fr_valid(&s.scene)) return 0;

    g_scene = s.scene;
    rnd_seed(s.rng ^ A->millis());
    pal_new(s.pal);
    return 1;
}

/* ================================================================== events */

static void new_place(uint8_t mode)
{
    label_two("looking for somewhere", "good to point this at");
    fr_find(mode, &g_scene);
}

/* Returns 0 when it is time to leave. */
static int handle(gb_event_t e)
{
    switch (e) {

    case GB_EV_L_SHORT:
        pal_new(0);
        g_pal_no++;
        s_job = JOB_RECOLOUR;
        save();
        break;

    case GB_EV_R_SHORT:
        new_place(g_scene.mode);
        s_job = JOB_RENDER;
        save();
        break;

    case GB_EV_R_LONG:
        new_place((uint8_t)((g_scene.mode + 1) % MODE_COUNT));
        s_job = JOB_RENDER;
        save();
        break;

    case GB_EV_L_LONG:
        A->log("exit");
        return 0;

    default:
        break;
    }
    return 1;
}

/* ==================================================================== main */

int gb_main(const gb_api_t *api)
{
    A   = api;
    g_w = A->width();
    g_h = A->height();
    if (g_w > SCR_MAX) g_w = SCR_MAX;
    if (g_h > SCR_MAX) g_h = SCR_MAX;

    gb_oscfg_t cfg;
    A->oscfg_get(&cfg);
    if (!A->theme_get(cfg.theme, &g_theme)) {
        memset(&g_theme, 0, sizeof g_theme);
        g_theme.fg = GB_WHITE;
        g_theme.muted = GB_GREY;
    }

    /* Nothing here is a good clock, so the seed is three poor ones mixed:
     * milliseconds since boot, the wall clock if it has ever been set, and
     * where the loader happened to put this program. */
    rnd_seed(A->millis() * 2654435761u
             ^ (A->unix_time() << 7)
             ^ (uint32_t)(uintptr_t)api);

    /* Optional, and the program is complete without it - see the header. */
    g_buf = (uint8_t *)A->alloc((size_t)g_w * (size_t)g_h);

    A->fill(g_theme.bg);

    s_job = JOB_RENDER;
    if (!load()) {
        pal_new(0);
        new_place(MODE_MANDEL);
        save();
    }

    int      l_down = 0, strip = 0;
    uint32_t l_since = 0;

    for (;;) {
        if (A->should_stop()) break;

        if (s_job) {
            uint8_t job = s_job;
            gb_event_t e;

            s_job = JOB_NONE;
            switch (job) {
            case JOB_RECOLOUR: e = recolour(); break;
            case JOB_REFINE:
                /* Not under the label: those rows are refined when it goes,
                 * which is also what puts the picture back there. */
                e = refine_span(0, s_label_until ? g_h - LABEL_H : g_h, 1);
                break;
            default:           e = render();   break;
            }

            if (e != GB_EV_NONE) {
                if (!handle(e)) break;
                continue;
            }
            /* The label goes up as soon as the picture is readable - after the
             * fine pass, not after the refinement, which can run for seconds
             * on a deep zoom and has nothing to say about what is on screen. */
            if (job != JOB_REFINE) {
                label_show();
                s_job = JOB_REFINE;
            } else {
                char msg[56];
                A->snprintf(msg, sizeof msg,
                            "drawn in %ums, refined %u%% in %ums",
                            (unsigned)s_ms_fine,
                            (unsigned)(s_ss_px * 100u /
                                       (unsigned)(g_w * g_h)),
                            (unsigned)(A->millis() - s_t_start - s_ms_fine));
                A->log(msg);
            }
            continue;
        }

        uint32_t now  = A->millis();
        uint32_t wait = 400;

        if (s_label_until) {
            if ((int32_t)(now - s_label_until) >= 0) label_hide();
            else wait = s_label_until - now;
        }

        /*
         * A left hold puts the OS quit bar across the bottom of whatever is on
         * the panel, and a hold let go of early leaves it there - the OS
         * cannot put back pixels it never had. So watch for that hold ending
         * without the event that would have taken us out of here, and repaint
         * the strip it covered. Polling four times a second is not enough to
         * catch the release promptly, hence the shorter wait while the button
         * is actually down.
         */
        uint8_t btn = A->buttons();
        if (btn & GB_BTN_L) {
            if (!l_down) { l_down = 1; l_since = now; }
            else if (now - l_since >= 1100) strip = 1;
            if (wait > 120) wait = 120;
        } else if (l_down) {
            l_down = 0;
            if (strip) {
                strip = 0;
                if (s_label_until) label_draw();
                else restore_rows(g_h - LABEL_H, LABEL_H);
            }
        }

        gb_event_t e = A->wait_event(wait ? wait : 1);
        if (e != GB_EV_NONE && !handle(e)) break;
    }

    if (g_buf) A->free(g_buf);
    return 0;
}
