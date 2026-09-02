/*
 * launcher.c - the shell.
 *
 * Button contract, which is the whole user interface:
 *
 *   L tap        previous entry
 *   R tap        next entry
 *   L hold 3 s   escape - leaves the info screen, does nothing at the list
 *                root, and kills whatever is running
 *   R hold 1 s   accept - runs the selected program
 *
 * The escape and the kill are one gesture: the input task emits GB_EV_L_LONG
 * and asks for the teardown together. The kill half is handled there, not
 * here, so it works even when a guest has stopped reading events. The launcher
 * only finds out about it by guest_supervise returning.
 *
 * Drawing rule: the panel is only cleared when something structural changes -
 * the selection, the view, the program list. The half-second wakeup that keeps
 * the header clock live must never repaint the whole screen, because a
 * full-screen fill followed by a repaint at 2 Hz reads as a flicker. Anything
 * that updates on a timer is drawn with an opaque background over its own
 * fixed region instead.
 *
 * Nothing below picks a colour. Every painter starts by reading the current
 * palette out of the settings, because the settings program can change it -
 * and the orientation with it - while the launcher is parked inside
 * guest_supervise, and a cached palette would come back stale.
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"

#include "st7789.h"
#include "osconf.h"
#include "input.h"
#include "guest.h"
#include "ostime.h"
#include "launcher.h"

static const char *TAG = "launch";

#define MAX_PROGS   16
#define ROW_H       20
#define TITLE_H     18
#define FOOT_H      10
#define CLOCK_W     40      /* reserved corner for the header clock */
#define TOAST_MS    2500

typedef struct {
    char     file[64];              /* full path */
    char     name[GB_NAME_MAX + 1]; /* from the header, falls back to filename */
    uint32_t bytes;                 /* on-disk size */
    bool     ok;                    /* header parsed and ABI matches */
} prog_t;

static prog_t s_prog[MAX_PROGS];
static int    s_n;
static int    s_sel;
static int    s_top;                /* first visible row */
static bool   s_info;               /* info screen instead of the list */
static bool   s_dirty = true;       /* a full repaint is owed */
static char   s_toast[40];
static uint32_t s_toast_until;
static bool   s_toast_shown;
static char   s_pending[GB_NAME_MAX + 1];   /* console asked for this one */
static char   s_hdr_clock[8];       /* what the header corner currently says */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------- scanning */

static void read_header(prog_t *p)
{
    FILE *f = fopen(p->file, "rb");
    if (!f) return;

    gb_hdr_t h;
    if (fread(&h, 1, sizeof h, f) == sizeof h && h.magic == GB_MAGIC) {
        memcpy(p->name, h.name, GB_NAME_MAX);
        p->name[GB_NAME_MAX] = 0;
        p->ok = (h.abi == GB_ABI_VERSION);
    }
    fclose(f);
}

void launcher_rescan(void)
{
    s_n = 0;
    s_dirty = true;

    DIR *d = opendir(GB_PROG_DIR);
    if (!d) { ESP_LOGW(TAG, "no %s", GB_PROG_DIR); return; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && s_n < MAX_PROGS) {
        size_t len = strlen(de->d_name);
        size_t ext = strlen(GB_PROG_EXT);
        if (len <= ext || strcmp(de->d_name + len - ext, GB_PROG_EXT) != 0)
            continue;

        prog_t *p = &s_prog[s_n];
        memset(p, 0, sizeof *p);
        snprintf(p->file, sizeof p->file, "%s/%.48s", GB_PROG_DIR, de->d_name);

        /* Filename minus extension is the fallback display name. */
        size_t stem = len - ext;
        if (stem > GB_NAME_MAX) stem = GB_NAME_MAX;
        memcpy(p->name, de->d_name, stem);
        p->name[stem] = 0;

        struct stat st;
        if (stat(p->file, &st) == 0) p->bytes = (uint32_t)st.st_size;

        read_header(p);
        s_n++;
    }
    closedir(d);

    if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : 0;
    ESP_LOGI(TAG, "%d program%s in %s", s_n, s_n == 1 ? "" : "s", GB_PROG_DIR);
}

/* --------------------------------------------------------------- drawing */

/* The top of every full repaint. The panel may be in the wrong orientation -
 * a guest that ran sideways, or the user changing the setting while this task
 * was blocked - and the caller is about to clear the screen anyway, which is
 * the one moment when turning it costs nothing. */
static const gb_theme_t *begin_paint(void)
{
    uint8_t want = osconf_rotation();
    if (st7789_rotation() != want) st7789_set_rotation(want);
    return osconf_theme();
}

static void toast(const char *msg)
{
    snprintf(s_toast, sizeof s_toast, "%s", msg);
    s_toast_until = now_ms() + TOAST_MS;
    s_dirty = true;
}

/* The header corner, repainted over itself. Cheap enough to call at 2 Hz, and
 * it touches the panel only when the text actually changed. */
static void paint_clock(bool force)
{
    char buf[8];
    gb_tm_t tm;
    ostime_get(&tm);

    if (tm.valid) snprintf(buf, sizeof buf, "%02u:%02u", tm.hour, tm.min);
    else          snprintf(buf, sizeof buf, "--:--");

    if (!force && strcmp(buf, s_hdr_clock) == 0) return;
    snprintf(s_hdr_clock, sizeof s_hdr_clock, "%s", buf);

    const gb_theme_t *t = osconf_theme();
    const int W = st7789_width();
    st7789_fill_rect(W - CLOCK_W - 2, 3, CLOCK_W, 11, t->surface);
    st7789_text(W - st7789_text_width(buf, 1) - 4, 5, buf,
                tm.valid ? t->fg : t->dim, t->surface, 1);
}

static void paint_toast(void)
{
    const gb_theme_t *t = osconf_theme();
    const int W = st7789_width(), H = st7789_height();
    const int y = H - FOOT_H - 12;

    if (s_toast[0]) {
        int tw = st7789_text_width(s_toast, 1);
        st7789_fill_rect(0, y, W, 12, t->surface);
        st7789_text((W - tw) / 2, y + 2, s_toast, t->accent, t->surface, 1);
        s_toast_shown = true;
    } else if (s_toast_shown) {
        st7789_fill_rect(0, y, W, 12, t->bg);
        s_toast_shown = false;
    }
}

static void draw_list(void)
{
    const gb_theme_t *t = begin_paint();
    const int W = st7789_width(), H = st7789_height();
    const int rows = (H - TITLE_H - FOOT_H) / ROW_H;

    if (s_sel < s_top)          s_top = s_sel;
    if (s_sel >= s_top + rows)  s_top = s_sel - rows + 1;

    st7789_fill(t->bg);
    st7789_fill_rect(0, 0, W, TITLE_H, t->surface);
    st7789_text(4, 5, "greenbox", t->accent, t->surface, 1);
    paint_clock(true);

    if (s_n == 0) {
        st7789_text(8, TITLE_H + 14, "no programs in /progs", t->dim, t->bg, 1);
        st7789_text(8, TITLE_H + 30, "flash the spiffs image", t->muted, t->bg, 1);
    }

    char buf[16];
    for (int i = 0; i < rows && s_top + i < s_n; i++) {
        prog_t  *p   = &s_prog[s_top + i];
        bool     sel = (s_top + i) == s_sel;
        int      y   = TITLE_H + i * ROW_H;
        uint16_t bg  = sel ? t->surface : t->bg;
        uint16_t fg  = p->ok ? (sel ? t->fg : t->dim) : t->warn;

        st7789_fill_rect(0, y, W, ROW_H - 2, bg);
        if (sel) st7789_fill_rect(0, y, 3, ROW_H - 2, t->accent);
        st7789_text(8, y + 3, p->name, fg, bg, 2);

        snprintf(buf, sizeof buf, "%uk", (unsigned)((p->bytes + 1023) / 1024));
        st7789_text(W - st7789_text_width(buf, 1) - 4, y + 6, buf,
                    sel ? t->accent : t->muted, bg, 1);
    }

    /* Footer: the whole control scheme, permanently on screen. There are two
     * buttons and four gestures, which is exactly the situation that needs a
     * legend. */
    const char *foot = s_n ? "L/R move  R-hold run  L-hold info"
                           : "L-hold info";
    st7789_text(4, H - FOOT_H + 1, foot, t->muted, t->bg, 1);

    s_toast_shown = false;      /* the fill above wiped whatever was there */
    paint_toast();
}

/* Only the values, drawn with an opaque background so they can be refreshed
 * without clearing anything behind them. */
static void paint_info_values(void)
{
    const gb_theme_t *t = osconf_theme();
    char buf[48];
    int  y = TITLE_H + 2;

    snprintf(buf, sizeof buf, "heap  %-7u B free",
             (unsigned)esp_get_free_heap_size());
    st7789_text(4, y, buf, t->fg, t->bg, 1); y += 11;

    snprintf(buf, sizeof buf, "exec  %-7u B largest",
             (unsigned)guest_exec_free());
    st7789_text(4, y, buf, t->fg, t->bg, 1); y += 11;

    gb_tm_t tm;
    ostime_get(&tm);
    if (tm.valid)
        snprintf(buf, sizeof buf, "time  %04u-%02u-%02u %02u:%02u:%02u",
                 tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
    else
        /* Padded to the width of the set form so the two can overwrite each
         * other without clearing. `settime` over the console still works, but
         * the clock program can do it now with the buttons alone. */
        snprintf(buf, sizeof buf, "time  unset - set it in clock  ");
    st7789_text(4, y, buf, tm.valid ? t->fg : t->warn, t->bg, 1); y += 11;

    /* Uptime answers the question a flickering screen always raises: is this
     * thing rebooting behind my back? If this keeps returning to zero, it is. */
    uint32_t up = now_ms() / 1000;
    snprintf(buf, sizeof buf, "up    %uh %02um %02us  progs %d",
             (unsigned)(up / 3600), (unsigned)((up / 60) % 60),
             (unsigned)(up % 60), s_n);
    st7789_text(4, y, buf, t->dim, t->bg, 1); y += 11;

    /* Padded, like the time line, so that changing either half from the
     * settings program repaints over the old text instead of under it. */
    snprintf(buf, sizeof buf, "look  %-8s %-9s", t->name,
             (osconf_rotation() & 1) ? "landscape" : "portrait");
    st7789_text(4, y, buf, t->dim, t->bg, 1);
}

static void draw_info(void)
{
    const gb_theme_t *t = begin_paint();
    const int W = st7789_width(), H = st7789_height();

    st7789_fill(t->bg);
    st7789_fill_rect(0, 0, W, TITLE_H, t->surface);
    st7789_text(4, 5, "system", t->accent, t->surface, 1);
    paint_clock(true);

    /* Both fields are 32 bytes in the descriptor and the panel fits 40
     * characters at this size, so they are bounded here rather than left to
     * snprintf to truncate wherever it lands. */
    const esp_app_desc_t *app = esp_app_get_description();
    char buf[48];
    snprintf(buf, sizeof buf, "os %.14s  idf %.14s", app->version, app->idf_ver);
    st7789_text(4, H - FOOT_H - 11, buf, t->muted, t->bg, 1);
    st7789_text(4, H - FOOT_H + 1, "L-hold back", t->muted, t->bg, 1);

    paint_info_values();
}

/* -------------------------------------------------------------- running */

static void run_selected(void)
{
    if (s_n == 0) return;
    prog_t *p = &s_prog[s_sel];

    if (!p->ok) { toast("wrong ABI - rebuild it"); return; }

    /* Hand the guest a clean panel in the system orientation and the user's
     * background colour. Whether it keeps either is up to the guest. */
    st7789_fill(osconf_theme()->bg);
    esp_err_t err = guest_start(p->file);
    if (err != ESP_OK) {
        char buf[40];
        snprintf(buf, sizeof buf, "start failed: %s", esp_err_to_name(err));
        toast(buf);
        return;
    }

    /* Blocks for the guest's whole lifetime, including the hard kill path. */
    guest_supervise();

    char buf[40];
    snprintf(buf, sizeof buf, "%s exited", p->name);
    toast(buf);
}

bool launcher_request_run(const char *name)
{
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_prog[i].name, name) == 0) {
            snprintf(s_pending, sizeof s_pending, "%s", name);
            input_inject(GB_EV_NONE);   /* wake the launcher out of its wait */
            return true;
        }
    }
    return false;
}

void launcher_repaint(void)
{
    s_dirty = true;
    input_inject(GB_EV_NONE);       /* wake the launcher out of its wait */
}

/* ------------------------------------------------------------------ loop */

/* Everything that has to happen on a timer but must not repaint the screen. */
static void tick(void)
{
    if (s_toast[0] && (int32_t)(s_toast_until - now_ms()) <= 0) {
        s_toast[0] = 0;
        paint_toast();              /* erases just the toast strip */
    }

    if (s_info) paint_info_values();
    else        paint_clock(false);
}

void launcher_run(void)
{
    launcher_rescan();

    for (;;) {
        if (s_pending[0]) {
            for (int i = 0; i < s_n; i++)
                if (strcmp(s_prog[i].name, s_pending) == 0) { s_sel = i; break; }
            s_pending[0] = 0;
            run_selected();
            s_dirty = true;         /* the guest owned the panel */
        }

        if (s_dirty) {
            if (s_info) draw_info(); else draw_list();
            s_dirty = false;
        } else {
            tick();
        }

        /* The half-second timeout drives tick(), not a repaint. */
        switch (input_wait(500)) {
        case GB_EV_L_SHORT:
            if (!s_info && s_n) { s_sel = (s_sel + s_n - 1) % s_n; s_dirty = true; }
            break;

        case GB_EV_R_SHORT:
            if (!s_info && s_n) { s_sel = (s_sel + 1) % s_n; s_dirty = true; }
            break;

        case GB_EV_R_LONG:
            if (!s_info) { run_selected(); s_dirty = true; }
            break;

        case GB_EV_L_LONG:
            s_info = !s_info;       /* escape out of info, or into it from root */
            s_dirty = true;
            break;

        default:
            break;                  /* timeout, or a wake-up with nothing to do */
        }
    }
}
