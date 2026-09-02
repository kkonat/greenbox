/*
 * mandel.h - what the three files of MANDEL share.
 *
 * The split is by cost, the same way astro splits: fractal.c owns everything
 * that runs per-pixel (fixed point, the three iteration kernels, the search
 * for somewhere worth looking), palette.c owns colour, and mandel.c owns the
 * screen, the buttons and the state that survives a reboot.
 */
#ifndef MANDEL_H
#define MANDEL_H

#include "greenbox_abi.h"
#include "gb_rt.h"

#define SCR_MAX 240             /* the long axis of the panel, either way up */

extern const gb_api_t *A;
extern int16_t g_w, g_h;

/* ------------------------------------------------------------ fixed point */
/*
 * Q28: one unit is 2^-28, and the representable range is +-8.
 *
 * There is no libm here and no reason to want one, but a fractal is not a
 * starfield - Q8 would run out of resolution before the first zoom. Q28 leaves
 * three bits above the point, which is exactly what the iteration needs: the
 * escape test fires while |z| <= 2, so no coordinate the kernel keeps is ever
 * larger than 6, and every product is formed in int64 before it comes back
 * down. The ESP32 has a 32x32->64 multiplier, so that costs two instructions
 * rather than a call into libgcc.
 *
 * The floor under a zoom is set by the width of a pixel: the render stops
 * zooming at a half-width of 1.2e-4, where one pixel is still ~270 units
 * across and the shapes stay smooth. Past that Q28 would start to stair-step.
 */
typedef int32_t fx;
#define FX_BITS  28
#define FX_ONE   ((fx)1 << FX_BITS)
#define FX(v)    ((fx)((v) * 268435456.0 + ((v) < 0 ? -0.5 : 0.5)))

static inline fx fxmul(fx a, fx b)
{
    return (fx)(((int64_t)a * (int64_t)b) >> FX_BITS);
}

/* ----------------------------------------------------------------- scenes */
typedef enum {
    MODE_MANDEL = 0,
    MODE_JULIA  = 1,
    MODE_LYAP   = 2,
    MODE_COUNT  = 3,
} mode_t;

#define SEQ_MAX 6               /* letters in a Lyapunov sequence */

/*
 * Everything needed to draw one picture, and small enough to keep in NVS so
 * the program comes back to the view it was showing.
 */
typedef struct {
    uint8_t  mode;
    uint8_t  seq_len;           /* Lyapunov: how many letters are in use */
    uint8_t  seq[SEQ_MAX];      /* 0 = a, 1 = b */
    fx       cx, cy;            /* view centre, in whatever the mode's plane is */
    fx       hw;                /* half the width of the view */
    fx       jx, jy;            /* Julia parameter; unused by the other two */
    uint16_t maxiter;
    uint8_t  cycles;            /* times round the palette across the range */
    uint8_t  phase;             /* where in the palette the shallow end sits */
    int32_t  vmax;              /* the value one full palette run should span */
} scene_t;

/* --------------------------------------------------------------- fractal.c */
#define FR_INSIDE  (-1)         /* in the set, or - for Lyapunov - chaotic */

void     fr_scene(const scene_t *s);    /* which fractal, from here on */
void     fr_view(int w, int h);         /* raster the scene's view onto w x h */
int32_t  fr_at(int px, int py);         /* FR_INSIDE, or a colour value in Q8 */
void     fr_at_ss4(int px, int py, int32_t out[4]);  /* its four quarter-points */
int      fr_jump(uint8_t a, uint8_t b); /* is there a visible step between them */

/* How many palette steps between neighbouring samples counts as a step worth
 * supersampling away. Small enough to catch the fringe, large enough that an
 * ordinary gradient - which crosses two or three steps per pixel at most -
 * costs nothing. */
#define FR_JUMP_STEPS 6
int      fr_find(uint8_t mode, scene_t *out);   /* somewhere worth looking */
int      fr_valid(const scene_t *s);    /* would this scene draw? */

/* maths, shared with palette.c */
uint32_t rnd(void);
void     rnd_seed(uint32_t s);
uint32_t rnd_state(void);
int      rnd_range(int lo, int hi);
int32_t  isin15(uint16_t turn);         /* turn 0..65535, result Q15 */
#define  icos15(t)  isin15((uint16_t)((t) + 16384u))
uint32_t ilog2_q16(uint32_t v);         /* log2 of a plain integer, Q16 */
uint32_t isqrt32(uint32_t v);           /* floor(sqrt) */

/* --------------------------------------------------------------- palette.c */
/*
 * 256 colours: entry 0 is the interior, entries 1..255 are one full turn
 * through the palette and wrap seamlessly, because a fractal's exterior is a
 * cycle - the bands never stop, they only get thinner.
 */
extern uint16_t g_pal[256];
extern uint32_t g_pal_k;        /* value -> palette position, Q16 */
extern uint32_t g_pal_phase;    /* where the cycle starts */

/*
 * The split between these two is what makes recolouring free.
 *
 * pal_map is the scene's business: how a value becomes a position in the
 * cycle, which depends on how deep this particular view goes. pal_new is the
 * palette's: what colour sits at each position. Only the second changes on an
 * L tap, so the stored per-pixel indices stay exactly right and the screen can
 * be repainted from a lookup table - and, for the same reason, a view restored
 * from NVS comes back as the same picture rather than merely a similar one.
 */
void     pal_new(uint32_t seed);        /* 0 = pick a fresh one */
uint32_t pal_seed(void);
void     pal_map(const scene_t *s);     /* fit the cycle to this scene */

/*
 * Value to colour, and the square root in the middle of it is the whole
 * difference between a picture and a rash.
 *
 * Escape counts pile up as the boundary is approached - the last few pixels
 * before it can cover half the iteration range - so a straight linear mapping
 * spends most of the palette in the fringe, where the bands are already
 * thinner than a pixel, and the result is confetti. The root compresses that
 * end and gives the room back to the open water, which is where a gradient can
 * actually be seen. It costs one isqrt32 per pixel.
 */
static inline uint8_t pal_shade(int32_t v)
{
    if (v < 0) return 0;
    uint32_t p = ((isqrt32((uint32_t)v) * g_pal_k) >> 16) + g_pal_phase;
    uint8_t s = (uint8_t)p;
    return s ? s : 1;           /* 0 belongs to the interior */
}

#endif /* MANDEL_H */
