/*
 * clock.c - the first greenbox guest.
 *
 * Controls, displaying:
 *   L tap    12h / 24h
 *   R tap    show or hide seconds
 *   R hold   enter setting mode, on the hours
 *   L hold   exit back to the launcher
 *
 * Controls, setting:
 *   L tap    the flashing field down one
 *   R tap    the flashing field up one
 *   R hold   move to the other field - hours, minutes, hours, ...
 *   L hold   exit back to the launcher
 *
 * There is no way back from setting mode to plain display short of leaving:
 * with two buttons and three gestures already spoken for, the honest answer to
 * "how do I stop setting" is the same as "how do I leave", and the clock is
 * cheap to start again.
 *
 * Every adjustment is applied to the system clock immediately - there is no
 * confirm step, because there is no button left to confirm with, and a clock
 * that shows a time it has not actually set is worse than one that tracks
 * every press.
 *
 * Settings survive a restart via the guest key/value store, which is
 * namespaced per program, so this cannot collide with anything else. The time
 * itself is the OS's, not this program's, and is persisted there.
 *
 * The panel is always landscape here, whatever the OS is set to. This is a
 * fixed layout, not a responsive one: 240x135 is what "00:00" at size 6 plus a
 * date line under it needs, and in portrait the big row would run off both
 * edges. The OS setting still decides which way up.
 *
 * Everything it can do, it does through `api`. There is no libc here beyond
 * the handful of functions in gb_rt.c, and no ESP-IDF at all.
 *
 * Drawing rule, same as the launcher's: never clear a region on a timer.
 * api->text paints the full glyph cell - the spacing column included - with
 * the background colour, so redrawing a string of the same length in the same
 * place overwrites it cleanly. Clearing first would show as a flicker twice a
 * second, which is exactly what the colon blink would produce, and four times
 * worse once a field is flashing too.
 */

#include "greenbox_abi.h"
#include "gb_rt.h"

/* Persisted as one blob rather than two keys - fewer NVS entries, and the
 * struct is the natural unit anyway. */
typedef struct {
    uint8_t h12;        /* 12-hour display */
    uint8_t secs;       /* show the seconds line */
} settings_t;

static settings_t s_cfg = { .h12 = 0, .secs = 1 };

/* Which field the buttons currently move, and therefore which one flashes. */
typedef enum { ED_OFF = 0, ED_HOUR, ED_MIN } edit_t;

static edit_t s_edit = ED_OFF;

/* Size 6 on a 5x7 font gives a 36x48 cell, so "00:00" occupies 180x48. Both
 * halves are always two digits and the separator is always one character, so
 * the big row never changes width: each field can be repainted in place
 * without anything underneath it ever needing to be cleared. */
#define BIG_SIZE   6
#define BIG_CELL   (6 * BIG_SIZE)
#define BIG_H      (8 * BIG_SIZE)
#define BIG_CHARS  "00:00"
#define MM_OFF     (3 * BIG_CELL)   /* minutes start past "HH:" */
#define COLON_OFF  (2 * BIG_CELL)

/* How a field is painted. The two edit styles are the flash: same digits,
 * inverted, so the value stays readable while you are changing it - blanking
 * it on the off phase would hide the very thing the buttons are moving. */
enum { ST_NORMAL, ST_DIM, ST_EDIT_ON, ST_EDIT_OFF };

/* What is currently on screen, so the panel is only touched where it changed.
 * Repainting everything at 4 Hz would hammer the SPI bus for nothing. */
static char    s_shown_hh[4];
static char    s_shown_mm[4];
static char    s_shown_sub[28];
static uint8_t s_style_hh = 0xff;
static uint8_t s_style_mm = 0xff;
static int     s_shown_colon = -1;

static void forget_screen(void)
{
    s_shown_hh[0]  = 0;
    s_shown_mm[0]  = 0;
    s_shown_sub[0] = 0;
    s_style_hh     = 0xff;
    s_style_mm     = 0xff;
    s_shown_colon  = -1;
}

/* One two-digit field, repainted only when its text or its style moved. */
static void put_field(const gb_api_t *api, int x, int y, const char *txt,
                      char *shown, uint8_t *shown_style, uint8_t style)
{
    if (*shown_style == style && strcmp(txt, shown) == 0) return;

    uint16_t fg = GB_WHITE, bg = GB_BLACK;
    switch (style) {
    case ST_DIM:      fg = GB_DKGREY; bg = GB_BLACK;  break;
    case ST_EDIT_ON:  fg = GB_BLACK;  bg = GB_YELLOW; break;
    case ST_EDIT_OFF: fg = GB_YELLOW; bg = GB_BLACK;  break;
    default:          break;
    }

    api->text((int16_t)x, (int16_t)y, txt, fg, bg, BIG_SIZE);
    strcpy(shown, txt);
    *shown_style = style;
}

/* `phase` is the 1 Hz square wave that drives both the colon blink and the
 * flashing field; it is passed in rather than read here so the two can never
 * disagree about which half of the second it is. */
static void draw_frame(const gb_api_t *api, const gb_tm_t *tm, int phase)
{
    const int W     = api->width();
    const int H     = api->height();
    const int big_y = (H - BIG_H) / 2 - 8;
    const int sub_y = H - 22;
    const int big_x = (W - api->text_width(BIG_CHARS, BIG_SIZE)) / 2;

    char hh[4], mm[4], col[2], sub[28];
    uint8_t st_h, st_m;

    if (!tm->valid) {
        strcpy(hh, "--");
        strcpy(mm, "--");
        api->snprintf(sub, sizeof sub, "R-hold to set the clock");
        st_h = st_m = ST_DIM;
    } else {
        unsigned h = tm->hour;
        const char *suffix = "";
        if (s_cfg.h12) {
            suffix = h >= 12 ? " pm" : " am";
            h = h % 12;
            if (h == 0) h = 12;
        }
        api->snprintf(hh, sizeof hh, "%02u", h);
        api->snprintf(mm, sizeof mm, "%02u", (unsigned)tm->min);

        if (s_cfg.secs)
            api->snprintf(sub, sizeof sub, "%02u  %04u-%02u-%02u%s",
                          (unsigned)tm->sec, (unsigned)tm->year,
                          (unsigned)tm->mon, (unsigned)tm->day, suffix);
        else
            api->snprintf(sub, sizeof sub, "%04u-%02u-%02u%s",
                          (unsigned)tm->year, (unsigned)tm->mon,
                          (unsigned)tm->day, suffix);
        st_h = st_m = ST_NORMAL;
    }

    if (s_edit == ED_HOUR) st_h = phase ? ST_EDIT_ON : ST_EDIT_OFF;
    if (s_edit == ED_MIN)  st_m = phase ? ST_EDIT_ON : ST_EDIT_OFF;

    put_field(api, big_x,          big_y, hh, s_shown_hh, &s_style_hh, st_h);
    put_field(api, big_x + MM_OFF, big_y, mm, s_shown_mm, &s_style_mm, st_m);

    /* The separator holds still while a field is flashing - one thing moving
     * at a time is the difference between "that digit is selected" and "the
     * screen is broken". */
    col[0] = (s_edit != ED_OFF || phase) ? ':' : ' ';
    col[1] = 0;
    if (s_shown_colon != col[0]) {
        api->text((int16_t)(big_x + COLON_OFF), (int16_t)big_y, col,
                  tm->valid ? GB_WHITE : GB_DKGREY, GB_BLACK, BIG_SIZE);
        s_shown_colon = col[0];
    }

    /* The sub line is centred, so a change of length also moves it, and only
     * then can it leave a tail behind. Equal lengths land on the same pixels
     * and paint over themselves. */
    if (strcmp(sub, s_shown_sub) != 0) {
        if (strlen(sub) != strlen(s_shown_sub))
            api->fill_rect(0, (int16_t)sub_y, (int16_t)W, 10, GB_BLACK);
        int x = (W - api->text_width(sub, 1)) / 2;
        api->text((int16_t)x, (int16_t)sub_y, sub,
                  tm->valid ? GB_CYAN : GB_ORANGE, GB_BLACK, 1);
        strcpy(s_shown_sub, sub);
    }
}

static void draw_chrome(const gb_api_t *api)
{
    const int W = api->width();
    const int H = api->height();

    const char *title = s_edit == ED_OFF ? "clock" :
                        s_edit == ED_HOUR ? "set hours" : "set minutes";
    const char *foot;
    if (s_edit == ED_OFF)
        foot = s_cfg.h12 ? "L:24h  R:secs  R-hold:set  L-hold:exit"
                         : "L:12h  R:secs  R-hold:set  L-hold:exit";
    else if (s_edit == ED_HOUR)
        foot = "L:-  R:+  R-hold:minutes  L-hold:exit";
    else
        foot = "L:-  R:+  R-hold:hours  L-hold:exit";

    api->fill(GB_BLACK);
    api->rect(0, 0, (int16_t)W, (int16_t)H,
              s_edit == ED_OFF ? GB_DKGREY : GB_YELLOW);
    api->text(4, 4, title, s_edit == ED_OFF ? GB_GREY : GB_YELLOW, GB_BLACK, 1);
    api->text(4, (int16_t)(H - 10), foot, GB_DKGREY, GB_BLACK, 1);

    /* The fill wiped everything, so the next frame has to repaint in full. */
    forget_screen();
}

/*
 * Move the field under the cursor by one and commit.
 *
 * A field editor, not an offset: the hours wrap 23 -> 00 without touching the
 * date and the minutes wrap 59 -> 00 without touching the hours, which is what
 * anyone setting a clock by hand expects. Carrying would mean tapping past
 * midnight moved you a day, and there is nothing on screen that would explain
 * why.
 *
 * Setting the minutes zeroes the seconds - that is how you line this up
 * against a phone. Setting the hours leaves them alone, so correcting a
 * timezone does not throw away a synchronised second hand.
 */
static void bump(const gb_api_t *api, int delta)
{
    gb_tm_t tm;
    api->get_time(&tm);

    if (!tm.valid) {
        /* Nothing to move yet. The OS refuses anything before 2021 - it will
         * not restore such a time across a reboot - so the first press
         * establishes a baseline rather than applying delta to nothing. */
        tm.year = 2026; tm.mon = 1; tm.day  = 1;
        tm.hour = 0;    tm.min = 0; tm.sec  = 0;
    } else if (s_edit == ED_MIN) {
        tm.min = (uint8_t)((tm.min + 60 + delta) % 60);
        tm.sec = 0;
    } else {
        tm.hour = (uint8_t)((tm.hour + 24 + delta) % 24);
    }

    if (!api->set_time(&tm)) api->log("set_time refused these fields");
}

int gb_main(const gb_api_t *api)
{
    /* Rotations 0 and 2 are the two ways up of portrait, 1 and 3 the two ways
     * up of landscape, and the pairs differ by a half turn. Keeping bit 1 and
     * forcing bit 0 therefore lands on the landscape that matches however the
     * user is holding the board, rather than flipping it on them. The OS puts
     * its own rotation back when this returns, kill or no kill, so there is
     * nothing to undo here. */
    gb_oscfg_t oscfg;
    api->oscfg_get(&oscfg);
    api->set_rotation((uint8_t)((oscfg.rotation & 2) | 1));

    settings_t saved;
    if (api->store_get("cfg", &saved, sizeof saved) == (int)sizeof saved)
        s_cfg = saved;

    draw_chrome(api);

    while (!api->should_stop()) {
        gb_tm_t tm;
        api->get_time(&tm);

        /* Blink at 1 Hz. draw_frame does the change detection, so calling it
         * every pass costs nothing when nothing moved. */
        int phase = ((api->millis() / 500) & 1) == 0;
        draw_frame(api, &tm, phase);

        /* 250 ms is fast enough for the blink and slow enough to leave the CPU
         * mostly idle. wait_event doubles as the frame timer. */
        switch (api->wait_event(250)) {
        case GB_EV_L_SHORT:
            if (s_edit != ED_OFF) {
                bump(api, -1);
            } else {
                s_cfg.h12 = !s_cfg.h12;
                api->store_put("cfg", &s_cfg, sizeof s_cfg);
                draw_chrome(api);
            }
            break;

        case GB_EV_R_SHORT:
            if (s_edit != ED_OFF) {
                bump(api, +1);
            } else {
                s_cfg.secs = !s_cfg.secs;
                api->store_put("cfg", &s_cfg, sizeof s_cfg);
                draw_chrome(api);
            }
            break;

        case GB_EV_R_LONG:
            /* Off -> hours -> minutes -> hours -> ... */
            s_edit = (s_edit == ED_HOUR) ? ED_MIN : ED_HOUR;
            draw_chrome(api);
            break;

        case GB_EV_L_LONG:
            api->log("exit");
            return 0;

        default:
            break;
        }
    }

    return 0;
}
