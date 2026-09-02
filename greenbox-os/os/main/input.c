/*
 * input.c - button polling, debounce, and press-length classification.
 *
 * Polled rather than interrupt-driven on purpose: at a 10 ms tick the cost is
 * nil, debouncing is trivial, and the kill gesture needs a timer running while
 * the button is *held* rather than an edge, which an ISR would have to arrange
 * anyway.
 *
 * Both buttons fire their long press the instant their threshold passes, and
 * neither waits for the release. For the left one that is only possible
 * because escape and kill are now the same gesture at the same three seconds -
 * at the old 450 ms it could not yet tell one from the start of the other, so
 * it had to wait for the release to find out. It now emits GB_EV_L_LONG and
 * asks for the kill in the same pass: the event for a guest polite enough to
 * return, the kill for one that is not.
 *
 * The right one has no such twin to disambiguate from, so it is not held to
 * the kill's timing: accept lands at one second.
 *
 * GPIO35 is input-only with no internal pull-up (true of GPIO34-39); it works
 * because the board provides an external one. GPIO0 is the boot strap pin, so
 * holding left through a reset drops the chip into the serial bootloader.
 */

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "board.h"
#include "input.h"
#include "guest.h"

static const char *TAG = "input";

#define POLL_MS      10
#define QUEUE_LEN     8

static QueueHandle_t s_q;

/* Read the GPIO input registers directly - GPIO35 lives in the second bank. */
static inline bool pin_down(int pin)
{
    if (pin < 32) return !((REG_READ(GPIO_IN_REG)  >> pin)        & 1U);
    else          return !((REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1U);
}

typedef struct {
    int      pin;
    bool     raw;         /* last sampled level */
    bool     down;        /* debounced */
    uint32_t stable_ms;   /* how long raw has agreed with itself */
    uint32_t down_at;     /* millis at the debounced press */
    bool     latched;     /* an event already fired for this press */
} btn_t;

static btn_t s_l = { .pin = TD_BTN_LEFT  };
static btn_t s_r = { .pin = TD_BTN_RIGHT };

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void emit(gb_event_t ev)
{
    /* Never block the input task: if the reader is behind, drop the oldest. */
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
        gb_event_t discard;
        xQueueReceive(s_q, &discard, 0);
        xQueueSend(s_q, &ev, 0);
    }
}

/* Debounce. Returns +1 on a fresh press, -1 on a fresh release, 0 otherwise. */
static int debounce(btn_t *b, uint32_t now)
{
    bool raw = pin_down(b->pin);
    if (raw != b->raw) { b->raw = raw; b->stable_ms = 0; return 0; }

    b->stable_ms += POLL_MS;
    if (raw == b->down || b->stable_ms < IN_DEBOUNCE_MS) return 0;

    b->down = raw;
    if (raw) { b->down_at = now; b->latched = false; return 1; }
    return -1;
}

static void input_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t now = now_ms();

        /* ---- left: escape and kill, both earned at IN_LONG_L_MS -------- */
        int el = debounce(&s_l, now);
        if (el < 0 && !s_l.latched) {
            emit(GB_EV_L_SHORT);
        } else if (s_l.down && !s_l.latched && now - s_l.down_at >= IN_LONG_L_MS) {
            s_l.latched = true;                 /* suppress the release event */
            ESP_LOGI(TAG, "left hold: escape + kill");
            /* Order matters. The event goes first so a guest sitting in
             * wait_event comes back with L_LONG rather than the empty wake-up
             * that guest_request_kill injects, and gets its grace period to
             * return on its own. */
            emit(GB_EV_L_LONG);
            guest_request_kill();               /* no-op when nothing is running */
        }

        /* The hold in progress, on screen. Kept out of the branches above
         * because it has to run on every poll, including the ones where the
         * button did nothing: the bar grows with time, not with events. It is
         * deliberately still fed while the press is latched, so a hold that
         * has already fired keeps a full bar until the finger comes off. */
        guest_quit_hint(s_l.down ? now - s_l.down_at : 0);

        /* ---- right: accept, sooner than the left because it is safe ---- */
        int er = debounce(&s_r, now);
        if (er < 0 && !s_r.latched) {
            emit(GB_EV_R_SHORT);
        } else if (s_r.down && !s_r.latched && now - s_r.down_at >= IN_LONG_R_MS) {
            s_r.latched = true;
            emit(GB_EV_R_LONG);
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void input_start(void)
{
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << TD_BTN_LEFT) | (1ULL << TD_BTN_RIGHT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   /* GPIO35 cannot have one anyway */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    s_q = xQueueCreate(QUEUE_LEN, sizeof(gb_event_t));

    /* Above the launcher and any guest: the kill gesture must be serviced
     * even when a guest is spinning. */
    xTaskCreate(input_task, "input", 2560, NULL, 6, NULL);
}

uint8_t input_buttons(void)
{
    /* Two bools written by the input task and read here without a lock. Each
     * is a single byte and either value is a value the button genuinely had,
     * so the worst this can return is one 10 ms poll out of date - which is
     * also true of the debounce itself. */
    return (uint8_t)((s_l.down ? GB_BTN_L : 0) | (s_r.down ? GB_BTN_R : 0));
}

gb_event_t input_poll(void)
{
    gb_event_t ev;
    return xQueueReceive(s_q, &ev, 0) == pdTRUE ? ev : GB_EV_NONE;
}

gb_event_t input_wait(uint32_t timeout_ms)
{
    gb_event_t ev;
    TickType_t t = timeout_ms == portMAX_DELAY ? portMAX_DELAY
                                               : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_q, &ev, t) == pdTRUE ? ev : GB_EV_NONE;
}

void input_drain(void)
{
    xQueueReset(s_q);
}

void input_inject(gb_event_t ev)
{
    emit(ev);
}
