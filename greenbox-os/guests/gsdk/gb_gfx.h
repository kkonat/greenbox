/*
 * gb_gfx.h - the guest's side of api->gfx.
 *
 * The OS owns the rasteriser now (osgfx.c); this is the two-line-per-call
 * layer that puts the guest's names back on it. Everything here is a static
 * inline that expands to one indirect call through the table, so a band
 * renderer written against fb_hspan() costs a load and a jump more than one
 * written against a local function, and nothing else.
 *
 * The band idiom it assumes is the one astro and pinball both arrived at:
 *
 *   static uint16_t g_fb[SCR_MAX_W * BAND_H];
 *   gfx_attach(api, g_fb, w, BAND_H);          once, after set_rotation
 *   for (y = 0; y < h; y += BAND_H) {
 *       gfx_band(y, min(BAND_H, h - y));       this band
 *       ...draw the whole frame in screen coordinates...
 *       api->blit(0, y, w, GS.h, g_fb);        and send it
 *   }
 *
 * There is no framebuffer on this board - 135x240 in RGB565 is 64 KB, next to
 * an OS that needs most of what is left - and no need for one either, because
 * the panel is written top to bottom anyway. A band is 16 rows, so the whole
 * screen costs about 7.5 KB of the guest's DRAM and every draw call is a store
 * into RAM instead of a windowed SPI transaction.
 */
#pragma once

#include "greenbox_abi.h"

/* The table, and the band being assembled. Both set by gfx_attach(). */
extern const gb_gfx_t *GFX;
extern gb_surf_t       GS;

/* Point the surface at a guest-owned buffer `w` pixels wide and `band_h` rows
 * tall. Call it again after a rotation change, because w changes with it. */
void gfx_attach(const gb_api_t *api, uint16_t *px, int16_t w, int16_t band_h);

/* Move the surface to the band starting at screen row y0. The last band of a
 * screen whose height is not a multiple of BAND_H is short, which is what `h`
 * is for; everything clips to it. */
static inline void gfx_band(int y0, int h)
{
    GS.org_y = (int16_t)y0;
    GS.h     = (int16_t)h;
}

/*
 * Stop drawing outside these screen rows, band or no band. Half open, and an
 * empty rectangle means no clipping at all - which is the state gfx_attach
 * leaves it in.
 *
 * A band cannot express a viewport: a HUD strip along the top means the world
 * has to stop at row 12, and row 12 is in the middle of the first band. This
 * is how a guest says so once instead of clamping every call site.
 */
static inline void gfx_clip(int x0, int y0, int x1, int y1)
{
    GS.clip_x0 = (int16_t)x0; GS.clip_y0 = (int16_t)y0;
    GS.clip_x1 = (int16_t)x1; GS.clip_y1 = (int16_t)y1;
}

static inline void gfx_noclip(void)
{
    GS.clip_x0 = GS.clip_y0 = GS.clip_x1 = GS.clip_y1 = 0;
}

/* ------------------------------------------------------------- primitives */
/* x1 and y1 are INCLUSIVE in fb_hspan, as they were in both guests' copies. */

static inline void fb_row(int y, uint16_t c)
{ GFX->row(&GS, (int16_t)y, c); }

static inline void fb_hspan(int x0, int x1, int y, uint16_t c)
{ GFX->hspan(&GS, (int16_t)x0, (int16_t)x1, (int16_t)y, c); }

static inline void fb_px(int x, int y, uint16_t c)
{ GFX->px(&GS, (int16_t)x, (int16_t)y, c); }

static inline void fb_box(int x, int y, int w, int h, uint16_t c)
{ GFX->box(&GS, (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, c); }

static inline void fb_frame(int x, int y, int w, int h, uint16_t c)
{ GFX->frame(&GS, (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, c); }

static inline void fb_disc(int cx, int cy, int r, uint16_t c)
{ GFX->disc(&GS, (int16_t)cx, (int16_t)cy, (int16_t)r, c); }

static inline void fb_ring(int cx, int cy, int r, uint16_t c)
{ GFX->ring(&GS, (int16_t)cx, (int16_t)cy, (int16_t)r, c); }

static inline void fb_ellipse(int cx, int cy, int rx, int ry, uint16_t c)
{ GFX->ellipse(&GS, (int16_t)cx, (int16_t)cy, (int16_t)rx, (int16_t)ry, c); }

static inline void fb_line(int x0, int y0, int x1, int y1, uint16_t c)
{ GFX->line(&GS, (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, c); }

static inline void fb_bar(int x0, int y0, int x1, int y1, int r, uint16_t c)
{ GFX->bar(&GS, (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (int16_t)r, c); }

/* ------------------------------------------------------------------- text */

static inline void fb_text(int x, int y, const char *s, uint16_t c, int scale)
{ GFX->text(&GS, (int16_t)x, (int16_t)y, s, c, (uint8_t)scale); }

static inline int fb_text_w(const char *s, int scale)
{ return GFX->text_w(s, (uint8_t)scale); }

/*
 * One offset pass underneath, in a colour the caller picks. Text on a game
 * screen sits over whatever the background happens to be doing, and a shadow
 * is cheaper than a backing box and does not punch a hole in the art. The
 * colour is a parameter because near-black over a starfield and near-black
 * over a lit playfield are not the same near-black - and because a guest that
 * only ever uses one of them wraps these under its own name and stops passing
 * it, which is what astro and pinball both do.
 */
static inline void gfx_text_sh(int x, int y, const char *s, uint16_t c,
                               int scale, uint16_t shadow)
{
    fb_text(x + scale, y + scale, s, shadow, scale);
    fb_text(x, y, s, c, scale);
}

static inline void gfx_text_ctrx(int cx, int y, const char *s, uint16_t c,
                                 int scale, uint16_t shadow)
{ gfx_text_sh(cx - fb_text_w(s, scale) / 2, y, s, c, scale, shadow); }

static inline void gfx_text_ctr(int y, const char *s, uint16_t c,
                                int scale, uint16_t shadow)
{ gfx_text_ctrx(GS.org_x + GS.w / 2, y, s, c, scale, shadow); }

/* ---------------------------------------------------------------- sprites */

static inline void fb_sprite(const gb_sprite_t *sp, int x, int y)
{ GFX->sprite(&GS, sp, (int16_t)x, (int16_t)y); }

/* ------------------------------------------------------------------ maths */
/* Prefixed, unlike the drawing calls: mandel has its own rnd() and its own
 * fixed-point sine with a different scale, and a guest that wants both should
 * not have to fight the header for the name. */

static inline uint32_t gb_isqrt(uint32_t v)      { return GFX->isqrt(v); }
static inline int      gb_isin(int a)            { return GFX->isin((int16_t)a); }
static inline int      gb_icos(int a)            { return GFX->isin((int16_t)(a + 64)); }
static inline uint16_t gb_mix565(uint16_t a, uint16_t b, int t)
{ return GFX->mix565(a, b, (int16_t)t); }

static inline uint32_t gb_rnd(void)              { return GFX->rnd(); }
static inline void     gb_rnd_seed(uint32_t s)   { GFX->rnd_seed(s); }

static inline int gb_rnd_range(int lo, int hi)   /* inclusive */
{
    if (hi <= lo) return lo;
    return lo + (int)(GFX->rnd() % (uint32_t)(hi - lo + 1));
}
