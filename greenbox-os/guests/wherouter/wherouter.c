/*
 * wherouter.c - a signal-strength gauge for one radio, and a census to pick it
 * out of.
 *
 * Two screens. The list is a rolling survey of everything in earshot, sorted
 * strongest first, one hairline bar per network. Lock onto a row and the
 * second screen turns the whole panel into a meter for that BSSID alone: how
 * strong it is now, whether it is getting stronger, and what the last minute
 * of walking around looked like.
 *
 * Controls, on the list:
 *   L tap    previous row
 *   R tap    next row
 *   R hold   lock onto the row and hunt it
 *   L hold   leave
 *
 * It is a hot-and-cold finder, and it is deliberately not called trilateration
 * anywhere on screen. Trilateration wants three known positions and a distance
 * from each; a board in one hand has one position, no idea where it is, and a
 * distance estimate with a factor of two in it. What one antenna can honestly
 * do is tell you whether the last two steps helped, and that turns out to be
 * enough to walk up to a router - the gradient is the instrument, not the
 * number. The metres shown under the badge are a path-loss guess, labelled as
 * one.
 *
 * (802.11mc fine timing would give a real distance, in metres, off a single
 * exchange. The ESP32-D0WDQ6 on this board cannot do it - FTM arrived with the
 * S2, and every part that has it is a later one.)
 *
 * The whole program is passive. Nothing here transmits: the OS scans by
 * listening for beacons rather than by asking for them, and the watch is
 * promiscuous mode on one channel. See oswifi.c for why that is a decision
 * rather than a limitation.
 */

#include "wherouter.h"

const gb_api_t *A;
gb_theme_t      T;
int16_t         W, H;

/* --------------------------------------------------------------- the table */
/*
 * The list is not a scan result. A scan result is one channel at a time, and
 * sweeping thirteen of them takes a second and a half - long enough that a
 * list redrawn from the last scan alone would show a third of the networks in
 * the flat and blink the rest in and out. So the sweep feeds a table that
 * persists across channels, and rows leave it by growing old rather than by
 * being missing from one pass.
 */
#define AP_MAX     24
#define CHAN_MAX   13
#define AGE_STALE  8000u        /* drawn as faded past here */
#define AGE_DROP  25000u        /* forgotten past here */

typedef struct {
    gb_ap_t  ap;                /* as last heard, except rssi - see below */
    int16_t  ema;               /* dBm x16: what the bar and the sort use */
    uint32_t seen;              /* millis of the last beacon */
} row_t;

static row_t   s_row[AP_MAX];
static uint8_t s_nrow;

/* One channel's worth of scan. A crowded channel in a block of flats runs to a
 * dozen; asking for sixteen means the OS never has to drop one for lack of
 * room in the ordinary case. */
static gb_ap_t s_found[16];

static uint8_t s_chan = 1;      /* where the sweep is */
static uint8_t s_sel;           /* cursor, an index into s_row */
static uint8_t s_top;           /* first visible row */
static bool    s_dirty = true;

/* --------------------------------------------------------------- the ramp */

uint16_t sig_colour(int dbm)
{
    if (dbm >= -50) return GB_RGB(  0, 214,  96);   /* in the same room */
    if (dbm >= -60) return GB_RGB(150, 214,  40);
    if (dbm >= -70) return GB_RGB(232, 200,  32);
    if (dbm >= -80) return GB_RGB(240, 138,  32);
    return                 GB_RGB(228,  72,  60);   /* the far edge of it */
}

int16_t sig_bar(int dbm, int16_t span)
{
    if (dbm >= RSSI_HOT)  return span;
    if (dbm <= RSSI_COLD) return 0;
    return (int16_t)(((long)(dbm - RSSI_COLD) * span) / (RSSI_HOT - RSSI_COLD));
}

/*
 * Log-distance path loss: d = 10^((P1 - rssi) / 10n), with P1 = -40 dBm at one
 * metre and n = 2.7, which is the usual indoor-with-walls exponent. Tabulated
 * every 5 dB and interpolated between, because there is no powf in a guest and
 * because a curve with a factor of two of honest error in it does not deserve
 * more resolution than that.
 *
 * What makes it a guess rather than a measurement is P1: transmit power varies
 * by 6 dB across ordinary routers, antenna gain by as much again, and a body
 * standing between the two costs 3-6 dB on its own. So the number moves the
 * right way and it is worth roughly what "warm" is worth. The gradient beside
 * it is the part to trust.
 */
static const uint16_t DIST_CM[14] = {   /* index = (-40 - rssi) / 5 */
      100,   153,   235,   359,   550,   843,  1291,
     1977,  3028,  4638,  7104, 10880, 16664, 25524,
};

uint16_t sig_dist_dm(int dbm)
{
    int d = -40 - dbm;
    if (d < 0)  d = 0;
    if (d > 64) d = 64;

    int i = d / 5, f = d % 5;
    uint32_t cm = DIST_CM[i];
    if (f) cm += ((uint32_t)(DIST_CM[i + 1] - DIST_CM[i]) * f) / 5;
    return (uint16_t)(cm / 10);         /* decimetres */
}

/* ------------------------------------------------------------------- text */

void fit_text(char *dst, int dstsz, const char *src, int16_t maxpx, uint8_t size)
{
    int room = maxpx / (6 * (size ? size : 1));
    if (room > dstsz - 1) room = dstsz - 1;
    if (room < 0) room = 0;

    int i = 0;
    while (i < room && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void ap_label(char *dst, int dstsz, const gb_ap_t *ap)
{
    if (ap->ssid[0]) { fit_text(dst, dstsz, ap->ssid, 32000, 1); return; }
    A->snprintf(dst, (size_t)dstsz, "<%02X%02X%02X>",
                ap->bssid[3], ap->bssid[4], ap->bssid[5]);
}

const char *pick_fit(const char *const *cand, int n, int16_t maxpx)
{
    for (int i = 0; i < n; i++)
        if (A->text_width(cand[i], 1) <= maxpx) return cand[i];
    return cand[n - 1];
}

/* --------------------------------------------------------------- the sweep */

static int find_row(const uint8_t *bssid)
{
    for (int i = 0; i < s_nrow; i++)
        if (memcmp(s_row[i].ap.bssid, bssid, GB_BSSID_LEN) == 0) return i;
    return -1;
}

/*
 * One reading folded in. The EMA is what the bar draws and what the sort uses,
 * because a single beacon can land 6 dB off the one before it - the same
 * scatter that makes a raw list reorder itself every pass. Four samples of
 * memory is about a second and a half of standing still, which is short enough
 * that walking into the next room still moves the row.
 */
static void row_feed(row_t *r, const gb_ap_t *ap, uint32_t now)
{
    int16_t target = (int16_t)(ap->rssi * 16);
    r->ema  += (int16_t)((target - r->ema) / 4);
    r->seen  = now;

    /* The record itself is taken as read - a beacon is the AP describing
     * itself, and one that changed its name or its channel between passes has
     * genuinely changed them. So r->ap.rssi holds the last raw reading and
     * r->ema holds the number anything on screen actually uses. */
    r->ap = *ap;
}

static void sweep_channel(void)
{
    uint32_t now = A->millis();

    int n = A->wifi_scan(s_found, (int)(sizeof s_found / sizeof s_found[0]),
                         s_chan, 0);
    for (int i = 0; i < n; i++) {
        int at = find_row(s_found[i].bssid);

        if (at < 0) {
            if (s_nrow < AP_MAX) {
                at = s_nrow++;
                s_row[at].ema = (int16_t)(s_found[i].rssi * 16);
            } else {
                /* The table is full and something has to lose. The weakest
                 * row goes, and only if this one beats it - a table that
                 * evicted whatever it found last would churn forever in a
                 * block of flats. */
                int weak = 0;
                for (int k = 1; k < s_nrow; k++)
                    if (s_row[k].ema < s_row[weak].ema) weak = k;
                if (s_found[i].rssi * 16 <= s_row[weak].ema) continue;
                at = weak;
                s_row[at].ema = (int16_t)(s_found[i].rssi * 16);
            }
        }
        row_feed(&s_row[at], &s_found[i], now);
    }

    /* Age-out, in place. A row that has not beaconed in twenty-five seconds
     * has either been switched off or is two rooms away through a wall, and
     * either way its last strength is a lie by now. */
    for (int i = 0; i < s_nrow; ) {
        if (now - s_row[i].seen > AGE_DROP) {
            s_row[i] = s_row[--s_nrow];
        } else {
            i++;
        }
    }
}

/*
 * Sorted at the end of a sweep and at no other time.
 *
 * Re-sorting after every channel would be correct and unusable: rows would
 * change places eight times a second, under a cursor the user is trying to
 * aim, for differences of a decibel. Once per sweep is slow enough to read and
 * still fast enough that walking towards something visibly promotes it.
 *
 * The cursor is pinned to a BSSID across the sort rather than to an index,
 * for the same reason - the row the user is looking at is the row they meant,
 * wherever the sort has just put it.
 */
static void sort_rows(void)
{
    uint8_t keep[GB_BSSID_LEN];
    bool    had = s_sel < s_nrow;
    if (had) memcpy(keep, s_row[s_sel].ap.bssid, GB_BSSID_LEN);

    for (int i = 1; i < s_nrow; i++) {
        row_t v = s_row[i];
        int j = i - 1;
        while (j >= 0 && s_row[j].ema < v.ema) { s_row[j + 1] = s_row[j]; j--; }
        s_row[j + 1] = v;
    }

    if (had) {
        int at = find_row(keep);
        s_sel = (uint8_t)(at < 0 ? 0 : at);
    }
    if (s_sel >= s_nrow) s_sel = s_nrow ? (uint8_t)(s_nrow - 1) : 0;
}

/* ------------------------------------------------------------ the list ui */

#define ROW_H 16

static uint8_t rows_fit(void)
{
    int n = (H - TITLE_H - FOOT_H) / ROW_H;
    return (uint8_t)(n < 1 ? 1 : n);
}

static void scroll_to_cursor(void)
{
    uint8_t vis = rows_fit();
    if (s_sel < s_top)            s_top = s_sel;
    if (s_sel >= s_top + vis)     s_top = (uint8_t)(s_sel - vis + 1);
    if (s_top + vis > s_nrow)     s_top = (uint8_t)(s_nrow > vis ? s_nrow - vis : 0);
}

static void draw_row(int idx, int16_t y)
{
    const row_t *r   = &s_row[idx];
    bool  sel        = (idx == s_sel);
    int   dbm        = r->ema / 16;
    bool  stale      = (A->millis() - r->seen) > AGE_STALE;

    uint16_t bg = sel ? T.surface : T.bg;
    A->fill_rect(0, y, W, ROW_H, bg);
    if (sel) A->fill_rect(0, y, 2, ROW_H, T.accent);

    /* Right-hand block first, because it is fixed width and the name gets
     * whatever is left. "c6 -63" reads as one field at a glance and costs six
     * characters, which is what 135 columns can spare. */
    char meta[12];
    A->snprintf(meta, sizeof meta, "c%-2u %3d", r->ap.channel, dbm);
    int16_t mw = A->text_width(meta, 1);
    A->text((int16_t)(W - mw - 4), (int16_t)(y + 2), meta,
            stale ? T.muted : T.dim, bg, 1);

    char name[34], cut[34];
    ap_label(name, sizeof name, &r->ap);
    fit_text(cut, sizeof cut, name, (int16_t)(W - mw - 14), 1);
    A->text(6, (int16_t)(y + 2), cut,
            stale ? T.muted : (sel ? T.fg : T.dim), bg, 1);

    /* The bar is one pixel tall on purpose. A row is two lines of information
     * and a rule under them; making the rule mean something costs no height at
     * all, and a column of them down the screen is a shape the eye reads
     * before it has read a single number. */
    int16_t span = (int16_t)(W - 12);
    int16_t len  = sig_bar(dbm, span);
    A->fill_rect(6, (int16_t)(y + 12), span, 1, bg);
    if (len) A->fill_rect(6, (int16_t)(y + 12), len,
                          1, stale ? T.muted : sig_colour(dbm));
}

static void draw_footer(void)
{
    static const char *const cand[] = {
        "L/R pick   R-hold lock   L-hold exit",
        "L/R pick  R-hold lock  L-hold exit",
        "L/R  R-hold lock  L-hold exit",
        "L/R  R:lock  L:exit",
    };
    A->fill_rect(0, (int16_t)(H - FOOT_H), W, FOOT_H, T.bg);
    A->text(4, (int16_t)(H - FOOT_H + 3),
            pick_fit(cand, (int)(sizeof cand / sizeof cand[0]), (int16_t)(W - 8)),
            T.muted, T.bg, 1);
}

static void draw_list(void)
{
    char buf[20];

    A->fill_rect(0, 0, W, TITLE_H, T.surface);
    A->text(4, 3, "wherouter", T.accent, T.surface, 1);

    /* The channel the sweep is on doubles as the only progress indicator this
     * screen needs: it counts to thirteen and starts again, so a radio that
     * has stopped answering is a number that has stopped moving. Portrait
     * cannot spell it out beside the title, so it gets the short form rather
     * than losing the indicator, which is the half worth keeping. */
    A->snprintf(buf, sizeof buf, "%u nets  c%02u", s_nrow, s_chan);
    if (A->text_width(buf, 1) > W - 66)
        A->snprintf(buf, sizeof buf, "%u/c%02u", s_nrow, s_chan);
    int16_t bw = A->text_width(buf, 1);
    A->text((int16_t)(W - bw - 4), 3, buf, T.dim, T.surface, 1);

    uint8_t vis = rows_fit();
    for (uint8_t i = 0; i < vis; i++) {
        int16_t y = (int16_t)(TITLE_H + i * ROW_H);
        int idx = s_top + i;
        if (idx < s_nrow) draw_row(idx, y);
        else              A->fill_rect(0, y, W, ROW_H, T.bg);
    }

    if (s_nrow == 0)
        A->text(6, (int16_t)(TITLE_H + 8), "listening for beacons...",
                T.muted, T.bg, 1);

    draw_footer();
}

/* ------------------------------------------------------------------- main */

/* The last thing locked onto, so the next run starts with the cursor on it.
 * Not the screen - resuming straight into the hunt would be a program that
 * comes up pointing at something that may have been switched off since. */
typedef struct {
    uint8_t bssid[GB_BSSID_LEN];
    uint8_t channel;
    uint8_t pad;
    char    ssid[GB_SSID_MAX];
} lock_t;

static void lock_save(const gb_ap_t *ap)
{
    lock_t l;
    memset(&l, 0, sizeof l);
    memcpy(l.bssid, ap->bssid, GB_BSSID_LEN);
    l.channel = ap->channel;
    memcpy(l.ssid, ap->ssid, sizeof l.ssid - 1);
    A->store_put("lock", &l, sizeof l);
}

static bool s_have_lock;
static lock_t s_lock;

static void splash(const char *msg, uint16_t colour)
{
    A->fill(T.bg);
    A->fill_rect(0, 0, W, TITLE_H, T.surface);
    A->text(4, 3, "wherouter", T.accent, T.surface, 1);
    A->text(6, (int16_t)(TITLE_H + 10), msg, colour, T.bg, 1);
}

int gb_main(const gb_api_t *api)
{
    A = api;

    gb_oscfg_t cfg;
    api->oscfg_get(&cfg);
    if (!api->theme_get(cfg.theme, &T) && !api->theme_get(0, &T)) return 1;

    /* No set_rotation anywhere in this program. A list is a list either way up
     * and the gauge lays itself out from the panel it is given, so the user's
     * orientation is honoured by doing nothing at all. */
    W = api->width();
    H = api->height();

    s_have_lock = api->store_get("lock", &s_lock, sizeof s_lock) == (int)sizeof s_lock;

    splash("waking the radio...", T.dim);
    if (!api->wifi_power(true)) {
        splash("the radio would not start", T.warn);
        while (!api->should_stop())
            if (api->wait_event(250) == GB_EV_L_LONG) break;
        return 1;
    }

    A->fill(T.bg);
    draw_list();

    while (!api->should_stop()) {
        /*
         * One channel per pass, then the events. A full sweep in one call
         * would be a second and a half in which a tap is neither seen nor
         * answered; a hundred and twenty milliseconds is under the threshold
         * where a button feels stuck.
         */
        sweep_channel();

        if (++s_chan > CHAN_MAX) {
            s_chan = 1;
            sort_rows();

            /* The cursor lands on the remembered target the first time that
             * target is heard, and never moves on its own again. */
            if (s_have_lock) {
                int at = find_row(s_lock.bssid);
                if (at >= 0) { s_sel = (uint8_t)at; s_have_lock = false; }
            }
            s_dirty = true;
        }

        scroll_to_cursor();
        if (s_dirty) { draw_list(); s_dirty = false; }
        else         { draw_footer(); }

        gb_event_t e;
        while ((e = api->poll_event()) != GB_EV_NONE) {
            switch (e) {
            case GB_EV_L_SHORT:
                if (s_nrow) s_sel = (uint8_t)((s_sel + s_nrow - 1) % s_nrow);
                s_dirty = true;
                break;

            case GB_EV_R_SHORT:
                if (s_nrow) s_sel = (uint8_t)((s_sel + 1) % s_nrow);
                s_dirty = true;
                break;

            case GB_EV_R_LONG:
                if (s_nrow) {
                    gb_ap_t target = s_row[s_sel].ap;
                    lock_save(&target);
                    if (!finder_run(&target)) return 0;
                    /* Back from the hunt. The table kept ageing in absentia,
                     * so everything in it is stale by definition - start the
                     * sweep at the top rather than wherever it was. */
                    s_chan  = 1;
                    s_dirty = false;
                    A->fill(T.bg);
                    draw_list();
                }
                break;

            case GB_EV_L_LONG:
                return 0;

            default:
                break;
            }
        }
    }

    return 0;
}
