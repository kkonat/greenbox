/*
 * display_probe - identify an unknown ESP32 board's display, and verify its buttons.
 *
 * Raw ESP-IDF. No Arduino, no TFT_eSPI, no Arduino_GFX. The panel init sequences
 * below are hand-written command bytes straight from the controller datasheets,
 * and the button polling reads the GPIO input registers directly.
 *
 * Order of operations (buttons first, so the confirm channel is proven before
 * it is needed):
 *   PHASE 1  poll both buttons, then flash every candidate LED / backlight pin
 *   PHASE 2  sweep candidate (pins + controller + geometry) combinations
 *   PHASE 3  the moment you SEE something, press a button -> config is reported
 *
 * Target: ESP32, IDF v5.2.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "esp_log.h"

static const char *TAG = "probe";

/* ------------------------------------------------------------------ buttons */
/* Both active LOW. GPIO35 is input-only and has no internal pull-up (true of
 * GPIO34-39 on the ESP32), so it relies on the board's external pull-up.
 * GPIO0 is the boot strap pin and carries a pull-up on essentially every board. */
#define BTN_A   35
#define BTN_B   0

/* Read the GPIO input registers directly rather than going through the driver. */
static inline bool pin_is_low(int pin)
{
    if (pin < 32) return !((REG_READ(GPIO_IN_REG)  >> pin)        & 1U);
    else          return !((REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1U);
}

static inline bool any_button_down(void)
{
    return pin_is_low(BTN_A) || pin_is_low(BTN_B);
}

/* Debounced wait. Returns the pin that was pressed, or -1 if timeout_ms elapsed. */
static int wait_for_button(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        int pin = pin_is_low(BTN_A) ? BTN_A : (pin_is_low(BTN_B) ? BTN_B : -1);
        if (pin >= 0) {
            vTaskDelay(pdMS_TO_TICKS(30));           /* debounce */
            if (pin_is_low(pin)) {
                while (pin_is_low(pin)) vTaskDelay(pdMS_TO_TICKS(10));   /* release */
                return pin;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    return -1;
}

/* ------------------------------------------------------------- candidate LEDs */
/* A TTGO T-Display has no user LED, but its backlight sits on GPIO4 and is just
 * as visible - if the panel glows, that pin is correct. */
static const struct { int pin; const char *what; } LEDS[] = {
    {  4, "GPIO4  - TFT backlight on TTGO T-Display" },
    {  2, "GPIO2  - onboard LED on many ESP32 boards" },
    { 25, "GPIO25 - LED on some LILYGO boards" },
};
#define N_LEDS (sizeof(LEDS)/sizeof(LEDS[0]))

/* ------------------------------------------------------------------ variants */
typedef enum { DRV_ST7789, DRV_ST7735, DRV_ILI9341 } drv_t;

typedef struct {
    const char *name;
    drv_t   drv;
    int     sck, mosi, dc, cs, rst, bl;
    int     w, h;
    int     col_off, row_off;
    bool    invert;          /* IPS panels need INVON */
} variant_t;

static const variant_t VARIANTS[] = {
    /* the prime suspect - pin map taken from TFT_eSPI's Setup25_TTGO_T_Display.h */
    { "TTGO T-Display 135x240 ST7789", DRV_ST7789, 18, 19, 16,  5, 23,  4, 135, 240, 52, 40, true  },
    { "T-Display rotated 240x135",     DRV_ST7789, 18, 19, 16,  5, 23,  4, 240, 135, 40, 53, true  },
    { "ST7789 240x240",                DRV_ST7789, 18, 19, 16,  5, 23,  4, 240, 240,  0,  0, true  },
    { "ST7789 240x320",                DRV_ST7789, 18, 19, 16,  5, 23,  4, 240, 320,  0,  0, true  },
    { "ST7735 128x160",                DRV_ST7735, 18, 19, 16,  5, 23,  4, 128, 160,  2,  1, false },
    { "ILI9341 240x320",               DRV_ILI9341,18, 19, 16,  5, 23,  4, 240, 320,  0,  0, false },
    /* alternate wiring seen on generic ESP32+TFT boards */
    { "generic CS15 DC2 RST4 ST7789",  DRV_ST7789, 14, 13,  2, 15,  4, 27, 240, 240,  0,  0, true  },
    { "generic HSPI ILI9341",          DRV_ILI9341,18, 23,  2, 15,  4, 32, 240, 320,  0,  0, false },
};
#define N_VARIANTS (sizeof(VARIANTS)/sizeof(VARIANTS[0]))

#define SHOW_MS 5000            /* how long each guess stays on screen */

/* --------------------------------------------------------------- SPI plumbing */
static spi_device_handle_t s_spi = NULL;
static bool s_bus_up = false;
static int  s_dc = -1;

static void spi_write(const uint8_t *data, size_t len, bool is_data)
{
    if (len == 0) return;
    gpio_set_level(s_dc, is_data ? 1 : 0);

    /* spi_master caps a single transfer; chunk to stay under it. */
    const size_t MAX = 2048;
    while (len) {
        size_t n = len > MAX ? MAX : len;
        spi_transaction_t t = { 0 };
        t.length    = n * 8;
        t.tx_buffer = data;
        spi_device_polling_transmit(s_spi, &t);
        data += n;
        len  -= n;
    }
}

static void cmd(uint8_t c)                          { spi_write(&c, 1, false); }
static void dat(const uint8_t *d, size_t n)         { spi_write(d, n, true); }
static void cmd_dat(uint8_t c, const uint8_t *d, size_t n) { cmd(c); if (n) dat(d, n); }

/* --------------------------------------------------------- panel init, by hand */

static void panel_reset(const variant_t *v)
{
    if (v->rst < 0) return;
    gpio_set_level(v->rst, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(v->rst, 0); vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(v->rst, 1); vTaskDelay(pdMS_TO_TICKS(150));
}

static void init_st7789(const variant_t *v)
{
    cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));          /* SWRESET */
    cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));          /* SLPOUT  */
    cmd_dat(0x3A, (const uint8_t[]){0x55}, 1);          /* COLMOD  16bpp/65k */
    cmd_dat(0x36, (const uint8_t[]){0x00}, 1);          /* MADCTL  */
    cmd(v->invert ? 0x21 : 0x20);                       /* INVON / INVOFF */
    cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));           /* NORON   */
    cmd(0x29); vTaskDelay(pdMS_TO_TICKS(120));          /* DISPON  */
}

static void init_st7735(const variant_t *v)
{
    cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));          /* SWRESET */
    cmd(0x11); vTaskDelay(pdMS_TO_TICKS(255));          /* SLPOUT  */
    cmd_dat(0x3A, (const uint8_t[]){0x05}, 1);          /* COLMOD  16bpp */
    cmd_dat(0x36, (const uint8_t[]){0xC8}, 1);          /* MADCTL  */
    if (v->invert) cmd(0x21);
    cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));
    cmd(0x29); vTaskDelay(pdMS_TO_TICKS(120));          /* DISPON  */
}

static void init_ili9341(const variant_t *v)
{
    cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));          /* SWRESET */
    cmd_dat(0xCF, (const uint8_t[]){0x00,0x83,0x30}, 3);
    cmd_dat(0xED, (const uint8_t[]){0x64,0x03,0x12,0x81}, 4);
    cmd_dat(0xE8, (const uint8_t[]){0x85,0x01,0x79}, 3);
    cmd_dat(0xCB, (const uint8_t[]){0x39,0x2C,0x00,0x34,0x02}, 5);
    cmd_dat(0xF7, (const uint8_t[]){0x20}, 1);
    cmd_dat(0xEA, (const uint8_t[]){0x00,0x00}, 2);
    cmd_dat(0xC0, (const uint8_t[]){0x26}, 1);          /* power control 1 */
    cmd_dat(0xC1, (const uint8_t[]){0x11}, 1);
    cmd_dat(0xC5, (const uint8_t[]){0x35,0x3E}, 2);     /* VCOM */
    cmd_dat(0xC7, (const uint8_t[]){0xBE}, 1);
    cmd_dat(0x36, (const uint8_t[]){0x48}, 1);          /* MADCTL */
    cmd_dat(0x3A, (const uint8_t[]){0x55}, 1);          /* COLMOD 16bpp */
    cmd_dat(0xB1, (const uint8_t[]){0x00,0x1B}, 2);
    cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));          /* SLPOUT */
    cmd(0x29); vTaskDelay(pdMS_TO_TICKS(120));          /* DISPON */
}

/* ------------------------------------------------------------------- drawing */

static void set_window(const variant_t *v, int x, int y, int w, int h)
{
    int x0 = x + v->col_off, x1 = x + w - 1 + v->col_off;
    int y0 = y + v->row_off, y1 = y + h - 1 + v->row_off;
    uint8_t b[4];

    b[0] = x0 >> 8; b[1] = x0 & 0xFF; b[2] = x1 >> 8; b[3] = x1 & 0xFF;
    cmd_dat(0x2A, b, 4);                                /* CASET */
    b[0] = y0 >> 8; b[1] = y0 & 0xFF; b[2] = y1 >> 8; b[3] = y1 & 0xFF;
    cmd_dat(0x2B, b, 4);                                /* RASET */
    cmd(0x2C);                                          /* RAMWR */
}

static void fill_rect(const variant_t *v, int x, int y, int w, int h, uint16_t colour)
{
    if (w <= 0 || h <= 0) return;
    set_window(v, x, y, w, h);

    /* one row of pixels, big-endian as the panel expects, reused per line */
    static uint8_t line[512 * 2];
    int px = w > 512 ? 512 : w;
    for (int i = 0; i < px; i++) { line[i*2] = colour >> 8; line[i*2+1] = colour & 0xFF; }

    gpio_set_level(s_dc, 1);
    for (int row = 0; row < h; row++) {
        int left = w;
        while (left) {
            int n = left > px ? px : left;
            spi_transaction_t t = { 0 };
            t.length = n * 2 * 8;
            t.tx_buffer = line;
            spi_device_polling_transmit(s_spi, &t);
            left -= n;
        }
    }
}

#define RGB565(r,g,b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))

/* Unmistakable pattern: bright background, colour bars, and (idx+1) white
 * blocks so you can tell which guess you are looking at without any font. */
static void draw_probe_screen(const variant_t *v, int idx)
{
    static const uint16_t BG[] = {
        RGB565(0,0,160), RGB565(0,120,0), RGB565(150,0,0),
        RGB565(120,0,120), RGB565(0,110,110), RGB565(150,80,0),
        RGB565(60,60,60), RGB565(0,0,0),
    };
    uint16_t bg = BG[idx % (sizeof(BG)/sizeof(BG[0]))];

    fill_rect(v, 0, 0, v->w, v->h, bg);

    /* colour bars along the top - also exposes RGB/BGR inversion */
    static const uint16_t BARS[] = {
        RGB565(255,0,0), RGB565(0,255,0), RGB565(0,0,255),
        RGB565(255,255,255), RGB565(255,255,0), RGB565(255,0,255),
    };
    int nb = sizeof(BARS)/sizeof(BARS[0]);
    int bw = v->w / nb;
    for (int i = 0; i < nb; i++) fill_rect(v, i*bw, 0, bw, 18, BARS[i]);

    /* 1px frame - a missing edge means col_off/row_off are wrong */
    uint16_t fr = RGB565(255,255,0);
    fill_rect(v, 0, 0, v->w, 1, fr);
    fill_rect(v, 0, v->h - 1, v->w, 1, fr);
    fill_rect(v, 0, 0, 1, v->h, fr);
    fill_rect(v, v->w - 1, 0, 1, v->h, fr);

    /* idx+1 white blocks = which variant this is */
    int bs = 14, gap = 6, y = 30;
    for (int i = 0; i <= idx; i++) {
        int x = 6 + i * (bs + gap);
        if (x + bs >= v->w) break;
        fill_rect(v, x, y, bs, bs, RGB565(255,255,255));
    }
}

/* ------------------------------------------------------- bring a variant up */

static bool bring_up(const variant_t *v)
{
    if (s_spi)   { spi_bus_remove_device(s_spi); s_spi = NULL; }
    if (s_bus_up){ spi_bus_free(SPI3_HOST); s_bus_up = false; }

    /* control pins as plain outputs */
    uint64_t mask = (1ULL << v->dc);
    if (v->rst >= 0) mask |= (1ULL << v->rst);
    if (v->bl  >= 0) mask |= (1ULL << v->bl);
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) return false;
    if (v->bl >= 0) gpio_set_level(v->bl, 1);           /* backlight on */
    s_dc = v->dc;

    spi_bus_config_t bus = {
        .mosi_io_num     = v->mosi,
        .miso_io_num     = -1,
        .sclk_io_num     = v->sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    s_bus_up = true;

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = v->cs,
        .queue_size     = 4,
    };
    if (spi_bus_add_device(SPI3_HOST, &dev, &s_spi) != ESP_OK) return false;

    panel_reset(v);
    switch (v->drv) {
        case DRV_ST7789:  init_st7789(v);  break;
        case DRV_ST7735:  init_st7735(v);  break;
        case DRV_ILI9341: init_ili9341(v); break;
    }
    return true;
}

/* ----------------------------------------------------------------- phase 1 */

static void phase1_buttons(void)
{
    printf("\n========================================\n");
    printf("  PHASE 1a - BUTTON CHECK (done first)\n");
    printf("========================================\n");

    if (pin_is_low(BTN_A)) printf("WARN: GPIO35 already LOW - stuck, or no external pull-up\n");
    if (pin_is_low(BTN_B)) printf("WARN: GPIO0 already LOW - stuck button\n");

    const struct { int pin; const char *label; } bs[] = {
        { BTN_A, "GPIO35 (one of the two front buttons)" },
        { BTN_B, "GPIO0  (the other front button)" },
    };
    for (int i = 0; i < 2; i++) {
        printf("\n  Press %s   (20s, or wait to skip)\n", bs[i].label);
        int t = 0; bool got = false;
        while (t < 20000) {
            if (pin_is_low(bs[i].pin)) {
                vTaskDelay(pdMS_TO_TICKS(30));
                if (pin_is_low(bs[i].pin)) {
                    int held = 0;
                    while (pin_is_low(bs[i].pin)) { vTaskDelay(pdMS_TO_TICKS(10)); held += 10; }
                    printf("    OK - detected, held %d ms\n", held);
                    got = true; break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10)); t += 10;
        }
        if (!got) printf("    no press seen - absent, different pin, or dead\n");
    }
}

static void phase1_leds(void)
{
    printf("\n========================================\n");
    printf("  PHASE 1b - FLASHING CANDIDATE LED PINS\n");
    printf("========================================\n");
    printf("Watch the board. The screen lighting up counts as a flash.\n");

    for (int i = 0; i < N_LEDS; i++) {
        printf("  -> %s\n", LEDS[i].what);
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << LEDS[i].pin),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        for (int n = 0; n < 6; n++) {
            gpio_set_level(LEDS[i].pin, 1); vTaskDelay(pdMS_TO_TICKS(150));
            gpio_set_level(LEDS[i].pin, 0); vTaskDelay(pdMS_TO_TICKS(150));
        }
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    /* leave every candidate backlight on for the sweep */
    for (int i = 0; i < N_LEDS; i++) gpio_set_level(LEDS[i].pin, 1);
    printf("All candidate backlight pins left HIGH.\n");
}

/* ----------------------------------------------------------------- phase 2 */

static int phase2_sweep(void)
{
    printf("\n========================================\n");
    printf("  PHASE 2 - LCD VARIANT SWEEP\n");
    printf("========================================\n");
    printf("Watch the screen. The instant you see ANYTHING, press either button.\n");
    printf("Each guess holds for %d s, then loops.\n\n", SHOW_MS / 1000);

    for (int pass = 1; ; pass++) {
        printf("--- pass %d ---\n", pass);
        for (int i = 0; i < N_VARIANTS; i++) {
            const variant_t *v = &VARIANTS[i];
            printf("  #%d  %s ... ", i + 1, v->name);
            fflush(stdout);

            if (!bring_up(v)) { printf("bring-up FAILED\n"); vTaskDelay(pdMS_TO_TICKS(200)); continue; }
            draw_probe_screen(v, i);
            printf("ON SCREEN NOW (%d white blocks)\n", i + 1);

            if (wait_for_button(SHOW_MS) != -1) return i;
        }
        printf("  (nothing pressed - looping)\n");
    }
}

/* ----------------------------------------------------------------- phase 3 */

static void phase3_report(int idx)
{
    const variant_t *v = &VARIANTS[idx];
    const char *drv = v->drv == DRV_ST7789 ? "ST7789"
                    : v->drv == DRV_ST7735 ? "ST7735" : "ILI9341";

    printf("\n========================================\n");
    printf("  PHASE 3 - CONFIRMED\n");
    printf("========================================\n");
    printf("Display: %s\n\n", v->name);
    printf("  controller : %s\n", drv);
    printf("  size       : %dx%d\n", v->w, v->h);
    printf("  offsets    : col=%d row=%d\n", v->col_off, v->row_off);
    printf("  inverted   : %s\n", v->invert ? "yes (IPS)" : "no");
    printf("  SCLK=%d MOSI=%d DC=%d CS=%d RST=%d BL=%d\n\n",
           v->sck, v->mosi, v->dc, v->cs, v->rst, v->bl);

    printf("Equivalent TFT_eSPI User_Setup, if you ever go back to Arduino:\n");
    printf("  #define %s_DRIVER\n", drv);
    printf("  #define TFT_WIDTH  %d\n", v->w);
    printf("  #define TFT_HEIGHT %d\n", v->h);
    printf("  #define TFT_MOSI %d\n", v->mosi);
    printf("  #define TFT_SCLK %d\n", v->sck);
    printf("  #define TFT_CS   %d\n", v->cs);
    printf("  #define TFT_DC   %d\n", v->dc);
    printf("  #define TFT_RST  %d\n", v->rst);
    printf("  #define TFT_BL   %d\n", v->bl);
    if (v->col_off || v->row_off) printf("  #define CGRAM_OFFSET\n");
    printf("\n");
}

/* Live button readout on the now-known-good panel. Never returns. */
static void phase3_button_ui(int idx)
{
    const variant_t *v = &VARIANTS[idx];
    printf("PHASE 3b - live button test. Press the buttons.\n");

    bool pa = false, pb = false, first = true;
    uint32_t na = 0, nb = 0;

    for (;;) {
        bool a = pin_is_low(BTN_A), b = pin_is_low(BTN_B);
        if (a != pa || b != pb || first) {
            if (a && !pa) printf("[GPIO35] press #%u\n", (unsigned)++na);
            if (b && !pb) printf("[GPIO0 ] press #%u\n", (unsigned)++nb);
            pa = a; pb = b; first = false;

            fill_rect(v, 0, 0, v->w, v->h, RGB565(0,0,0));
            /* left half tracks GPIO35, right half GPIO0 */
            fill_rect(v, 2,          2, v->w/2 - 4, v->h - 4,
                      a ? RGB565(0,220,0) : RGB565(40,40,40));
            fill_rect(v, v->w/2 + 2, 2, v->w/2 - 4, v->h - 4,
                      b ? RGB565(0,220,0) : RGB565(40,40,40));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* -------------------------------------------------------------------- main */

void app_main(void)
{
    /* buttons: inputs. GPIO35 cannot have an internal pull-up. */
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << BTN_A) | (1ULL << BTN_B),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    vTaskDelay(pdMS_TO_TICKS(300));
    printf("\n\n=== display_probe (raw ESP-IDF) ===\n");
    ESP_LOGI(TAG, "buttons first, then LEDs, then the LCD sweep");

    phase1_buttons();
    phase1_leds();

    int idx = phase2_sweep();      /* blocks until you confirm */
    phase3_report(idx);
    phase3_button_ui(idx);         /* never returns */
}
