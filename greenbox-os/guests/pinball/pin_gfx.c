/*
 * pin_gfx.c - the band buffer, and where the rest of this file went.
 *
 * This was the pixel half of PINBALL: two hundred lines of spans, circles, a
 * Bresenham line, a 3x5 font and a sprite blitter. The header said they were
 * the ones astro established, copied here rather than shared, and gave the
 * reason: a guest is linked on its own, with no OS symbols to bind against, so
 * the alternative to a copy looked like a library directory that build.ps1
 * would have to learn about.
 *
 * That missed the third option, which is the one the whole system is built on.
 * Code a guest cannot LINK against it can still CALL, through the table it is
 * handed at entry. The primitives are the OS's now - osgfx.c, behind api->gfx,
 * reached through gb_gfx.h - and what was left here afterwards that pinball
 * alone wanted turned out to be nothing at all.
 *
 * So what remains is the buffer they used to fill, which is still the guest's:
 * 135x240 at 16 bits is 64 KB and the OS is using that memory, so a frame is
 * assembled one 16-row band at a time in 7.5 KB and blitted.
 */

#include "pinball.h"

const gb_api_t *A;
int16_t g_w, g_h;

uint16_t g_fb[SCR_MAX_W * BAND_H];
