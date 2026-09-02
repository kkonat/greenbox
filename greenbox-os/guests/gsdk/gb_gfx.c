/*
 * gb_gfx.c - the two globals behind gb_gfx.h.
 *
 * Kept out of the header so that every translation unit in a guest shares one
 * surface: the band renderer sets it in one file and the drawing code reads it
 * in another, which is exactly how astro splits per-frame work from per-pixel
 * work. --gc-sections drops both symbols from a guest that never draws.
 */

#include "gb_gfx.h"

const gb_gfx_t *GFX;
gb_surf_t       GS;

void gfx_attach(const gb_api_t *api, uint16_t *px, int16_t w, int16_t band_h)
{
    GFX = api->gfx;

    GS.px      = px;
    GS.w       = w;
    GS.h       = band_h;
    GS.org_x   = 0;
    GS.org_y   = 0;
    GS.clip_x0 = GS.clip_y0 = GS.clip_x1 = GS.clip_y1 = 0;  /* no clip */
}
