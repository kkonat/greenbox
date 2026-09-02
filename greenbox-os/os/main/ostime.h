/*
 * ostime.h - the one wall clock in the system.
 *
 * The OS owns time so that guests do not each have to. There is no 32.768 kHz
 * crystal on this board, so the RTC runs off the internal 150 kHz RC
 * oscillator and drifts on the order of minutes per day: treat any unattended
 * reading as approximate until something authoritative (NTP, later) sets it.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "greenbox_abi.h"

void     ostime_init(void);          /* restores the last known time from NVS */
void     ostime_set(uint32_t unix_utc);

/* Set from broken-down LOCAL time - the same fields ostime_get produces, so a
 * read-modify-write round-trips. .wday and .valid are ignored. False means the
 * fields were rejected and the clock was left alone; see the range check in
 * ostime.c for what "rejected" covers. */
bool     ostime_set_local(const gb_tm_t *tm);
uint32_t ostime_unix(void);          /* 0 until something sets it */
bool     ostime_valid(void);
void     ostime_get(gb_tm_t *out);   /* local time, per the stored offset */

/* Minutes east of UTC, persisted. */
void     ostime_set_tz(int16_t minutes);
int16_t  ostime_tz(void);
