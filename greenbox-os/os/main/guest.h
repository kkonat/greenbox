/*
 * guest.h - loading and running one guest program.
 *
 * One at a time, deliberately. A second concurrent guest would double every
 * ownership question (who owns the panel? who gets the buttons?) for no gain
 * on a board with two buttons and one screen.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "greenbox_abi.h"

/* Load a .gbx from the filesystem and start it. Returns once the guest task
 * is running, or an error if the image was rejected. */
esp_err_t    guest_start(const char *path);

/* Create the loader's semaphores. Call once at boot, before anything else
 * here. */
void         guest_init(void);

/* Block until the running guest exits, hard-killing it if it ignored a kill
 * request past the grace period, then release its memory. Safe to call when
 * nothing is running. */
void         guest_supervise(void);

bool         guest_is_running(void);
const char  *guest_name(void);

/* Ask the guest to stop. Called from the input task on the kill gesture, so it
 * must not block: it flags the guest and returns. guest_supervise does the waiting
 * and, if the guest ignores the flag, the hard kill. */
void         guest_request_kill(void);

/* Feedback for a left hold in progress, drawn over a running guest: nothing
 * for the first IN_QUIT_HINT_MS, then a bar that fills as the kill is earned.
 * Called from the input task on every poll with how long the button has been
 * down, or 0 when it is up. Does nothing unless a guest is running, and never
 * waits on the panel for longer than a bounded few milliseconds.
 *
 * For as long as the bar is up it reserves those scanlines through
 * st7789_reserve(), so the guest's own drawing is clipped above them and
 * cannot paint over the bar however often it redraws. The rows are given back
 * when the hold ends and by guest_supervise when the guest does - see the
 * comment on the implementation. */
void         guest_quit_hint(uint32_t held_ms);

/* --- used by the syscall table in gapi.c ------------------------------- */
bool         guest_stop_requested(void);
void         guest_syscall_enter(void);
void         guest_syscall_exit(void);
void        *guest_track_alloc(size_t n);
void         guest_track_free(void *p);

/* Largest contiguous block of executable RAM, i.e. the biggest guest that
 * could be loaded right now. */
size_t       guest_exec_free(void);

/* The table handed to every guest. */
extern const gb_api_t g_gb_api;
