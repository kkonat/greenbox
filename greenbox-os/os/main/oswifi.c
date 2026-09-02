/*
 * oswifi.c - listening, and only listening.
 *
 * The radio costs about 21 KB of heap while it runs - measured on the board,
 * against 263 KB free when a guest starts - and that is with the buffers in
 * sdkconfig.defaults cut down to what a receiver needs. So it is off at boot,
 * brought up by the first guest that asks, and dropped again by
 * guest_supervise() when that guest ends, kill or no kill, so a program that
 * dies mid-scan does not leave the radio running behind it.
 *
 * Nothing here transmits.
 *
 * That is a decision, not an accident of the implementation. An active scan
 * puts a probe request on every channel it visits, which turns a survey into
 * emission on channels the board has no idea whether it is allowed to use -
 * 12 and 13 exist in some regions and not others, and the ESP32's default
 * country is the conservative one. A passive scan just listens for the
 * beacons that are already in the air, which costs a beacon interval per
 * channel instead of a round trip and answers exactly the question a survey
 * asks. The watch below is passive for the same reason and for a second one:
 * a probe response tells you the AP heard YOU, and what a hot-and-cold finder
 * wants to measure is the other direction.
 *
 * The two modes are exclusive because the hardware is: one radio, one channel.
 * A scan therefore cancels a watch and does not put it back, which is stated
 * in the ABI rather than hidden here - a guest that interleaves them decides
 * for itself when the radio goes back to its target.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "oswifi.h"
#include "guest.h"

static const char *TAG = "oswifi";

#define CHAN_MAX  13

static bool s_up;                       /* esp_wifi_init + start have run */
static bool s_loop;                     /* we created the default event loop */
static bool s_watching;
static volatile bool s_scanning;

static SemaphoreHandle_t s_scan_done;
static esp_event_handler_instance_t s_scan_h;

/* ------------------------------------------------------------- the ring */
/*
 * One producer - the promiscuous callback, which runs in the WiFi task - and
 * one consumer, the guest. The critical section is per element rather than
 * around the whole drain: a guest polling at 10 Hz can take 64 frames at once
 * and there is no reason to hold off the radio for the length of that copy.
 */
static gb_hit_t s_ring[GB_HIT_RING];
static volatile uint8_t s_head, s_tail;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_target[GB_BSSID_LEN];

static void ring_reset(void)
{
    portENTER_CRITICAL(&s_ring_mux);
    s_head = s_tail = 0;
    portEXIT_CRITICAL(&s_ring_mux);
}

static void ring_push(const gb_hit_t *h)
{
    portENTER_CRITICAL(&s_ring_mux);
    uint8_t next = (uint8_t)((s_head + 1) % GB_HIT_RING);
    if (next == s_tail)                        /* full: the oldest goes */
        s_tail = (uint8_t)((s_tail + 1) % GB_HIT_RING);
    s_ring[s_head] = *h;
    s_head = next;
    portEXIT_CRITICAL(&s_ring_mux);
}

/* ------------------------------------------------------------ the watch */

/*
 * Runs in the WiFi task, once per frame on a busy channel, so it does the
 * least it can: one address compare, and a push if it matches.
 *
 * addr2 is the transmitter, and it sits at offset 10 of every management and
 * data frame. Control frames do not all have one - an ACK is ten bytes long
 * and carries only a receiver - which is why the filter below asks for MGMT
 * and DATA and nothing else. The length check is belt and braces on that.
 */
static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_watching) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    if (p->rx_ctrl.sig_len < 24) return;

    const uint8_t *hdr = p->payload;
    if (memcmp(hdr + 10, s_target, GB_BSSID_LEN) != 0) return;

    gb_hit_t h = {
        .t_ms     = (uint32_t)(esp_timer_get_time() / 1000),
        .rssi     = (int8_t)p->rx_ctrl.rssi,
        .channel  = (uint8_t)p->rx_ctrl.channel,
        .reserved = 0,
        /* Frame control byte 0: type in bits 2-3, subtype in 4-7. Management
         * subtype 8 is a beacon, which is the one frame an AP sends whether
         * or not anybody is using it - and therefore the one worth timing. */
        .kind     = (type == WIFI_PKT_DATA) ? GB_HIT_DATA
                  : ((hdr[0] & 0xF0) == 0x80 ? GB_HIT_BEACON : GB_HIT_MGMT),
    };
    ring_push(&h);
}

static void watch_off(void)
{
    if (!s_watching) return;
    esp_wifi_set_promiscuous(false);
    s_watching = false;
}

/* --------------------------------------------------------------- power */

static void on_scan_done(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    if (s_scan_done) xSemaphoreGive(s_scan_done);
}

/*
 * No esp_netif is created. An interface exists to give the station an address,
 * and this API never associates with anything, so the TCP/IP plumbing would be
 * set up and never used. esp_wifi_start() in STA mode is perfectly happy
 * without one attached - it just has nowhere to deliver a packet, which is the
 * point. (esp_wifi_init still logs lwIP's mailbox sizes on the way up: lwIP is
 * a default IDF component and is linked whether or not anything asks it for an
 * interface. Nothing here does.)
 */
static bool wifi_up(void)
{
    esp_err_t err;

    if (!s_scan_done) {
        s_scan_done = xSemaphoreCreateBinary();
        if (!s_scan_done) return false;
    }

    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        s_loop = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              on_scan_done, NULL, &s_scan_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "handler: %s", esp_err_to_name(err));
        goto fail_loop;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* Nothing to remember: no SSID, no key, no autoconnect. Leaving this on
     * would have the driver write to the same NVS partition the OS keeps its
     * settings and its clock in, every time a guest woke the radio. */
    cfg.nvs_enable = false;

    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: %s", esp_err_to_name(err));
        goto fail_handler;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    /* 1..13, because a passive scan is the same thing as sitting on a channel
     * with the receiver on: no emission, so no channel here is one the board
     * could use wrongly. A guest still only ever sees the channels that
     * actually answered. */
    wifi_country_t country = {
        .cc = "01", .schan = 1, .nchan = CHAN_MAX,
        .max_tx_power = 20, .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    esp_wifi_set_country(&country);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        goto fail_init;
    }

    /* Modem sleep parks the receiver between the beacons of an AP we are not
     * associated to - which is every AP here - and a finder that samples a
     * duty cycle instead of a signal is measuring the wrong thing. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_up = true;
    ESP_LOGI(TAG, "radio up for %s", guest_name());
    return true;

fail_init:
    esp_wifi_deinit();
fail_handler:
    esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, s_scan_h);
    s_scan_h = NULL;
fail_loop:
    if (s_loop) { esp_event_loop_delete_default(); s_loop = false; }
    return false;
}

static void wifi_down(void)
{
    watch_off();
    if (s_scanning) { esp_wifi_scan_stop(); s_scanning = false; }
    esp_wifi_clear_ap_list();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_scan_h) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, s_scan_h);
        s_scan_h = NULL;
    }
    /* The default loop is a task and a queue of its own. Deleting it is what
     * makes "the radio costs nothing while it is down" true rather than
     * nearly true - nothing else in the OS posts events. */
    if (s_loop) { esp_event_loop_delete_default(); s_loop = false; }

    s_up = false;
    ESP_LOGI(TAG, "radio down");
}

bool oswifi_power(bool on)
{
    if (on == s_up) return true;
    if (on) return wifi_up();
    wifi_down();
    return true;
}

bool oswifi_is_up(void) { return s_up; }

void oswifi_release(void)
{
    if (s_up) wifi_down();
    ring_reset();
    memset(s_target, 0, sizeof s_target);
}

/* ---------------------------------------------------------------- scan */

/*
 * Keep `out` sorted strongest first, capped at `max`, dropping the weakest
 * when it overflows. Doing it as an insertion is what lets the OS answer
 * without allocating: the alternative - pull the whole scan list into a
 * buffer, sort it, copy the top of it out - is a kilobyte or two of heap to
 * arrive at the same answer, taken from the guest that asked.
 */
static int insert_ap(gb_ap_t *out, int n, int max, const wifi_ap_record_t *r)
{
    if (n == max && r->rssi <= out[max - 1].rssi) return n;

    int at = n;
    while (at > 0 && out[at - 1].rssi < r->rssi) at--;
    if (at >= max) return n;

    int last = (n < max) ? n : max - 1;
    for (int i = last; i > at; i--) out[i] = out[i - 1];

    gb_ap_t *a = &out[at];
    memset(a, 0, sizeof *a);
    memcpy(a->bssid, r->bssid, GB_BSSID_LEN);
    a->channel = r->primary;
    a->rssi    = r->rssi;

    switch (r->authmode) {
    case WIFI_AUTH_OPEN:            a->auth = GB_AUTH_OPEN;       break;
    case WIFI_AUTH_WEP:             a->auth = GB_AUTH_WEP;        break;
    case WIFI_AUTH_WPA_PSK:         a->auth = GB_AUTH_WPA;        break;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:    a->auth = GB_AUTH_WPA2;       break;
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE:
    case WIFI_AUTH_OWE:             a->auth = GB_AUTH_WPA3;       break;
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:    a->auth = GB_AUTH_ENTERPRISE; break;
    default:                        a->auth = GB_AUTH_OTHER;      break;
    }

    memcpy(a->ssid, r->ssid, sizeof a->ssid - 1);
    a->ssid[sizeof a->ssid - 1] = 0;
    /* A hidden AP beacons with a zero-length SSID, so this is the whole test.
     * Passive scanning cannot uncover the name - that would take a probe, and
     * a probe is a transmission. */
    a->hidden = a->ssid[0] ? 0 : 1;

    return (n < max) ? n + 1 : max;
}

int oswifi_scan(gb_ap_t *out, int max, uint8_t channel, uint16_t dwell_ms)
{
    if (!out || max <= 0) return 0;
    if (!s_up && !wifi_up()) return -1;

    watch_off();            /* one radio, one channel */

    if (dwell_ms == 0)   dwell_ms = OSWIFI_DWELL_DEFAULT;
    if (dwell_ms < 40)   dwell_ms = 40;
    if (dwell_ms > 1500) dwell_ms = 1500;
    if (channel > CHAN_MAX) channel = 0;

    wifi_scan_config_t sc = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = channel,
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time   = { .passive = dwell_ms },
    };

    xSemaphoreTake(s_scan_done, 0);         /* a stale signal is not this scan */
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) return -1;
    s_scanning = true;

    /*
     * Waited out in slices rather than blocked on, because a full sweep is a
     * second and a half and the OS may want the guest gone inside that.
     * Abandoning gives back an empty answer rather than a partial one: half a
     * survey, sorted as though it were a whole one, reads as a list of
     * networks that have gone away.
     */
    const uint32_t chans  = channel ? 1u : (uint32_t)CHAN_MAX;
    const uint32_t budget = (dwell_ms + 60u) * chans + 400u;

    bool done = false;
    for (uint32_t waited = 0; waited < budget; waited += 25) {
        if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(25)) == pdTRUE) { done = true; break; }
        if (guest_stop_requested()) break;
    }
    s_scanning = false;

    if (!done) {
        esp_wifi_scan_stop();
        esp_wifi_clear_ap_list();
        return 0;
    }

    int n = 0;
    wifi_ap_record_t r;
    /* Drained to the end even once `out` is full: the driver holds the list
     * until it is read out or cleared, and records left in it would come back
     * as part of the next scan's answer. */
    while (esp_wifi_scan_get_ap_record(&r) == ESP_OK)
        n = insert_ap(out, n, max, &r);
    esp_wifi_clear_ap_list();

    return n;
}

/* --------------------------------------------------------------- watch */

bool oswifi_watch(const uint8_t *bssid, uint8_t channel)
{
    if (!bssid) { watch_off(); ring_reset(); return true; }
    if (channel < 1 || channel > CHAN_MAX) return false;
    if (!s_up && !wifi_up()) return false;

    memcpy(s_target, bssid, GB_BSSID_LEN);
    ring_reset();

    if (!s_watching) {
        wifi_promiscuous_filter_t f = {
            .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
        };
        esp_wifi_set_promiscuous_filter(&f);
        /* Callback before the mode, not after: promiscuous with nothing
         * registered is a window in which frames arrive and go nowhere. */
        esp_wifi_set_promiscuous_rx_cb(promisc_cb);
        if (esp_wifi_set_promiscuous(true) != ESP_OK) return false;
        s_watching = true;
    }

    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK;
}

int oswifi_watch_poll(gb_hit_t *out, int max)
{
    if (!s_watching) return -1;
    if (!out || max <= 0) return 0;

    int n = 0;
    while (n < max) {
        bool got = false;
        portENTER_CRITICAL(&s_ring_mux);
        if (s_tail != s_head) {
            out[n] = s_ring[s_tail];
            s_tail = (uint8_t)((s_tail + 1) % GB_HIT_RING);
            got = true;
        }
        portEXIT_CRITICAL(&s_ring_mux);
        if (!got) break;
        n++;
    }
    return n;
}
