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
int16_t  st7789_width(void);
int16_t  st7789_height(void);

void     st7789_backlight(bool on);

void     st7789_fill(uint16_t colour);
void     st7789_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t colour);
void     st7789_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t colour);
void     st7789_pixel(int16_t x, int16_t y, uint16_t colour);

/* Push a caller-owned RGB565 buffer of w*h pixels. */
void     st7789_blit(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels);

/* Built-in 5x7 font, scaled by `size`. Returns the x just past the text. */
int16_t  st7789_text(int16_t x, int16_t y, const char *s,
                     uint16_t fg, uint16_t bg, uint8_t size);
int16_t  st7789_text_width(const char *s, uint8_t size);
