/*
 * st7789.h - minimal hand-written ST7789 driver for the TTGO T-Display.
 *
 * No display library. Commands are the datasheet opcodes, sent over the IDF's
 * spi_master with DC toggled by hand.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* RGB565, which is what the panel wants once COLMOD is set to 0x55. */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BLACK   RGB565(0,0,0)
#define C_WHITE   RGB565(255,255,255)
#define C_RED     RGB565(255,0,0)
#define C_GREEN   RGB565(0,255,0)
#define C_BLUE    RGB565(0,0,255)
#define C_YELLOW  RGB565(255,255,0)
#define C_CYAN    RGB565(0,255,255)
#define C_MAGENTA RGB565(255,0,255)
#define C_GREY    RGB565(128,128,128)
#define C_DKGREY  RGB565(48,48,48)
#define C_NAVY    RGB565(0,0,128)
#define C_ORANGE  RGB565(255,140,0)

/* rotation: 0,2 = portrait 135x240;  1,3 = landscape 240x135 */
void     st7789_init(uint8_t rotation);

/* False if init failed - check this instead of drawing into the void. */
bool     st7789_ready(void);
void     st7789_set_rotation(uint8_t rotation);

/* What the panel is set to right now, which is not always what the OS settings
 * ask for: a guest may have rotated it, and the launcher syncs the two back up
 * on its next repaint. */
uint8_t  st7789_rotation(void);
int16_t  st7789_width(void);
int16_t  st7789_height(void);

void     st7789_backlight(bool on);

void     st7789_fill(uint16_t colour);
void     st7789_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t colour);
void     st7789_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t colour);
void     st7789_pixel(int16_t x, int16_t y, uint16_t colour);

/* ------------------------------------------------------- the OS's own rows
 *
 * The OS sometimes has to put something on the panel that a running guest must
 * not be able to rub out - the quit bar of a left hold, drawn from the input
 * task over whatever the guest has on screen. Feedback that a full-screen
 * repaint erases forty times a second is not feedback.
 *
 * So the strip is taken away from everyone else instead of being redrawn in a
 * race with them: while `rows` is non-zero, every primitive in this file is
 * clipped to the panel *above* the bottom `rows` scanlines. Guests notice
 * nothing - width() and height() keep reporting the whole panel, so nothing
 * reflows when the strip appears - their pixels in those rows simply do not
 * reach the glass.
 *
 * The OS lifts the clip for its own painting with st7789_reserve_override(),
 * which is the whole of the priority: one writer can draw there, and it is not
 * the guest. Both calls are cheap, and the reservation is expressed in the
 * current rotation's coordinates, so it stays at the bottom of the screen
 * through a rotation change. */
void     st7789_reserve(int16_t rows);
int16_t  st7789_reserved(void);
void     st7789_reserve_override(bool on);

/* Push a caller-owned RGB565 buffer of w*h pixels. Rows that fall into the
 * reserved strip are dropped. */
void     st7789_blit(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels);

/* Built-in 5x7 font, scaled by `size`. Returns the x just past the text -
 * which is the real advance even for glyphs the reserved strip clipped away,
 * so a caller laying out a line gets the same answer either way. */
int16_t  st7789_text(int16_t x, int16_t y, const char *s,
                     uint16_t fg, uint16_t bg, uint8_t size);
int16_t  st7789_text_width(const char *s, uint8_t size);
