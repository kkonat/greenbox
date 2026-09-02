/*
 * launcher.h - the shell: a list of programs and the buttons that drive it.
 */
#pragma once

#include <stdbool.h>

#define GB_PROG_DIR  "/progs"
#define GB_PROG_EXT  ".gbx"

/* Rescan GB_PROG_DIR. Called at boot and after anything writes a program. */
void launcher_rescan(void);

/* Never returns. Owns the panel whenever a guest does not. */
void launcher_run(void);

/* Force a full repaint on the launcher task. That is how a settings change
 * made from somewhere else - the serial console - reaches the screen: the
 * repaint picks up the new palette and, if it moved, the new orientation. */
void launcher_repaint(void);

/* Ask the launcher task to run a program by name. Returns false if there is no
 * such program. The launch itself happens on the launcher task - the panel and
 * the guest lifecycle have exactly one owner, and it is not the caller. */
bool launcher_request_run(const char *name);
