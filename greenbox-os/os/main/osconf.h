/*
 * osconf.h - the OS's own settings, and the palettes they choose between.
 *
 * Two knobs today: the orientation the system lives in, and the colour theme
 * everything is painted with. Both are the user's choice, made once in the
 * settings program and expected to stick, so both are persisted.
 *
 * The theme type itself is an ABI type - guests get the same table through
 * api->theme_get(), so a program can paint in the user's palette instead of
 * inventing its own.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "greenbox_abi.h"

/* Loads the saved settings, or the defaults if there are none. Must run after
 * nvs_flash_init and before the panel is brought up - st7789_init wants the
 * orientation. */
void osconf_init(void);

/* The system orientation: 0,2 portrait 135x240;  1,3 landscape 240x135. */
uint8_t osconf_rotation(void);

/* The live palette. Never NULL, valid for the lifetime of the OS, and the
 * pointer moves when the theme changes - read it, do not cache it. */
const gb_theme_t *osconf_theme(void);

uint8_t osconf_theme_count(void);
bool    osconf_theme_at(uint8_t idx, gb_theme_t *out);

void osconf_get(gb_oscfg_t *out);

/* Validates, applies and persists. False means one of the fields was out of
 * range and nothing was stored, not even the field that was fine. Nothing on
 * the panel is touched: the launcher picks the new orientation and palette up
 * on its next full repaint, and a guest that wants to see a rotation change
 * immediately asks for it with set_rotation(). */
bool osconf_set(const gb_oscfg_t *in);
