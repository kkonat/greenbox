/*
 * pinball.h - what the three source files of PINBALL share.
 *
 * The split follows cost, the same way astro's does: pin_gfx.c owns
 * everything that runs per pixel (the band buffer, the font, the sprite
 * rasteriser), cast.c owns the two characters and the palettes that come with
 * them, and pinball.c owns the table - geometry, physics, scoring and the
 * state machine.
 */
#ifndef PINBALL_H
#define PINBALL_H

#include "greenbox_abi.h"
#include "gb_rt.h"
#include "gb_gfx.h"

/* ---------------------------------------------------------------- screen */
/*
 * The table is portrait, 135x240, and it is not a preference: a pinball
 * playfield is taller than it is wide, and turned the other way there is no
 * room between the flippers and the top arch for anything to happen. The OS
 * restores the system orientation when the guest exits.
 *
 * Nothing below assumes the panel actually gave us portrait - g_w and g_h are
 * read back after the rotation call and the band buffer is sized for the
 * larger axis - but the geometry in pinball.c is laid out in 135x240 and is
 * simply clipped if it is not.
 */
#define SCR_MAX_W   240
#define SCR_MAX_H   240
#define BAND_H      16          /* 240 / 16 = 15 whole bands */

extern const gb_api_t *A;
extern int16_t g_w, g_h;

/* ----------------------------------------------------------- band buffer */
/*
 * 135x240 at 16 bits is 64 KB and the OS is using that memory, so there is no
 * framebuffer. Each frame is assembled a 16-row slice at a time into 7.5 KB
 * and blitted, which turns every draw call into a store into RAM and lets the
 * panel see none of the intermediate states. Every primitive clips to the
 * current band, so drawing code is still written in screen coordinates and
 * simply gets called once per band.
 *
 * The primitives themselves are no longer here. They are the OS's - api->gfx,
 * reached through gb_gfx.h - because this file used to carry a copy of astro's
 * and said so; see osgfx.c for what changed and what it bought.
 */
extern uint16_t g_fb[SCR_MAX_W * BAND_H];

/* Where the band being assembled sits on the screen. Read-only: the renderer
 * moves the band with gfx_band(), and the culling in pin_gfx.c asks these two
 * what it is currently allowed to touch. */
#define g_band_y0  ((int)GS.org_y)
#define g_band_h   ((int)GS.h)

/* ------------------------------------------------------------ primitives */
/*
 * fb_row, fb_hspan, fb_px, fb_box, fb_frame, fb_disc, fb_ring, fb_line and
 * fb_bar all come from gb_gfx.h and clip to the current band exactly as the
 * local copies did. Only the ones carrying this table's own taste in shadows
 * are spelled out here.
 */

/* 3x5 font, drawn into the band. The OS has a 5x7 one but api->text() paints
 * straight onto the panel, which tears against a band that has already been
 * sent - which is why the band font came across into api->gfx too. */
#define TXT_W(str, sc)  ((int)strlen(str) * 4 * (sc) - (sc))

/* Near-black with a little warmth: this text sits over a lit playfield. */
#define PIN_SHADOW  GB_RGB(6, 4, 12)

static inline void fb_text_sh(int x, int y, const char *s, uint16_t c, int scale)
{ gfx_text_sh(x, y, s, c, scale, PIN_SHADOW); }

static inline void fb_text_ctr(int y, const char *s, uint16_t c, int scale)
{ gfx_text_ctr(y, s, c, scale, PIN_SHADOW); }

static inline void fb_text_ctrx(int cx, int y, const char *s, uint16_t c, int scale)
{ gfx_text_ctrx(cx, y, s, c, scale, PIN_SHADOW); }

/* ----------------------------------------------------------------- maths */
/* The integer geometry moved to the OS with the primitives. These are the
 * names pinball.c and cast.c already use, on top of api->gfx. */
#define rnd()           gb_rnd()
#define rnd_seed(s)     gb_rnd_seed(s)
#define rnd_range(a, b) gb_rnd_range((a), (b))
#define isqrt32(v)      gb_isqrt(v)
#define isin(a)         gb_isin(a)
#define icos(a)         gb_icos(a)
#define mix565(a, b, t) gb_mix565((a), (b), (t))

/* ------------------------------------------------------------ sprite art */
/*
 * Sprites are ASCII in the source - a row per line, one character per pixel,
 * '.' transparent - and drawn at an integer scale. It costs a palette lookup
 * per source pixel, which for a 26x28 face is a thousand lookups whatever the
 * scale, and in exchange the two faces in cast.c are editable in a text editor
 * and legible as art in the file.
 *
 * `tint` mixes every colour that far toward `to` before it is written, which
 * is what turns a character portrait into a backdrop rather than a picture
 * standing in front of the table.
 */

typedef gb_spal_t spal_t;

static inline void draw_sprite(const char *const *rows, int nrows, int ncols,
                               int x, int y, const spal_t *pal, int npal,
                               int scale, uint16_t to, int tint)
{
    gb_sprite_t sp = {
        .rows = rows, .pal = pal,
        .nrows = (int16_t)nrows, .ncols = (int16_t)ncols, .npal = (int16_t)npal,
        .scale = (int16_t)scale, .lean = 0,
        .tint_to = to, .tint = (int16_t)tint,
    };
    fb_sprite(&sp, x, y);
}

/* --------------------------------------------------------------- the cast */
/*
 * Which of the two is on the backglass is picked afresh for every ball, so a
 * three-ball game is usually a Gumball ball and a Darwin ball rather than one
 * of them all the way through. Everything the rest of the program needs to
 * change with them lives in this record - the lane letters included, because
 * lighting G-U-M under Gumball and D-A-R under Darwin is most of what makes
 * the swap feel deliberate rather than decorative.
 */
typedef struct {
    const char *name;
    const char *lane;           /* three letters, one per top lane */
    uint16_t    body;           /* the character's own colour */
    uint16_t    trim;           /* darker, for outlines and rails */
    uint16_t    glow;           /* lamps, lit lanes, the ball's rim light */
    uint16_t    felt_top;       /* playfield wash, top and bottom */
    uint16_t    felt_bot;
    const char *taunt;          /* shown while the ball waits in the lane */
} cast_t;

#define CAST_GUMBALL 0
#define CAST_DARWIN  1

const cast_t *cast_get(int who);
/* The portrait, tinted into the felt, centred on the playfield. */
void cast_draw_backdrop(int who, int x, int y, int scale, uint16_t felt);
/* The same face at scale 1..2 for the attract screen and the ball-in-lane
 * prompt, drawn at full strength. */
void cast_draw_face(int who, int x, int y, int scale);

#define CAST_W 34
#define CAST_H 30

#endif /* PINBALL_H */
