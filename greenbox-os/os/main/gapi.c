/*
 * gapi.c - the syscall table handed to guests.
 *
 * Every entry is a thin shim rather than the OS function itself. Three reasons
 * that is worth the indirection:
 *
 *   - the guard. A short, hardware-touching call is bracketed so the loader
 *     can tell whether it is safe to delete the guest task. Blocking calls
 *     (wait_event, sleep_ms) are deliberately NOT guarded: holding the guard
 *     while parked would make every kill wait out the guest's frame timer.
 *   - allocation tracking, so a killed guest cannot leak.
 *   - the table layout stays under this file's control, independent of how the
 *     OS happens to spell its internals today.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"

#include "st7789.h"
#include "input.h"
#include "guest.h"
#include "ostime.h"
#include "osconf.h"
#include "oswifi.h"
#include "osgfx.h"

static const char *TAG = "gapi";

/* ---------------------------------------------------------------- display */

#define GUARD(call) do { guest_syscall_enter(); call; guest_syscall_exit(); } while (0)

static int16_t a_width(void)  { return st7789_width();  }
static int16_t a_height(void) { return st7789_height(); }

static void a_fill(uint16_t c) { GUARD(st7789_fill(c)); }

static void a_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c)
{ GUARD(st7789_fill_rect(x, y, w, h, c)); }

static void a_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c)
{ GUARD(st7789_rect(x, y, w, h, c)); }

static void a_pixel(int16_t x, int16_t y, uint16_t c)
{ GUARD(st7789_pixel(x, y, c)); }

static void a_blit(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px)
{ GUARD(st7789_blit(x, y, w, h, px)); }

static int16_t a_text(int16_t x, int16_t y, const char *s,
                      uint16_t fg, uint16_t bg, uint8_t size)
{
    int16_t r;
    guest_syscall_enter();
    r = st7789_text(x, y, s, fg, bg, size);
    guest_syscall_exit();
    return r;
}

static int16_t a_text_width(const char *s, uint8_t size)
{ return st7789_text_width(s, size); }

static void a_backlight(bool on) { GUARD(st7789_backlight(on)); }

/* A guest that wants the other orientation - a vertical scroller, say - asks
 * for it here rather than being stuck with the launcher's. Nothing is cleared:
 * the panel keeps whatever pixels it had, at the new addressing, and the guest
 * repaints. guest_supervise() puts the system orientation back on the way out,
 * so a program with a fixed layout can decline the user's choice for as long
 * as it runs without having to restore anything. */
static void a_set_rotation(uint8_t rot) { GUARD(st7789_set_rotation(rot & 3)); }

/* ------------------------------------------------------------------ input */

static gb_event_t a_poll_event(void)
{
    gb_event_t e = input_poll();
    return e == GB_EV_KILL ? GB_EV_NONE : e;    /* kill is never a guest event */
}

/* Unguarded, like the two above it: a read of two bytes that touches no
 * hardware and cannot be caught holding the SPI bus. A guest polls this once a
 * frame, so the mutex would cost more than the call. */
static uint8_t a_buttons(void) { return input_buttons(); }

static gb_event_t a_wait_event(uint32_t timeout_ms)
{
    gb_event_t e = input_wait(timeout_ms);
    return e == GB_EV_KILL ? GB_EV_NONE : e;
}

/* ------------------------------------------------------------------- time */

static uint32_t a_millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void     a_get_time(gb_tm_t *out) { ostime_get(out); }
static uint32_t a_unix_time(void) { return ostime_unix(); }

/* Guarded: it commits to NVS, and a task deleted halfway through that is
 * exactly the case the guard exists for. */
static bool a_set_time(const gb_tm_t *tm)
{
    bool ok;
    guest_syscall_enter();
    ok = ostime_set_local(tm);
    guest_syscall_exit();
    return ok;
}

static void a_sleep_ms(uint32_t ms)
{
    /* Chopped up so a kill lands within a tick or two even if the guest asked
     * to sleep for a minute. */
    while (ms && !guest_stop_requested()) {
        uint32_t slice = ms > 50 ? 50 : ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
    }
}

/* -------------------------------------------------------------- lifecycle */

static bool a_should_stop(void) { return guest_stop_requested(); }

static void a_log(const char *msg)
{
    ESP_LOGI(TAG, "[%s] %s", guest_name(), msg ? msg : "");
}

static int a_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* ----------------------------------------------------------- persistence */

/* NVS namespaces are capped at 15 characters, so a guest's name is truncated
 * rather than hashed - collisions between "clock" and "clockwork" would need
 * a 13-character shared prefix, which is a problem for another day. */
static void ns_for_guest(char *out, size_t n)
{
    snprintf(out, n, "g_%.12s", guest_name());
}

static int a_store_get(const char *key, void *buf, size_t len)
{
    char ns[16];
    ns_for_guest(ns, sizeof ns);

    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t sz = len;
    esp_err_t err = nvs_get_blob(h, key, buf, &sz);
    nvs_close(h);
    return err == ESP_OK ? (int)sz : -1;
}

static int a_store_put(const char *key, const void *buf, size_t len)
{
    if (len > GB_STORE_MAX) len = GB_STORE_MAX;

    char ns[16];
    ns_for_guest(ns, sizeof ns);

    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? (int)len : -1;
}

/* -------------------------------------------------------- OS settings */

/* Read-only calls, so no guard: osconf keeps its record in RAM and copies it
 * out under no lock at all. Two bytes written by one task, read by another,
 * with no invariant spanning them - the worst a race can produce is last
 * frame's orientation. */
static void a_oscfg_get(gb_oscfg_t *out) { osconf_get(out); }

/* Guarded, because it commits to NVS - same reasoning as set_time. Note what
 * it deliberately does NOT do: rotate the panel. A guest that changes the
 * orientation setting is usually previewing it and wants to control exactly
 * when the screen turns, so it calls set_rotation() itself; and one that is
 * only writing the preference should not yank the display out from under its
 * own half-drawn frame. */
static bool a_oscfg_set(const gb_oscfg_t *in)
{
    bool ok;
    guest_syscall_enter();
    ok = osconf_set(in);
    guest_syscall_exit();
    return ok;
}

static uint8_t a_theme_count(void) { return osconf_theme_count(); }

static bool a_theme_get(uint8_t idx, gb_theme_t *out)
{ return osconf_theme_at(idx, out); }


/* ------------------------------------------------------------------ radio */
/*
 * Unguarded, all four, and for the opposite of the usual reason. The guard
 * exists so a kill never deletes a task holding the SPI bus; none of these
 * touch the panel, and wifi_scan can park for a second and a half. Holding
 * the guard across that would make every kill wait out a sweep, which is
 * precisely the case the guard was meant to keep quick.
 *
 * What replaces it is that the radio's state lives in oswifi, not on the
 * guest's stack. A guest deleted mid-scan leaves a scan running and nothing
 * else; oswifi_release() stops it on the way out. And oswifi_scan watches
 * guest_stop_requested() between slices, so the ordinary case is that the
 * scan gives up on its own before anybody has to kill anything.
 */

static bool a_wifi_power(bool on) { return oswifi_power(on); }

static int a_wifi_scan(gb_ap_t *out, int max, uint8_t channel, uint16_t dwell_ms)
{
    if (max > GB_SCAN_MAX) max = GB_SCAN_MAX;
    return oswifi_scan(out, max, channel, dwell_ms);
}

static bool a_wifi_watch(const uint8_t *bssid, uint8_t channel)
{ return oswifi_watch(bssid, channel); }

static int a_wifi_watch_poll(gb_hit_t *out, int max)
{ return oswifi_watch_poll(out, max); }

/* --------------------------------------------------------------- graphics */
/*
 * No shim at all, and no guard: api->gfx is a pointer straight at the static
 * table in osgfx.c. Every routine behind it writes into a buffer the guest
 * owns and passes in, so there is no hardware to be caught holding, nothing to
 * track and nothing to reclaim - which is the whole reason the rasteriser was
 * allowed across the boundary in the first place. Putting the finished band on
 * the glass is still a_blit, and that one is guarded like everything else that
 * touches the bus.
 */

/* ------------------------------------------------------------- the table */

const gb_api_t g_gb_api = {
    .abi_version = GB_ABI_VERSION,

    .width       = a_width,
    .height      = a_height,
    .fill        = a_fill,
    .fill_rect   = a_fill_rect,
    .rect        = a_rect,
    .pixel       = a_pixel,
    .blit        = a_blit,
    .text        = a_text,
    .text_width  = a_text_width,
    .backlight   = a_backlight,
    .set_rotation = a_set_rotation,

    .poll_event  = a_poll_event,
    .wait_event  = a_wait_event,
    .buttons     = a_buttons,

    .millis      = a_millis,
    .get_time    = a_get_time,
    .unix_time   = a_unix_time,
    .sleep_ms    = a_sleep_ms,
    .set_time    = a_set_time,

    .should_stop = a_should_stop,
    .log         = a_log,
    .snprintf    = a_snprintf,

    .alloc       = guest_track_alloc,
    .free        = guest_track_free,

    .store_get   = a_store_get,
    .store_put   = a_store_put,

    .oscfg_get   = a_oscfg_get,
    .oscfg_set   = a_oscfg_set,
    .theme_count = a_theme_count,
    .theme_get   = a_theme_get,

    .wifi_power      = a_wifi_power,
    .wifi_scan       = a_wifi_scan,
    .wifi_watch      = a_wifi_watch,
    .wifi_watch_poll = a_wifi_watch_poll,

    .gfx             = &g_gb_gfx,
};
