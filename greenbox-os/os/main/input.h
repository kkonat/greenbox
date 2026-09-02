/*
 * input.h - the two buttons, debounced and classified into events.
 *
 * There is exactly one event queue in the system and exactly one reader of it
 * at a time: the launcher while nothing is running, the guest while something
 * is. The kill request never reaches that queue at all - the input task acts
 * on it directly, so a wedged guest that has stopped reading events can still
 * be killed.
 */
#pragma once

#include "greenbox_abi.h"

/* Press-length thresholds, milliseconds.
 *
 * One threshold per button, and they are deliberately different.
 *
 * The left hold is escape and kill in one gesture, so its threshold is the
 * kill's: three seconds, long enough that nobody destroys a running program by
 * resting a thumb on it. It fires on the earn rather than on the release
 * because at three seconds there is no longer anything to disambiguate - the
 * old arrangement was 450 ms for the escape and a separate 3 s for the kill,
 * which meant the left button had to wait and see, and escape only happened
 * when you let go.
 *
 * The right hold is accept, and accept has no destructive twin to be told
 * apart from, so it has no reason to make the user wait: one second, which is
 * clearly past a tap and short enough that running a program does not feel
 * like an argument with the board. */
#define IN_DEBOUNCE_MS    25
#define IN_LONG_L_MS    3000    /* escape, and the kill that comes with it */
#define IN_LONG_R_MS    1000    /* accept */

/* How far into a left hold the OS starts showing the quit bar over a running
 * guest, leaving the remaining IN_LONG_L_MS - IN_QUIT_HINT_MS to fill it. Late
 * enough that an ordinary tap-that-lingered never flashes anything on screen,
 * early enough to answer "is this thing listening" before the doubt sets in. */
#define IN_QUIT_HINT_MS 1000

void       input_start(void);

/* Non-blocking / blocking reads of the single event queue. */
gb_event_t input_poll(void);
gb_event_t input_wait(uint32_t timeout_ms);

/* The debounced buttons as they are right now, a mask of GB_BTN_*. Beside the
 * queue rather than through it: an event says what gesture happened, and no
 * gesture says when a hold ends. Anything that has to move while a button is
 * down and stop when it is released has to read the state instead. */
uint8_t    input_buttons(void);

/* Throw away anything queued - called on every hand-off between readers so a
 * guest does not inherit the long-press that launched it. */
void       input_drain(void);

/* Push a synthetic event. Used by the serial console so the board can be
 * driven without touching it. */
void       input_inject(gb_event_t ev);
