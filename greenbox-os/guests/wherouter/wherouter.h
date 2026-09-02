/*
 * wherouter.h - what the two screens share.
 *
 * The census lives in wherouter.c and the hunt lives in finder.c, and the only
 * things that cross between them are the panel, the palette, and the one AP
 * the user picked out of the list.
 */
#pragma once

#include "greenbox_abi.h"
#include "gb_rt.h"

extern const gb_api_t *A;
extern gb_theme_t      T;
extern int16_t         W, H;

/* The strip the OS paints its quit bar into is 13 rows tall and it is drawn
 * over whatever is there. Reserving 14 keeps the bar off everything that
 * matters, and both screens repaint their footer often enough that a
 * cancelled hold heals within a frame or two rather than leaving a scar. */
#define FOOT_H   14
#define TITLE_H  14

/* dBm the display treats as the ends of the world. -30 is a radio in the same
 * room as this one and -92 is the noise floor of a board with a chip antenna;
 * outside that range the bar is pinned rather than wrong. */
#define RSSI_HOT  (-30)
#define RSSI_COLD (-92)

/*
 * The signal ramp, and the one set of colours in this program that the theme
 * does not get a say in. A palette is free to decide what a heading looks
 * like; it is not free to decide that a weak signal looks encouraging.
 */
uint16_t sig_colour(int dbm);

/* Bar length in pixels for `dbm` across `span`, clamped to the ends above. */
int16_t  sig_bar(int dbm, int16_t span);

/* Rough metres, times ten, from a log-distance path loss fit. Rough is the
 * operative word - see the table in wherouter.c. */
uint16_t sig_dist_dm(int dbm);

/* Copy `src` into `dst`, cut to whatever fits in `maxpx` at text size `size`.
 * No ellipsis: the 5x7 font has no character for one, and three dots cost
 * three characters to say what a cut already said. */
void     fit_text(char *dst, int dstsz, const char *src, int16_t maxpx, uint8_t size);

/* A name for an AP that has none. Hidden networks beacon with a zero-length
 * SSID, so the last three octets of the BSSID are all there is to call it. */
void     ap_label(char *dst, int dstsz, const gb_ap_t *ap);

/* Longest of `cand` that fits the width, so one footer serves 240 columns and
 * 135 without either being written twice. */
const char *pick_fit(const char *const *cand, int n, int16_t maxpx);

/* ------------------------------------------------------------ the finder */
/*
 * Runs until the user leaves it. Returns true to go back to the list, false
 * when the OS wants the program gone.
 */
bool finder_run(const gb_ap_t *target);
