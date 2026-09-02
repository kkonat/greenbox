/*
 * settings.c - the OS settings program.
 *
 * Two things to choose - the orientation the system lives in, and the palette
 * everything is painted with - and two buttons to choose them with.
 *
 * Controls:
 *   L tap    previous cell
 *   R tap    next cell
 *   R hold   apply the cell the cursor is on, and remember it
 *   L hold   leave
 *
 * The cursor walks one flat row of cells: the two orientations, then one
 * swatch per theme. There is no edit mode and no second level, because with
 * seven cells there does not need to be one, and because a menu that has to be
 * entered before it can be changed costs two holds per setting where this
 * costs one.
 *
 * Everything previews. Moving the cursor onto a swatch repaints this program
 * in that theme; moving it onto an orientation turns the panel. What is
 * previewed is not what is stored - the accent bar under a cell marks the
 * value the OS actually has, and the title bar says "unsaved" whenever the two
 * differ. R-hold is what closes the gap.
 *
 * That split is also why leaving without applying needs no confirmation and no
 * undo: a theme preview never existed anywhere but in this program's own
 * drawing, and the OS puts the stored orientation back when a guest exits,
 * kill or no kill. Walking away really does leave nothing behind.
 *
 * Drawing: the whole screen is repainted on every press. The launcher's rule -
 * never clear on a timer - is about repaints the user did not ask for; here
 * there is no timer and nothing moves except in answer to a button, so a full
 * clear is the cheap and obviously-correct thing.
 */

#include "greenbox_abi.h"
#include "gb_rt.h"

static const gb_api_t *A;

/* More themes than this and the swatch row stops being a row. The OS is asked
 * how many it has rather than told, so this is a display limit, not a claim
 * about the OS. */
#define MAX_THEMES  6

static gb_theme_t s_theme[MAX_THEMES];
static uint8_t    s_nthemes;

static gb_oscfg_t s_saved;          /* what the OS has stored right now */
static uint8_t    s_cursor;         /* 0,1 orientation; 2.. one per theme */
static uint8_t    s_panel_rot;      /* what the panel is actually turned to */

/* ------------------------------------------------------------- geometry */

#define TITLE_H   16
#define FOOT_H    10
#define PILL_H    20
#define PILL_GAP   8
#define SW_H      28
#define SW_GAP     5
#define MARK_H     3        /* the "this one is stored" bar under a cell */
#define LABEL_H    8

static int16_t W, H;
static int16_t s_pill_x0, s_pill_y, s_pill_w;
static int16_t s_sw_x0,   s_sw_y,   s_sw_w;

static void layout(void)
{
    W = A->width();
    H = A->height();

    s_pill_w = (int16_t)((W - 12 - PILL_GAP) / 2);
    s_sw_w   = (int16_t)((W - 12 - (s_nthemes - 1) * SW_GAP) / s_nthemes);

    /* Centre both rows in whatever slack the division left, so the same code
     * lays out 240 columns and 135 without either looking left-heavy. */
    s_pill_x0 = (int16_t)((W - (2 * s_pill_w + PILL_GAP)) / 2);
    s_sw_x0   = (int16_t)((W - (s_nthemes * s_sw_w +
                                (s_nthemes - 1) * SW_GAP)) / 2);

    const int block = LABEL_H + 2 + PILL_H + 3 + MARK_H
                    + 10
                    + LABEL_H + 2 + SW_H + 3 + MARK_H;

    int top = TITLE_H + (H - TITLE_H - FOOT_H - block) / 2;
    if (top < TITLE_H + 4) top = TITLE_H + 4;

    s_pill_y = (int16_t)(top + LABEL_H + 2);
    s_sw_y   = (int16_t)(s_pill_y + PILL_H + 3 + MARK_H + 10 + LABEL_H + 2);
}

static uint8_t cell_count(void) { return (uint8_t)(2 + s_nthemes); }

static void cell_rect(uint8_t i, int16_t *x, int16_t *y, int16_t *w, int16_t *h)
{
    if (i < 2) {
        *x = (int16_t)(s_pill_x0 + i * (s_pill_w + PILL_GAP));
        *y = s_pill_y; *w = s_pill_w; *h = PILL_H;
    } else {
        *x = (int16_t)(s_sw_x0 + (i - 2) * (s_sw_w + SW_GAP));
        *y = s_sw_y;   *w = s_sw_w;   *h = SW_H;
    }
}

/* ------------------------------------------------------ what is previewed */

/* The palette this program paints itself with: the one under the cursor if the
 * cursor is on a swatch, otherwise the stored one. */
static const gb_theme_t *ui_theme(void)
{
    uint8_t idx = s_cursor >= 2 ? (uint8_t)(s_cursor - 2) : s_saved.theme;
    if (idx >= s_nthemes) idx = 0;
    return &s_theme[idx];
}

/* Rotations 2 and 3 are the same two shapes upside down. The OS stores them
 * and the console can set them, but there is nothing on this screen to point
 * at that would say "portrait, but the other way up", so this program offers 0
 * and 1 and treats a stored 2 or 3 as the orientation it looks like. */
static uint8_t ui_rotation(void)
{
    return s_cursor < 2 ? s_cursor : s_saved.rotation;
}

static bool cell_is_stored(uint8_t i)
{
    if (i < 2) return (s_saved.rotation & 1u) == i;
    return (uint8_t)(i - 2) == s_saved.theme;
}

/* --------------------------------------------------------------- drawing */

/* A swatch is a screen in miniature - title bar, a bright tick, three weights
 * of text, one warning - rather than a row of colour chips. What a theme is
 * for is what a screen looks like in it, and at 41x28 that is still legible.
 */
static void draw_swatch(int16_t x, int16_t y, int16_t w, int16_t h,
                        const gb_theme_t *q, uint16_t border)
{
    A->fill_rect(x, y, w, h, q->bg);
    A->fill_rect(x, y, w, 6, q->surface);
    A->fill_rect((int16_t)(x + 2), (int16_t)(y + 2), 3, 2, q->accent);

    A->fill_rect((int16_t)(x + 2), (int16_t)(y +  9), (int16_t)(w -  8), 3, q->fg);
    A->fill_rect((int16_t)(x + 2), (int16_t)(y + 14), (int16_t)(w - 12), 3, q->dim);
    A->fill_rect((int16_t)(x + 2), (int16_t)(y + 19), (int16_t)(w -  6), 3, q->muted);
    A->fill_rect((int16_t)(x + w - 6), (int16_t)(y + h - 5), 3, 3, q->warn);

    /* Bordered in the surrounding theme, not its own: a dark swatch on a dark
     * page needs an edge, and the edge belongs to the page. */
    A->rect(x, y, w, h, border);
}

static void draw_pill(int16_t x, int16_t y, int16_t w, int16_t h,
                      const char *label, bool stored, const gb_theme_t *t)
{
    uint16_t bg = stored ? t->surface : t->bg;

    A->fill_rect(x, y, w, h, bg);
    A->rect(x, y, w, h, t->muted);
    A->text((int16_t)(x + (w - A->text_width(label, 1)) / 2),
            (int16_t)(y + (h - 8) / 2), label,
            stored ? t->fg : t->dim, bg, 1);
}

/* The footer legend is the only text here that will not fit both orientations,
 * so it is chosen by measuring rather than by assuming 240 columns. */
static const char *footer(void)
{
    static const char *const cand[] = {
        "L/R move   R-hold apply   L-hold exit",
        "L/R move  R-hold apply  L-hold exit",
        "L/R  R-hold apply  L-hold exit",
        "L/R  R-hold:ok  L:exit",
        "L/R move  R:ok L:exit",
        "L/R  R:ok  L:exit",
    };
    const int n = (int)(sizeof cand / sizeof cand[0]);

    for (int i = 0; i < n; i++)
        if (A->text_width(cand[i], 1) <= W - 8) return cand[i];
    return cand[n - 1];
}

static void draw(void)
{
    const gb_theme_t *t = ui_theme();
    char buf[24];

    A->fill(t->bg);
    A->fill_rect(0, 0, W, TITLE_H, t->surface);
    A->text(4, 4, "settings", t->accent, t->surface, 1);

    /* One word for the whole preview-versus-stored distinction. It is on
     * screen for exactly as long as an R-hold would change something. */
    if (!cell_is_stored(s_cursor)) {
        const char *u = "unsaved";
        A->text((int16_t)(W - A->text_width(u, 1) - 4), 4, u,
                t->warn, t->surface, 1);
    }

    A->text(s_pill_x0, (int16_t)(s_pill_y - LABEL_H - 2), "orientation",
            t->muted, t->bg, 1);

    draw_pill(s_pill_x0, s_pill_y, s_pill_w, PILL_H, "portrait",
              cell_is_stored(0), t);
    draw_pill((int16_t)(s_pill_x0 + s_pill_w + PILL_GAP), s_pill_y,
              s_pill_w, PILL_H, "landscape", cell_is_stored(1), t);

    A->snprintf(buf, sizeof buf, "theme  %s", t->name);
    A->text(s_sw_x0, (int16_t)(s_sw_y - LABEL_H - 2), buf, t->muted, t->bg, 1);

    for (uint8_t k = 0; k < s_nthemes; k++)
        draw_swatch((int16_t)(s_sw_x0 + k * (s_sw_w + SW_GAP)), s_sw_y,
                    s_sw_w, SW_H, &s_theme[k], t->muted);

    /* The stored markers, then the cursor on top of whichever cell it is on. */
    for (uint8_t i = 0; i < cell_count(); i++) {
        int16_t x, y, w, h;
        cell_rect(i, &x, &y, &w, &h);
        if (cell_is_stored(i))
            A->fill_rect(x, (int16_t)(y + h + 3), w, MARK_H, t->accent);
    }

    {
        int16_t x, y, w, h;
        cell_rect(s_cursor, &x, &y, &w, &h);
        A->rect((int16_t)(x - 2), (int16_t)(y - 2),
                (int16_t)(w + 4), (int16_t)(h + 4), t->accent);
        A->rect((int16_t)(x - 3), (int16_t)(y - 3),
                (int16_t)(w + 6), (int16_t)(h + 6), t->accent);
    }

    A->text(4, (int16_t)(H - FOOT_H + 1), footer(), t->muted, t->bg, 1);
}

/* Turning the panel changes what every coordinate means, so the layout is
 * recomputed before anything is drawn in the new shape. */
static void repaint(void)
{
    uint8_t want = ui_rotation();

    if (want != s_panel_rot) {
        A->set_rotation(want);
        s_panel_rot = want;
        layout();
    }
    draw();
}

/* ------------------------------------------------------------------ main */

static void apply(void)
{
    gb_oscfg_t c = s_saved;

    if (s_cursor < 2) c.rotation = s_cursor;
    else              c.theme    = (uint8_t)(s_cursor - 2);

    if (A->oscfg_set(&c)) {
        s_saved = c;
        A->log("applied");
    } else {
        /* The OS validates. Every value here came from the OS in the first
         * place, so a refusal is a bug report, not something to put on screen
         * for the user to work around. */
        A->log("settings refused");
    }
}

int gb_main(const gb_api_t *api)
{
    A = api;

    s_nthemes = api->theme_count();
    if (s_nthemes > MAX_THEMES) s_nthemes = MAX_THEMES;
    for (uint8_t i = 0; i < s_nthemes; i++)
        if (!api->theme_get(i, &s_theme[i])) { s_nthemes = i; break; }

    if (s_nthemes == 0) {           /* an OS with no palettes: nothing to show */
        api->log("no themes");
        return 1;
    }

    api->oscfg_get(&s_saved);
    if (s_saved.theme >= s_nthemes) s_saved.theme = 0;

    /* A guest is handed the panel already in the system orientation, so the
     * cursor can start on the cell that is currently true. */
    s_panel_rot = s_saved.rotation;
    s_cursor    = (uint8_t)(s_saved.rotation & 1u);

    layout();
    draw();

    while (!api->should_stop()) {
        switch (api->wait_event(250)) {
        case GB_EV_L_SHORT:
            s_cursor = (uint8_t)((s_cursor + cell_count() - 1) % cell_count());
            repaint();
            break;

        case GB_EV_R_SHORT:
            s_cursor = (uint8_t)((s_cursor + 1) % cell_count());
            repaint();
            break;

        case GB_EV_R_LONG:
            apply();
            repaint();
            break;

        case GB_EV_L_LONG:
            api->log("exit");
            return 0;

        default:
            break;                  /* timeout: nothing here moves on its own */
        }
    }

    return 0;
}
