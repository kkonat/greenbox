/*
 * pacman.h - what the three pacman translation units share.
 *
 * Split by cost, the way astro is: maze.c is the board and the questions you
 * can ask it, pac_gfx.c is everything that runs per pixel (the camera
 * transform, the maze, the sprites), and pacman.c is everything that runs per
 * frame (movement, the ghosts, the camera's decisions, the state machine).
 */
#ifndef PACMAN_H
#define PACMAN_H

#include "greenbox_abi.h"
#include "gb_rt.h"
#include "gb_gfx.h"

/* ---------------------------------------------------------------- screen */
/*
 * Portrait, like astro, and for the same kind of reason turned the other way:
 * the board is 28 tiles wide by 31 tall, so it very nearly fills a square, and
 * the panel is 135x240 either way up. Portrait spends its long axis on the
 * board's long axis, which at full zoom-out is 4.8 pixels per tile; landscape
 * would be 4.3, and the HUD would eat a third of the playfield instead of a
 * tenth.
 */
#define SCR_MAX_W   240
#define SCR_MAX_H   240
#define BAND_H      16

extern const gb_api_t *A;
extern int16_t  g_w, g_h;
extern uint16_t g_fb[SCR_MAX_W * BAND_H];

/* The strips the HUD keeps for itself. Everything the camera draws is clipped
 * between them - see cam_clip() - so a ghost that wanders up to the top of the
 * view slides under the score rather than over it. */
#define HUD_TOP     12
#define HUD_BOT     11

/* ----------------------------------------------------------------- board */

#define MAZE_W      28
#define MAZE_H      31

/* Tile kinds. The door is a wall to Pac-Man and a doorway to a ghost, which is
 * the whole of the ghost house rule. */
#define T_WALL      0
#define T_OPEN      1
#define T_DOOR      2

/* Both wrap x: the tunnel row runs off one side of the board and back on the
 * other, and every caller would otherwise have to remember that. Rows above
 * and below the board are walls. */
uint8_t maze_tile(int tx, int ty);
int     maze_passable(int tx, int ty, int for_ghost);

/* What is left to eat. 0 none, 1 pellet, 2 energizer. */
uint8_t maze_dot(int tx, int ty);
void    maze_eat(int tx, int ty);
void    maze_reset(void);
int     maze_dots_left(void);
int     maze_dots_eaten(void);

/* Where things start, in tiles. The half-tile offsets are the arcade's: an
 * entity sits on a lane centre in one axis and between two tiles in the other,
 * which is why every position below is in Q8 tile units rather than tiles. */
#define TILE_Q      8                       /* fixed point: 1 tile = 256 */
#define TQ(t)       ((int32_t)((t) * 256))
#define TILE_OF(q)  ((int)((q) >> TILE_Q))
#define CENTRE_OF(t) (TQ(t) + 128)

#define PAC_START_X  ((int32_t)(13 * 256 + 256))    /* between 13 and 14 */
#define PAC_START_Y  (CENTRE_OF(23))
#define HOUSE_X      ((int32_t)(13 * 256 + 256))
#define HOUSE_Y      (CENTRE_OF(14))
#define GATE_Y       (CENTRE_OF(11))                /* just above the door */
#define FRUIT_X      HOUSE_X
#define FRUIT_Y      (CENTRE_OF(17))

/* --------------------------------------------------------------- entities */

/* 0 right, 1 down, 2 left, 3 up: +1 is a right turn on screen, -1 a left one,
 * +2 is the about-face. The whole control scheme is arithmetic on this. */
enum { D_R = 0, D_D = 1, D_L = 2, D_U = 3 };
extern const int8_t DX[4], DY[4];

typedef struct {
    int32_t x, y;       /* Q8 tile units, centre of the sprite */
    uint8_t dir;
    uint8_t stopped;    /* ran into a wall and is waiting to be turned */
    /*
     * A turn that has been asked for and is waiting for somewhere to happen.
     * It is stored as a ROTATION rather than as a direction - -1 for left, +1
     * for right - because that is what the button means and because it stays
     * true after the corner it is taken on. `want_left` is how many more tile
     * centres it may go on looking at before it gives up.
     */
    int8_t  want_rot;
    uint8_t want_left;
} mover_t;

/* Ghost state. HOUSE and LEAVING are the only ones that do not use the
 * ordinary tile AI; EYES is a ghost that has been eaten and is walking home. */
typedef enum { GH_HOUSE = 0, GH_LEAVING, GH_OUT, GH_EYES, GH_ENTER } ghost_st_t;

typedef struct {
    mover_t    m;
    ghost_st_t st;
    uint8_t    kind;        /* 0 blinky, 1 pinky, 2 inky, 3 clyde */
    uint8_t    fright;      /* blue, and worth eating */
    int32_t    bob;         /* house bobbing, Q8 */
    int8_t     bob_dir;
    uint8_t    dot_limit_met;
} ghost_t;

#define NGHOST 4

/* The viewport: the panel minus the two HUD strips. Everything the camera
 * frames lives between them, and the camera's idea of "fits on screen" is
 * this rectangle rather than the panel. */
#define VIEW_Y0     HUD_TOP
#define VIEW_W      ((int)g_w)
#define VIEW_H      ((int)g_h - HUD_TOP - HUD_BOT)
#define VIEW_CX     ((int)g_w / 2)
#define VIEW_CY     (HUD_TOP + VIEW_H / 2)

/* Two things the renderer needs to know that are properly game state: whether
 * the energizers are in their bright half-second, and whether the board is in
 * the white flash that says the level is over. */
extern uint8_t g_blink;
extern uint8_t g_flash;

/* --------------------------------------------------------------- camera */
/*
 * The reason this program exists in the shape it does. See pacman.c for what
 * drives it; pac_gfx.c only reads it.
 *
 * x, y     centre of the view, Q8 tile units
 * z        zoom, Q8 pixels per tile
 * t*       where each is heading; the smoothing walks the pair together
 */
typedef struct {
    int32_t x, y, z;
    int32_t tx, ty, tz;
    int32_t hold_ms;        /* how long the roomier framing has been asked for */
    int32_t zmin;           /* the whole board in the viewport - never wider */
} cam_t;

extern cam_t CAM;

/* Screen pixel of a world coordinate, and back. Q8 in, whole pixels out. */
int  cam_sx(int32_t wx);
int  cam_sy(int32_t wy);
int32_t cam_wx(int px);     /* the world x at the left edge of pixel px */
int32_t cam_wy(int py);
/* The same as cam_sx, in Q8 pixels: tile edges are rounded from this so that
 * two neighbouring tiles always agree about the boundary between them. */
int32_t cam_sx_q8(int32_t wx);
int32_t cam_sy_q8(int32_t wy);

void cam_clip(void);        /* clip the surface to the viewport */
void cam_unclip(void);

/* ------------------------------------------------------------- rendering */

void draw_maze(void);                       /* walls, pellets, the door */
void draw_pac(int32_t wx, int32_t wy, int dir, int mouth, uint16_t col);
void draw_ghost(const ghost_t *g, int flash);
void draw_eyes_only(int cx, int cy, int r, int dir);
void draw_fruit(int32_t wx, int32_t wy, int kind);
void draw_life_icon(int x, int y, int r);

/*
 * The two button hints: where L and R would send him if pressed now, drawn as
 * arrows over the board at the corners nearest the buttons themselves.
 * `armed` is -1, 0 or +1 - the rotation of a turn already waiting - and that
 * arrow is drawn solid enough to be read as a state rather than as advice.
 *
 * `flipped` says the panel is upside down relative to the buttons, which
 * happens when the board is held the other way up: the hints move to the top
 * of the screen and swap sides, so each one stays beside the button it is
 * talking about.
 */
void draw_hints(int dir_l, int dir_r, int armed, int flipped);

/* Entity radius in pixels at the current zoom. Never smaller than PAC_MIN_R:
 * the point of the camera is that everything stays visible, and a Pac-Man that
 * has shrunk to two pixels because the ghosts spread out is not visible even
 * though it is technically on screen. */
#define PAC_MIN_R   3
int  ent_radius(void);

/* --------------------------------------------------------------- palette */

#define C_BG        GB_RGB(  0,   0,   0)
#define C_WALL      GB_RGB( 12,  14,  56)   /* filled, so a corridor reads */
#define C_EDGE      GB_RGB( 48,  92, 240)   /* ...and outlined, so it reads far */
#define C_DOOR      GB_RGB(255, 184, 222)
#define C_PELLET    GB_RGB(255, 214, 168)
#define C_PAC       GB_RGB(255, 232,  40)
#define C_FRIGHT    GB_RGB( 56,  64, 255)   /* against a navy maze, not black */
#define C_FLASH     GB_RGB(230, 230, 255)
#define C_EYE       GB_RGB(255, 255, 255)
#define C_PUPIL     GB_RGB( 40,  40, 200)
#define C_TEXT      GB_RGB(240, 240, 240)
#define C_DIM       GB_RGB(120, 120, 140)
#define C_SHADOW    GB_RGB(  0,   0,  10)
#define C_SCORE     GB_RGB(120, 255, 255)

extern const uint16_t GHOST_COL[NGHOST];

/* Shadowed text, with pacman's shade of near-black filled in. */
static inline void pac_text(int x, int y, const char *s, uint16_t c, int sc)
{ gfx_text_sh(x, y, s, c, sc, C_SHADOW); }

static inline void pac_text_ctr(int y, const char *s, uint16_t c, int sc)
{ gfx_text_ctr(y, s, c, sc, C_SHADOW); }

#endif /* PACMAN_H */
