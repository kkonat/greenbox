/*
 * astro.h - what astro.c and astro_gfx.c share.
 *
 * The split is by cost, not by topic: astro_gfx.c owns everything that runs
 * per-pixel (the band buffer, the font, the parallax background, the rock
 * rasteriser) and astro.c owns everything that runs per-frame (entities,
 * collision, powerups, the HUD, the state machine).
 */
#ifndef ASTRO_H
#define ASTRO_H

#include "greenbox_abi.h"
#include "gb_rt.h"
#include "gb_gfx.h"

/* ---------------------------------------------------------------- screen */
/*
 * The game asks the OS for portrait (135x240) because a vertical scroller in
 * 135 rows of height is not a game, it is a corridor. The system orientation
 * in the OS settings is a request and this is the program that declines it -
 * the OS puts the user's choice back the moment the game exits. Nothing below
 * assumes portrait though - g_w and g_h are read back from the panel after the rotation
 * call, and the band buffer is sized for the larger of the two axes so the
 * whole thing still renders if it ever runs landscape.
 */
#define SCR_MAX_W   240
#define SCR_MAX_H   240
#define BAND_H      16          /* 240 / 16 = 15 whole bands in portrait */

extern const gb_api_t *A;
extern int16_t g_w, g_h;

/* ----------------------------------------------------------- band buffer */
/*
 * There is no room for a 135x240 framebuffer (64 KB) next to the OS, and there
 * is no need for one: the panel is written top to bottom anyway. Each frame is
 * rendered a BAND_H-row slice at a time into g_fb and blitted, which costs
 * 7.5 KB instead and lets every draw call be a plain store into RAM rather
 * than a windowed SPI transaction.
 *
 * The primitives that write into it are the OS's now - api->gfx, reached
 * through gb_gfx.h - so what used to be four hundred lines of astro_gfx.c is
 * the surface set-up in resize() and the aliases below. The rasteriser did not
 * change on the way across: it was moved, comments and all, because pinball
 * had already copied it once and pacman would have made three.
 */
extern uint16_t g_fb[SCR_MAX_W * BAND_H];

/* Where the band being assembled sits on the screen. Read-only here: the
 * renderer moves the band with gfx_band(), and the culling in astro_gfx.c
 * asks these two what it is currently allowed to touch. */
#define g_band_y0  ((int)GS.org_y)
#define g_band_h   ((int)GS.h)

/* ------------------------------------------------------------ primitives */
/*
 * fb_row, fb_hspan, fb_px, fb_box, fb_frame, fb_disc and fb_ellipse all come
 * from gb_gfx.h and clip to the current band exactly as the local copies did.
 * Only the three that carry astro's own taste in shadows and palettes are
 * spelled out here.
 */

/* 3x5 font, drawn into the band so HUD text never flickers against the
 * scrolling background the way a second pass over the panel would. */
#define TXT_W(str, sc)  ((int)strlen(str) * 4 * (sc) - (sc))

/* Near-black with a hint of blue: this text sits over a starfield. */
#define AST_SHADOW  GB_RGB(0, 0, 8)

static inline void fb_text_sh(int x, int y, const char *s, uint16_t c, int scale)
{ gfx_text_sh(x, y, s, c, scale, AST_SHADOW); }

static inline void fb_text_ctr(int y, const char *s, uint16_t c, int scale)
{ gfx_text_ctr(y, s, c, scale, AST_SHADOW); }

/* ----------------------------------------------------------------- maths */
/* The integer geometry moved to the OS with the primitives. These are the
 * names astro.c and astro_gfx.c already use, on top of api->gfx. */
#define rnd()           gb_rnd()
#define rnd_seed(s)     gb_rnd_seed(s)
#define rnd_range(a, b) gb_rnd_range((a), (b))
#define isqrt32(v)      gb_isqrt(v)
#define isin(a)         gb_isin(a)
#define icos(a)         gb_icos(a)
#define mix565(a, b, t) gb_mix565((a), (b), (t))

/* ------------------------------------------------------------ sprite art */
/*
 * Sprites are ASCII in the source: a row per line, one character per pixel,
 * '.' transparent. It costs a lookup per pixel at draw time - a rounding error
 * next to the SPI transfer - and in exchange the art is editable in a text
 * editor without a tool chain. The rasteriser is the OS's; `lean` is the shear
 * that banks the ship into its turn.
 */
typedef gb_spal_t spal_t;

static inline void draw_sprite(const char *const *rows, int nrows, int ncols,
                               int x, int y, const spal_t *pal, int npal,
                               int lean)
{
    gb_sprite_t sp = {
        .rows = rows, .pal = pal,
        .nrows = (int16_t)nrows, .ncols = (int16_t)ncols, .npal = (int16_t)npal,
        .scale = 1, .lean = (int16_t)lean, .tint_to = 0, .tint = 0,
    };
    fb_sprite(&sp, x, y);
}

/* ------------------------------------------------------------- rocks */
/* Rasterised per scanline rather than blitted, because asteroids come in every
 * size and tumble. See astro_gfx.c for the shape model. */
void draw_rock(int cx, int cy, int rad, int rot, int shape, int kind);

/* -------------------------------------------------------- the background */
/* Owned end to end by astro_gfx.c. astro.c only says how fast the world is
 * moving; parallax and drift are the background's business. */
void bg_init(void);
void bg_update(uint32_t dt_ms, int scroll_v);
void bg_draw(void);
void bg_flash(void);            /* white-out for one frame, on a crash */

#endif /* ASTRO_H */
