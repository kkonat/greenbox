#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "board.h"
#include "st7789.h"

static const char *TAG = "st7789";

#define SPI_HOST_USED   SPI3_HOST
#define SPI_CLOCK_HZ    (40 * 1000 * 1000)   /* ST7789 handles 40MHz fine here */

static spi_device_handle_t s_spi;      /* NULL until st7789_init() succeeds */
static bool s_ready;
static uint8_t  s_rot;
static int16_t  s_w, s_h;
static uint8_t  s_colof, s_rowof;

/* ------------------------------------------------------ the reserved strip
 *
 * Rows at the bottom of the panel the OS has claimed for itself; see the
 * comment in st7789.h for why they are taken rather than repainted. Every
 * primitive below clips to draw_h() instead of s_h, which costs one load and
 * one subtract on a path that is already about to talk to a 40 MHz bus.
 *
 * s_override is what makes the OS the one writer that can reach in there. It
 * is only ever set for the length of a single painting pass, from the same
 * task, under the same syscall guard a guest's draws take - so a guest and the
 * OS can never be inside a primitive at once, and the flag needs no lock of
 * its own. */
static int16_t  s_reserve;
static bool     s_override;

static inline int16_t draw_h(void)
{
    if (s_override) return s_h;
    int16_t h = (int16_t)(s_h - s_reserve);
    return h > 0 ? h : 0;      /* a rotation into the shorter axis cannot go negative */
}

void st7789_reserve(int16_t rows)
{
    if (rows < 0)   rows = 0;
    if (rows > s_h) rows = s_h;
    s_reserve = rows;
}

int16_t st7789_reserved(void) { return s_reserve; }

void st7789_reserve_override(bool on) { s_override = on; }

/* --------------------------------------------------------------- SPI layer */

static void tx(const uint8_t *d, size_t n, bool is_data)
{
    if (!n) return;
    /* Without this guard a failed init turns into an endless stream of
     * "invalid dev handle" from spi_master with no hint at the cause. */
    if (!s_spi) {
        static bool moaned;
        if (!moaned) { moaned = true; ESP_LOGE(TAG, "SPI device not initialised - st7789_init() failed"); }
        return;
    }
    gpio_set_level(TD_DC, is_data);
    const size_t MAX = 4092;
    while (n) {
        size_t k = n > MAX ? MAX : n;
        spi_transaction_t t = { .length = k * 8, .tx_buffer = d };
        spi_device_polling_transmit(s_spi, &t);
        d += k;
        n -= k;
    }
}

static void wr_cmd(uint8_t c)                        { tx(&c, 1, false); }
static void wr_dat(const uint8_t *d, size_t n)       { tx(d, n, true); }
static void wr_cd(uint8_t c, const uint8_t *d, size_t n) { wr_cmd(c); if (n) wr_dat(d, n); }

/* ------------------------------------------------------------ addressing */

static void set_window(int16_t x, int16_t y, int16_t w, int16_t h)
{
    uint16_t x0 = x + s_colof, x1 = x + w - 1 + s_colof;
    uint16_t y0 = y + s_rowof, y1 = y + h - 1 + s_rowof;
    uint8_t b[4];

    b[0] = x0 >> 8; b[1] = x0 & 0xFF; b[2] = x1 >> 8; b[3] = x1 & 0xFF;
    wr_cd(0x2A, b, 4);                        /* CASET */
    b[0] = y0 >> 8; b[1] = y0 & 0xFF; b[2] = y1 >> 8; b[3] = y1 & 0xFF;
    wr_cd(0x2B, b, 4);                        /* RASET */
    wr_cmd(0x2C);                             /* RAMWR */
}

/* ------------------------------------------------------------------- init */

void st7789_set_rotation(uint8_t r)
{
    r &= 3;
    s_rot = r;

    /* MADCTL bits: MY(0x80) MX(0x40) MV(0x20). The offsets differ per rotation
     * because the 135x240 glass sits off-centre in the controller's 240x320 RAM. */
    static const uint8_t madctl[4] = { 0x00, 0x60, 0xC0, 0xA0 };
    static const uint8_t colof[4]  = {   52,   40,   53,   40 };
    static const uint8_t rowof[4]  = {   40,   52,   40,   53 };

    wr_cd(0x36, &madctl[r], 1);
    s_colof = colof[r];
    s_rowof = rowof[r];

    if (r & 1) { s_w = TD_PANEL_H; s_h = TD_PANEL_W; }   /* landscape 240x135 */
    else       { s_w = TD_PANEL_W; s_h = TD_PANEL_H; }   /* portrait  135x240 */
}

uint8_t st7789_rotation(void) { return s_rot; }
int16_t st7789_width(void)  { return s_w; }
int16_t st7789_height(void) { return s_h; }

void st7789_backlight(bool on) { gpio_set_level(TD_BL, on); }

void st7789_init(uint8_t rotation)
{
    esp_err_t err;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << TD_DC) | (1ULL << TD_RST) | (1ULL << TD_BL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err)); return; }

    spi_bus_config_t bus = {
        .mosi_io_num = TD_MOSI, .miso_io_num = -1, .sclk_io_num = TD_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4096 * 2,
    };
    err = spi_bus_initialize(SPI_HOST_USED, &bus, SPI_DMA_CH_AUTO);
    /* Tolerate an already-initialised bus so a warm re-init is not fatal. */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode = 0, .spics_io_num = TD_CS, .queue_size = 4,
        /* This board wires MOSI to GPIO19, but VSPI's native IOMUX MOSI is
         * GPIO23 - so the bus goes through the GPIO matrix. Above 26.7MHz the
         * driver then refuses full-duplex, because the extra matrix delay makes
         * *reads* unreliable. We never read from the panel (miso_io_num = -1),
         * so skip the dummy-cycle check and keep the full 40MHz. Without this,
         * spi_bus_add_device returns ESP_ERR_NOT_SUPPORTED and every later
         * transfer fails with "invalid dev handle". */
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    err = spi_bus_add_device(SPI_HOST_USED, &dev, &s_spi);
    if (err != ESP_OK || s_spi == NULL) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
        s_spi = NULL;
        return;
    }
    ESP_LOGI(TAG, "SPI up: host=%d sclk=%d mosi=%d cs=%d dc=%d rst=%d bl=%d @%dHz",
             SPI_HOST_USED, TD_SCLK, TD_MOSI, TD_CS, TD_DC, TD_RST, TD_BL, SPI_CLOCK_HZ);

    /* hardware reset */
    gpio_set_level(TD_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TD_RST, 0); vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(TD_RST, 1); vTaskDelay(pdMS_TO_TICKS(150));

    wr_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));               /* SWRESET */
    wr_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));               /* SLPOUT  */
    wr_cd(0x3A, (const uint8_t[]){0x55}, 1);                    /* COLMOD 16bpp */
    st7789_set_rotation(rotation);                              /* MADCTL + offsets */
#if TD_IS_IPS
    wr_cmd(0x21);                                               /* INVON - IPS panel */
#else
    wr_cmd(0x20);
#endif
    wr_cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));                /* NORON  */
    wr_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(120));               /* DISPON */

    st7789_fill(C_BLACK);
    st7789_backlight(true);
    s_ready = true;
    ESP_LOGI(TAG, "panel ready: %dx%d rot%u", s_w, s_h, s_rot);
}

bool st7789_ready(void) { return s_ready; }

/* --------------------------------------------------------------- drawing */

/* One scanline of solid colour, reused. 240 px is the widest this panel gets. */
static uint16_t s_line[240];

void st7789_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t colour)
{
    const int16_t lim = draw_h();

    if (x >= s_w || y >= lim) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_w) w = s_w - x;
    if (y + h > lim) h = lim - y;
    if (w <= 0 || h <= 0) return;

    /* the panel takes pixels big-endian */
    uint16_t be = (colour >> 8) | (colour << 8);
    for (int i = 0; i < w; i++) s_line[i] = be;

    set_window(x, y, w, h);
    gpio_set_level(TD_DC, 1);
    for (int row = 0; row < h; row++) {
        spi_transaction_t t = { .length = (size_t)w * 16, .tx_buffer = s_line };
        spi_device_polling_transmit(s_spi, &t);
    }
}

/* Whole panel, minus whatever the OS has reserved - fill_rect does that part. */
void st7789_fill(uint16_t colour) { st7789_fill_rect(0, 0, s_w, s_h, colour); }

void st7789_pixel(int16_t x, int16_t y, uint16_t colour)
{
    st7789_fill_rect(x, y, 1, 1, colour);
}

void st7789_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t colour)
{
    st7789_fill_rect(x,         y,         w, 1, colour);
    st7789_fill_rect(x,         y + h - 1, w, 1, colour);
    st7789_fill_rect(x,         y,         1, h, colour);
    st7789_fill_rect(x + w - 1, y,         1, h, colour);
}

void st7789_blit(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px)
{
    /* Rows come out of the caller's buffer top-first, so a blit that runs into
     * the reserved strip is shortened from the bottom and everything above it
     * still lands where the caller meant it to. */
    const int16_t lim = draw_h();
    if (y >= lim) return;
    if (y + h > lim) h = (int16_t)(lim - y);
    if (w <= 0 || h <= 0) return;

    set_window(x, y, w, h);
    /* caller's buffer is already big-endian if it came from st7789_text scratch;
     * for raw RGB565 we byte-swap a row at a time */
    gpio_set_level(TD_DC, 1);
    for (int row = 0; row < h; row++) {
        for (int i = 0; i < w; i++) {
            uint16_t c = px[row * w + i];
            s_line[i] = (c >> 8) | (c << 8);
        }
        spi_transaction_t t = { .length = (size_t)w * 16, .tx_buffer = s_line };
        spi_device_polling_transmit(s_spi, &t);
    }
}

/* ------------------------------------------------------------------ font */
/* Classic 5x7 cell font, ASCII 32..126. Each glyph is 5 column bytes, LSB=top. */
static const uint8_t FONT5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x08,0x2A,0x1C,0x2A,0x08}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x01,0x01},
    {0x3E,0x41,0x41,0x51,0x32}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x04,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x7F,0x20,0x18,0x20,0x7F}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20}, {0x41,0x41,0x7F,0x00,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x00,0x7F,0x10,0x28,0x44}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x08,0x08,0x2A,0x1C,0x08},
};

#define GLYPH_W 6      /* 5 columns + 1 spacing */
#define GLYPH_H 8      /* 7 rows + 1 spacing */

int16_t st7789_text_width(const char *s, uint8_t size)
{
    return (int16_t)(strlen(s) * GLYPH_W * size);
}

int16_t st7789_text(int16_t x, int16_t y, const char *s,
                    uint16_t fg, uint16_t bg, uint8_t size)
{
    if (size < 1) size = 1;
    const int16_t lim = draw_h();

    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if (c < 32 || c > 126) c = '?';
        const uint8_t *g = FONT5x7[c - 32];

        /* Render the whole cell into the scratch line, column by column, so each
         * glyph is one windowed write instead of a pixel storm. */
        int16_t cw = GLYPH_W * size, ch = GLYPH_H * size;
        if (x >= s_w) break;

        /* A cell the reserved strip eats into is drawn short rather than
         * skipped, and x advances either way: the return value has to be the
         * text's real width or a caller laying out a line would see it move
         * the moment the OS took the strip. */
        if (y + ch > lim) ch = (int16_t)(lim - y);
        if (ch <= 0) { x += cw; continue; }

        set_window(x, y, cw, ch);
        gpio_set_level(TD_DC, 1);

        for (int row = 0; row < ch; row++) {
            int gr = row / size;                     /* 0..7 */
            for (int col = 0; col < cw; col++) {
                int gc = col / size;                 /* 0..5 */
                bool on = (gc < 5 && gr < 7) && ((g[gc] >> gr) & 1);
                uint16_t v = on ? fg : bg;
                s_line[col] = (v >> 8) | (v << 8);
            }
            spi_transaction_t t = { .length = (size_t)cw * 16, .tx_buffer = s_line };
            spi_device_polling_transmit(s_spi, &t);
        }
        x += cw;
    }
    return x;
}
