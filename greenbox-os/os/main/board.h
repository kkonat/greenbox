/*
 * board.h - LILYGO TTGO T-Display (ESP32 + ST7789 135x240 IPS)
 *
 * Every value here was verified against the actual board on COM5, not copied
 * from a datasheet: the panel by sweeping configs until it rendered, the
 * buttons by pressing them, the orientation by the user telling us which is
 * which.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ panel */
#define TD_SCLK        18
#define TD_MOSI        19
#define TD_DC          16
#define TD_CS           5
#define TD_RST         23
#define TD_BL           4        /* backlight, active HIGH */

/* Native panel is portrait. The ST7789 controller has 240x320 of RAM but this
 * panel only exposes a 135x240 window inside it, hence the offsets. */
#define TD_PANEL_W    135
#define TD_PANEL_H    240

/* The panel is IPS, so it needs INVON (0x21) or everything comes out negative. */
#define TD_IS_IPS       1

/* ---------------------------------------------------------------- buttons */
/* Both active LOW. Physical orientation with the USB port facing down:
 *   GPIO0  is the LEFT button
 *   GPIO35 is the RIGHT button
 *
 * GPIO35 is input-only and the ESP32 has NO internal pull-up on GPIO34-39;
 * it works only because the board provides an external one. GPIO0 is also the
 * boot strap pin - holding it during reset drops the chip into the serial
 * bootloader, which is exactly how flashing works, so don't hold it at boot
 * unless you mean it.
 */
#define TD_BTN_LEFT     0
#define TD_BTN_RIGHT   35

/* -------------------------------------------------------------- rotation */
/* The orientation the OS starts life in. It is only the factory default now -
 * the live value is the one in the settings record (osconf.c), which the user
 * changes in the settings program and which survives a reboot. A guest may ask
 * for a different one through api->set_rotation(); the OS puts the user's back
 * when the guest exits. */
#define TD_OS_ROTATION  1        /* 1 = landscape 240x135 */

/* ------------------------------------------------------------------ misc */
/* This board has no user LED. The backlight on GPIO4 is the only thing you can
 * blink, which is why the probe used it as one. */
