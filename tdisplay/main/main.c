/*
 * TTGO T-Display starter - proves the board environment works end to end.
 *
 * LEFT button (GPIO0)  : decrement / previous screen
 * RIGHT button (GPIO35): increment / next screen
 * hold both ~1s        : cycle rotation
 *
 * Raw ESP-IDF. The only display code is st7789.c in this directory.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "board.h"
#include "st7789.h"

/* Read the GPIO input registers directly - GPIO35 lives in the second bank. */
static inline bool pressed(int pin)
{
    if (pin < 32) return !((REG_READ(GPIO_IN_REG)  >> pin)        & 1U);
    else          return !((REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1U);
}

typedef struct {
    int      pin;
    bool     state;      /* debounced: true = down */
    bool     raw;
    uint32_t stable_ms;
    uint32_t down_ms;
    uint32_t count;
} btn_t;

static btn_t s_left  = { .pin = TD_BTN_LEFT  };
static btn_t s_right = { .pin = TD_BTN_RIGHT };

/* Returns true on a fresh press (rising edge of "down"). */
static bool btn_poll(btn_t *b, uint32_t now, uint32_t dt)
{
    bool raw = pressed(b->pin);
    if (raw != b->raw) { b->raw = raw; b->stable_ms = 0; return false; }
    b->stable_ms += dt;
    if (raw == b->state || b->stable_ms < 25) return false;

    b->state = raw;
    if (raw) { b->down_ms = now; b->count++; return true; }
    return false;
}

static void draw(int value, uint8_t rot)
{
    st7789_fill(C_BLACK);

    int W = st7789_width(), H = st7789_height();

    st7789_rect(0, 0, W, H, C_DKGREY);
    st7789_text(4, 4, "T-DISPLAY", C_CYAN, C_BLACK, 1);

    char buf[32];
    snprintf(buf, sizeof buf, "%d", value);
    int tw = st7789_text_width(buf, 3);
    st7789_text((W - tw) / 2, H / 2 - 12, buf, C_WHITE, C_BLACK, 3);

    /* button state strip along the bottom */
    int bh = 18, by = H - bh - 3;
    st7789_fill_rect(3,         by, W / 2 - 5, bh, s_left.state  ? C_GREEN : C_DKGREY);
    st7789_fill_rect(W / 2 + 2, by, W / 2 - 5, bh, s_right.state ? C_GREEN : C_DKGREY);
    st7789_text(8,          by + 5, "L", C_BLACK, s_left.state  ? C_GREEN : C_DKGREY, 1);
    st7789_text(W / 2 + 7,  by + 5, "R", C_BLACK, s_right.state ? C_GREEN : C_DKGREY, 1);

    snprintf(buf, sizeof buf, "rot%u %dx%d", rot, W, H);
    st7789_text(4, H - bh - 16, buf, C_GREY, C_BLACK, 1);
}

void app_main(void)
{
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << TD_BTN_LEFT) | (1ULL << TD_BTN_RIGHT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   /* GPIO35 cannot have one anyway */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    st7789_init(0);            /* 0 = native portrait 135x240 */

    if (!st7789_ready()) {
        /* Keep running so the buttons still report over serial - a dead panel
         * should not look like a dead board. */
        printf("\n*** PANEL INIT FAILED - see the st7789 error above ***\n");
    }

    printf("\nT-Display starter up. LEFT=GPIO%d  RIGHT=GPIO%d\n",
           TD_BTN_LEFT, TD_BTN_RIGHT);

    int value = 0;
    uint8_t rot = 0;
    uint32_t now = 0;
    const uint32_t DT = 20;
    uint32_t both_since = 0;
    bool dirty = true;

    for (;;) {
        bool l = btn_poll(&s_left,  now, DT);
        bool r = btn_poll(&s_right, now, DT);

        if (l) { value--; dirty = true; printf("LEFT  -> %d\n", value); }
        if (r) { value++; dirty = true; printf("RIGHT -> %d\n", value); }

        /* both held for ~1s cycles rotation */
        if (s_left.state && s_right.state) {
            if (both_since == 0) both_since = now;
            if (now - both_since > 1000) {
                rot = (rot + 1) & 3;
                st7789_set_rotation(rot);
                printf("rotation -> %u  (%dx%d)\n", rot, st7789_width(), st7789_height());
                both_since = 0;
                dirty = true;
                while (s_left.state || s_right.state) {   /* wait for release */
                    btn_poll(&s_left, now, DT);
                    btn_poll(&s_right, now, DT);
                    vTaskDelay(pdMS_TO_TICKS(DT));
                    now += DT;
                }
            }
        } else {
            both_since = 0;
        }

        if (l || r || dirty) { draw(value, rot); dirty = false; }

        vTaskDelay(pdMS_TO_TICKS(DT));
        now += DT;
    }
}
