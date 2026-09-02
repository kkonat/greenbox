/*
 * ostime.c - wall clock kept as an offset from the monotonic timer.
 *
 * Nothing here talks to the RTC peripheral directly: esp_timer already gives a
 * monotonic microsecond count that survives everything except a power cycle,
 * so wall time is just that plus a base captured whenever someone sets it.
 * The base is written to NVS on every set and reloaded at boot, which is not
 * accurate - it does not account for the time the board was off - but it does
 * mean the clock comes back showing roughly the right decade instead of 1970.
 */

#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "ostime.h"

static const char *TAG   = "time";
static const char *NS    = "gbos";
static const char *K_UNIX = "unix";
static const char *K_TZ   = "tz";

static uint32_t s_base_unix;      /* wall time at s_base_us */
static int64_t  s_base_us;
static bool     s_valid;
static int16_t  s_tz_min;

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, K_UNIX, ostime_unix());
    nvs_set_i16(h, K_TZ, s_tz_min);
    nvs_commit(h);
    nvs_close(h);
}

void ostime_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t saved = 0;
        if (nvs_get_u32(h, K_UNIX, &saved) == ESP_OK && saved > 1600000000u) {
            s_base_unix = saved;
            s_base_us   = esp_timer_get_time();
            s_valid     = true;      /* stale by however long the board was off */
            ESP_LOGW(TAG, "restored %u from NVS - stale, resync when you can",
                     (unsigned)saved);
        }
        nvs_get_i16(h, K_TZ, &s_tz_min);
        nvs_close(h);
    }
}

void ostime_set(uint32_t unix_utc)
{
    s_base_unix = unix_utc;
    s_base_us   = esp_timer_get_time();
    s_valid     = true;
    persist();
    ESP_LOGI(TAG, "set to %u UTC (tz %+d min)", (unsigned)unix_utc, s_tz_min);
}

/* Days since 1970-01-01 for a proleptic Gregorian date, by Hinnant's
 * algorithm: shift the year to start in March so the leap day falls at the end
 * of it, and the day-of-year becomes a closed form with no table and no loop.
 * Written out here rather than reached for in libc because timegm is a newlib
 * extension and mktime would drag in the TZ database this OS deliberately does
 * not have - everything else in this file treats local time as UTC plus a
 * stored offset, and this has to agree with that. */
static int32_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int32_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                 /* 0..399   */
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;  /* 0..146096 */
    return era * 146097 + (int32_t)doe - 719468;
}

static unsigned days_in_month(unsigned y, unsigned m)
{
    static const uint8_t len[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
    return len[m - 1];
}

bool ostime_set_local(const gb_tm_t *tm)
{
    if (!tm) return false;

    /* The lower bound is not arbitrary: ostime_init refuses to restore
     * anything at or before 2020-09, so a time this function accepted below
     * that would silently come back unset after a reboot. */
    if (tm->year < 2021 || tm->year > 2099)               return false;
    if (tm->mon  < 1    || tm->mon  > 12)                 return false;
    if (tm->day  < 1    || tm->day  > days_in_month(tm->year, tm->mon))
                                                          return false;
    if (tm->hour > 23 || tm->min > 59 || tm->sec > 59)    return false;

    int64_t local = (int64_t)days_from_civil(tm->year, tm->mon, tm->day) * 86400
                  + tm->hour * 3600 + tm->min * 60 + tm->sec;

    /* ostime_get shifts UTC east by s_tz_min to produce these fields, so
     * undoing it is the whole conversion. */
    ostime_set((uint32_t)(local - (int64_t)s_tz_min * 60));
    return true;
}

uint32_t ostime_unix(void)
{
    if (!s_valid) return 0;
    return s_base_unix + (uint32_t)((esp_timer_get_time() - s_base_us) / 1000000);
}

bool ostime_valid(void) { return s_valid; }

void ostime_set_tz(int16_t minutes)
{
    s_tz_min = minutes;
    persist();
}

int16_t ostime_tz(void) { return s_tz_min; }

void ostime_get(gb_tm_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);

    uint32_t u = ostime_unix();
    out->valid = s_valid;
    if (!s_valid) { out->year = 1970; out->mon = 1; out->day = 1; return; }

    time_t   t = (time_t)u + s_tz_min * 60;
    struct tm tm;
    gmtime_r(&t, &tm);               /* already shifted, so gm not local */

    out->year = (uint16_t)(tm.tm_year + 1900);
    out->mon  = (uint8_t)(tm.tm_mon + 1);
    out->day  = (uint8_t)tm.tm_mday;
    out->hour = (uint8_t)tm.tm_hour;
    out->min  = (uint8_t)tm.tm_min;
    out->sec  = (uint8_t)tm.tm_sec;
    out->wday = (uint8_t)tm.tm_wday;
}
