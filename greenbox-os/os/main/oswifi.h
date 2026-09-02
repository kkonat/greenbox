/*
 * oswifi.h - the radio, brought up only while a guest is asking.
 *
 * Everything here backs the four wifi_* entries in gapi.c and nothing else
 * calls it. See greenbox_abi.h for what a guest is promised; this header is
 * about what the OS owes the guest that asked.
 *
 * The radio is off at boot and off again the moment the guest that woke it
 * exits. It is not free: about 21 KB of heap while it runs, measured on the
 * board, and that is with the buffers in sdkconfig.defaults cut to what a
 * receiver needs. Nothing else on this board asks for that much and then
 * stops needing it, which is why it has a power switch at all.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "greenbox_abi.h"

/* Listen time per channel when a guest asks for the default. A beacon
 * interval is 102.4 ms, so anything under that is a coin toss about whether
 * an AP that is plainly there answers at all. */
#define OSWIFI_DWELL_DEFAULT 120

bool oswifi_power(bool on);
int  oswifi_scan(gb_ap_t *out, int max, uint8_t channel, uint16_t dwell_ms);
bool oswifi_watch(const uint8_t *bssid, uint8_t channel);
int  oswifi_watch_poll(gb_hit_t *out, int max);

/* Called by the loader when a guest ends, kill or no kill. Powers the radio
 * down and forgets the watch, so the next program starts with the heap the
 * first one found. Safe when the radio was never up. */
void oswifi_release(void);

/* True while the radio is powered - for the console's heap report, which is
 * otherwise a mystery when 21 KB has quietly gone missing. */
bool oswifi_is_up(void);
