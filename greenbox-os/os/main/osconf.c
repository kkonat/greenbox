/*
 * osconf.c - the settings record and the theme table.
 *
 * Kept in NVS rather than in a file on /progs, for a boring but decisive
 * reason: /progs is a SPIFFS image that the OS build generates and
 * `idf.py flash` rewrites wholesale, so anything stored there would be wiped
 * every time the OS is reflashed - which is exactly when a user is most likely
 * to have just set their preferences. NVS is left alone by an app flash and
 * already holds the clock and the guests' key/value blobs.
 *
 * Two u8 keys rather than one blob of gb_oscfg_t, so that adding a field later
 * is a new key and not a migration: an old record simply leaves the new field
 * at its default.
 *
 * About the palettes: seven roles, chosen so that a screen can be laid out
 * without ever picking a raw colour. bg and surface are the two backgrounds,
 * fg/dim/muted are three weights of text on either of them, accent is the one
 * bright thing on screen and warn is the only red-ish. Every theme keeps those
 * relationships, which is what makes the launcher legible in all five without
 * a single per-theme special case.
 */

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "board.h"
#include "osconf.h"

static const char *TAG = "osconf";

/* Same namespace as ostime - one place for everything that belongs to the OS
 * itself, with distinct keys. Guests get their own namespaces elsewhere. */
static const char *NS      = "gbos";
static const char *K_ROT   = "rot";
static const char *K_THEME = "theme";

/* --------------------------------------------------------------- themes */

static const gb_theme_t s_themes[] = {
    {   /* midnight - the original navy-and-cyan look, cleaned up a little */
        .bg      = GB_RGB( 0,  0,  0),
        .surface = GB_RGB(18, 26, 80),
        .fg      = GB_RGB(240,244,255),
        .dim     = GB_RGB(150,160,200),
        .muted   = GB_RGB( 72, 80,120),
        .accent  = GB_RGB(  0,220,255),
        .warn    = GB_RGB(255, 90, 80),
        .name    = "midnight",
    },
    {   /* amber - a warm monochrome, near enough a VT220 to feel like one */
        .bg      = GB_RGB( 12,  8,  0),
        .surface = GB_RGB( 64, 38,  4),
        .fg      = GB_RGB(255,196, 72),
        .dim     = GB_RGB(196,136, 44),
        .muted   = GB_RGB(104, 68, 20),
        .accent  = GB_RGB(255,232,140),
        .warn    = GB_RGB(255, 84, 40),
        .name    = "amber",
    },
    {   /* forest */
        .bg      = GB_RGB(  4, 14, 10),
        .surface = GB_RGB( 16, 60, 40),
        .fg      = GB_RGB(216,246,228),
        .dim     = GB_RGB(128,186,152),
        .muted   = GB_RGB( 60,104, 84),
        .accent  = GB_RGB( 96,232,144),
        .warn    = GB_RGB(255,152, 64),
        .name    = "forest",
    },
    {   /* slate - the quiet one, for when the board is just sitting there */
        .bg      = GB_RGB( 14, 17, 23),
        .surface = GB_RGB( 44, 52, 68),
        .fg      = GB_RGB(226,232,240),
        .dim     = GB_RGB(152,162,182),
        .muted   = GB_RGB( 88, 98,118),
        .accent  = GB_RGB(124,172,255),
        .warn    = GB_RGB(255,124,112),
        .name    = "slate",
    },
    {   /* plum */
        .bg      = GB_RGB( 18,  8, 26),
        .surface = GB_RGB( 62, 28, 76),
        .fg      = GB_RGB(246,228,252),
        .dim     = GB_RGB(188,148,208),
        .muted   = GB_RGB(108, 72,124),
        .accent  = GB_RGB(255,124,204),
        .warn    = GB_RGB(255,184, 72),
        .name    = "plum",
    },
};

#define THEME_N ((uint8_t)(sizeof s_themes / sizeof s_themes[0]))

/* ---------------------------------------------------------------- state */

static gb_oscfg_t s_cfg = {
    .rotation = TD_OS_ROTATION,
    .theme    = 0,
};

static bool valid(const gb_oscfg_t *c)
{
    return c && c->rotation <= 3 && c->theme < THEME_N;
}

void osconf_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        gb_oscfg_t got = s_cfg;
        nvs_get_u8(h, K_ROT,   &got.rotation);
        nvs_get_u8(h, K_THEME, &got.theme);
        nvs_close(h);

        /* A record written by a future OS with more themes must not leave the
         * palette pointer dangling, so anything out of range falls back to the
         * defaults rather than being clamped into something the user never
         * picked. */
        if (valid(&got)) s_cfg = got;
        else ESP_LOGW(TAG, "stored settings out of range - using defaults");
    }

    ESP_LOGI(TAG, "rotation %u (%s), theme %u (%s)",
             s_cfg.rotation, (s_cfg.rotation & 1) ? "landscape" : "portrait",
             s_cfg.theme, s_themes[s_cfg.theme].name);
}

uint8_t osconf_rotation(void) { return s_cfg.rotation; }

const gb_theme_t *osconf_theme(void) { return &s_themes[s_cfg.theme]; }

uint8_t osconf_theme_count(void) { return THEME_N; }

bool osconf_theme_at(uint8_t idx, gb_theme_t *out)
{
    if (!out || idx >= THEME_N) return false;
    *out = s_themes[idx];
    return true;
}

void osconf_get(gb_oscfg_t *out)
{
    if (out) *out = s_cfg;
}

bool osconf_set(const gb_oscfg_t *in)
{
    if (!valid(in)) return false;

    s_cfg = *in;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        /* The setting still applies for this session - failing to remember it
         * is not a reason to also refuse to do it. */
        ESP_LOGW(TAG, "nvs_open failed - settings will not survive a reboot");
        return true;
    }
    nvs_set_u8(h, K_ROT,   s_cfg.rotation);
    nvs_set_u8(h, K_THEME, s_cfg.theme);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "rotation %u, theme %s",
             s_cfg.rotation, s_themes[s_cfg.theme].name);
    return true;
}
