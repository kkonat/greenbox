/*
 * pac_gfx.c - the camera transform, and everything that runs per pixel.
 *
 * Nothing here decides anything. pacman.c moves the camera; this file answers
 * "where does world coordinate w land on the panel" and then draws the board
 * and the cast through that answer, one band at a time.
 *
 * ------------------------------------------------------------- the transform
 *
 * World coordinates are Q8 tile units - 256 to the tile - with the origin at
 * the top left corner of tile (0,0). The camera has a centre in those units
 * and a zoom in Q8 pixels per tile, so the map is one multiply:
 *
 *      screen_px = (world - centre) * zoom / 256  +  viewport centre
 *
 * kept in Q8 pixels to the last moment and rounded once. That rounding is the
 * only reason cam_sx_q8() is exported: a tile is drawn from the rounded
 * position of its left edge to one pixel short of the rounded position of the
 * NEXT tile's left edge, so two neighbours always agree about the boundary
 * between them. Rounding a width instead leaves a one-pixel gap or a one-pixel
 * overlap wherever the fractional part crosses a half, which at a zoom that
 * changes every frame is a seam that crawls across the maze.
 *
 * ------------------------------------------------------------------ the wrap
 *
 * The board is a cylinder: the tunnel on row 14 joins column 27 to column 0.
 * The maze is therefore drawn modulo 28, over whatever column range the
 * viewport happens to cover, which may run from -6 to 21. Nothing special
 * happens at the seam - the columns either side of it are both drawn, in their
 * true screen positions - so Pac-Man walks through the tunnel with the camera
 * tracking him the whole way instead of the view snapping half a board
 * sideways. What it costs is one modulo per tile lookup, which maze.c was
 * doing anyway.
 *
 * ------------------------------------------------------------ minimum sizes
 *
 * Every sprite has a floor on its radius. When all four ghosts spread to the
 * corners the camera pulls back to about five pixels per tile, and a Pac-Man
 * drawn honestly at that zoom is two pixels across - technically on screen,
 * practically not there. The floor makes him slightly too big for his corridor
 * at full zoom-out, which is the right way round: the whole point of the
 * camera is that you can see everyone.
 */

#include "pacman.h"

const gb_api_t *A;
int16_t  g_w, g_h;
uint16_t g_fb[SCR_MAX_W * BAND_H];

cam_t CAM;
uint8_t g_blink = 1;
uint8_t g_flash;

const int8_t DX[4] = {  1,  0, -1,  0 };
const int8_t DY[4] = {  0,  1,  0, -1 };

const uint16_t GHOST_COL[NGHOST] = {
    GB_RGB(255,  40,  30),      /* blinky */
    GB_RGB(255, 170, 220),      /* pinky  */
    GB_RGB( 60, 230, 240),      /* inky   */
    GB_RGB(255, 170,  70),      /* clyde  */
};

/* =============================================================== transform */

/*
 * The one place the cylinder is dealt with. A thing at column 0.2 and a camera
 * at column 27 are eight tenths of a tile apart, not twenty-seven, so the
 * offset is folded into the half board either side of the camera before it is
 * scaled. Pac-Man walking into the tunnel therefore slides off the right of
 * the view and back on at the left with the camera tracking him the whole way,
 * instead of the picture jumping a board width sideways at the seam.
 *
 * The maze does NOT go through here - see tile_sx_q8() - because folding is
 * per point, and a tile whose two edges landed on opposite sides of the fold
 * would be drawn a board wide.
 */
int32_t cam_sx_q8(int32_t wx)
{
    int32_t d = wx - CAM.x;
    if (d >  TQ(MAZE_W) / 2) d -= TQ(MAZE_W);
    if (d < -TQ(MAZE_W) / 2) d += TQ(MAZE_W);
    return ((d * CAM.z) >> 8) + ((int32_t)VIEW_CX << 8);
}

int32_t cam_sy_q8(int32_t wy)
{
    return (((wy - CAM.y) * CAM.z) >> 8) + ((int32_t)VIEW_CY << 8);
}

int cam_sx(int32_t wx) { return (int)((cam_sx_q8(wx) + 128) >> 8); }
int cam_sy(int32_t wy) { return (int)((cam_sy_q8(wy) + 128) >> 8); }

int32_t cam_wx(int px)
{
    return CAM.x + ((((int32_t)px - VIEW_CX) << 16) / CAM.z);
}

int32_t cam_wy(int py)
{
    return CAM.y + ((((int32_t)py - VIEW_CY) << 16) / CAM.z);
}

void cam_clip(void)   { gfx_clip(0, VIEW_Y0, g_w, VIEW_Y0 + VIEW_H); }
void cam_unclip(void) { gfx_noclip(); }

int ent_radius(void)
{
    /* 0.45 of a tile, so two sprites in neighbouring lanes do not touch. */
    int r = (int)((CAM.z * 115) >> 16);
    return r < PAC_MIN_R ? PAC_MIN_R : r;
}

/* The rows of the current band that are also inside the viewport. Every
 * routine below starts here and loops over nothing when the answer is empty,
 * which is most bands for most sprites. */
static int band_rows(int *y0, int *y1)
{
    int a = GS.org_y, b = GS.org_y + GS.h - 1;
    if (a < VIEW_Y0) a = VIEW_Y0;
    if (b > VIEW_Y0 + VIEW_H - 1) b = VIEW_Y0 + VIEW_H - 1;
    *y0 = a; *y1 = b;
    return b >= a;
}

/* ==================================================================== maze */

/*
 * Where a tile edge lands, straight off the line rather than folded. Column
 * -1 is drawn at the far left showing column 27's bricks, column 28 at the far
 * right showing column 0's, and the seam is a place where the board carries on
 * rather than a place where anything is decided.
 */
static int32_t s_tile_base;     /* screen Q8 of world x = 0, this frame */

static inline int tile_sx(int tx)
{
    return (int)((s_tile_base + ((TQ(tx) * CAM.z) >> 8) + 128) >> 8);
}

void draw_maze(void)
{
    int by0, by1;
    if (!band_rows(&by0, &by1)) return;

    s_tile_base = ((-CAM.x * CAM.z) >> 8) + ((int32_t)VIEW_CX << 8);

    /* Which tiles can reach these rows. TILE_OF is an arithmetic shift, so it
     * floors for the negative columns the wrap produces. */
    int ty0 = TILE_OF(cam_wy(by0));
    int ty1 = TILE_OF(cam_wy(by1)) + 1;
    if (ty0 < 0) ty0 = 0;
    if (ty1 > MAZE_H - 1) ty1 = MAZE_H - 1;
    if (ty1 < ty0) return;

    int tx0 = TILE_OF(cam_wx(0)) - 1;
    int tx1 = TILE_OF(cam_wx(g_w - 1)) + 1;

    uint16_t fill = g_flash ? GB_RGB(70, 70, 90) : C_WALL;
    uint16_t edge = g_flash ? GB_RGB(255, 255, 255) : C_EDGE;

    for (int ty = ty0; ty <= ty1; ty++) {
        int sy0 = cam_sy(TQ(ty));
        int sy1 = cam_sy(TQ(ty + 1)) - 1;
        if (sy1 < by0 || sy0 > by1) continue;
        if (sy1 < sy0) sy1 = sy0;               /* sub-pixel tile at min zoom */

        for (int tx = tx0; tx <= tx1; tx++) {
            uint8_t t = maze_tile(tx, ty);
            int sx0 = tile_sx(tx);
            int sx1 = tile_sx(tx + 1) - 1;
            if (sx1 < sx0) sx1 = sx0;

            if (t == T_WALL) {
                fb_box(sx0, sy0, sx1 - sx0 + 1, sy1 - sy0 + 1, fill);
                /* Outline only where the wall meets something walkable. Doing
                 * it per edge rather than per tile is what keeps the corridors
                 * legible at four pixels a tile: the bright line is the shape
                 * of the corridor, not of the block. */
                if (maze_tile(tx, ty - 1) != T_WALL)
                    fb_hspan(sx0, sx1, sy0, edge);
                if (maze_tile(tx, ty + 1) != T_WALL)
                    fb_hspan(sx0, sx1, sy1, edge);
                if (maze_tile(tx - 1, ty) != T_WALL)
                    fb_box(sx0, sy0, 1, sy1 - sy0 + 1, edge);
                if (maze_tile(tx + 1, ty) != T_WALL)
                    fb_box(sx1, sy0, 1, sy1 - sy0 + 1, edge);
                continue;
            }

            if (t == T_DOOR) {
                int mid = (sy0 + sy1) / 2;
                fb_hspan(sx0, sx1, mid, C_DOOR);
                continue;
            }

            uint8_t d = maze_dot(tx, ty);
            if (!d) continue;

            int cx = (sx0 + sx1) / 2;
            int cy = (sy0 + sy1) / 2;
            if (d == 1) {
                int ps = (int)(CAM.z >> 11);            /* a tile / 8 */
                if (ps < 1) ps = 1;
                fb_box(cx - ps / 2, cy - ps / 2, ps, ps, C_PELLET);
            } else if (g_blink) {
                int pr = (int)(CAM.z >> 10);            /* a tile / 4 */
                if (pr < 2) pr = 2;
                fb_disc(cx, cy, pr, C_PELLET);
            }
        }
    }
}

/* ================================================================ Pac-Man */
/*
 * A disc with a wedge taken out of it, solved per pixel.
 *
 * The wedge is symmetric about the heading, and the test for "inside it" is
 * one comparison with no arcsine and no division. Split the offset from the
 * centre into the part along the heading (b) and the part across it (a); the
 * angle between the pixel and the heading is at most the mouth's half angle
 * exactly when
 *
 *      |a| * cos(half) <= b * sin(half)
 *
 * which stays correct all the way round to a half angle of 180 degrees, where
 * cos is -1, sin is 0, and every pixel satisfies it. That matters because the
 * death animation is nothing but this angle opening from 0 to 180: Pac-Man
 * does not shrink or fade, his mouth swallows him, which is what the arcade
 * does and is one variable rather than an animation.
 */
void draw_pac(int32_t wx, int32_t wy, int dir, int mouth, uint16_t col)
{
    int by0, by1;
    if (!band_rows(&by0, &by1)) return;

    int cx = cam_sx(wx), cy = cam_sy(wy), r = ent_radius();
    int y0 = cy - r, y1 = cy + r;
    if (y0 < by0) y0 = by0;
    if (y1 > by1) y1 = by1;
    if (y1 < y0) return;

    int ux = DX[dir & 3], uy = DY[dir & 3];
    int co = gb_icos(mouth), si = gb_isin(mouth);

    for (int y = y0; y <= y1; y++) {
        int dy = y - cy;
        int t = r * r - dy * dy;
        if (t < 0) continue;
        int hw = (int)gb_isqrt((uint32_t)t);

        if (mouth <= 0) {                       /* closed: a plain disc */
            fb_hspan(cx - hw, cx + hw, y, col);
            continue;
        }
        for (int dx = -hw; dx <= hw; dx++) {
            int b = dx * ux + dy * uy;          /* along the heading */
            int a = dx * uy - dy * ux;          /* across it */
            if (a < 0) a = -a;
            if (a * co <= b * si) continue;     /* inside the mouth */
            fb_px(cx + dx, y, col);
        }
    }
}

/* ================================================================= ghosts */
/*
 * A dome on a skirt, and the skirt is where the character is. Three humps
 * along the bottom, cut as triangles rather than arcs: at the sizes this runs
 * at - four to ten pixels of radius - a triangular notch and a round one are
 * the same handful of pixels, and the triangle is a subtraction instead of a
 * second circle solve per column.
 */
static void ghost_body(int cx, int cy, int r, uint16_t col, int by0, int by1)
{
    int skirt = r / 3;
    if (skirt < 1) skirt = 1;
    int hump = (2 * r + 1) / 3;
    if (hump < 1) hump = 1;

    int y0 = cy - r, y1 = cy + r;
    if (y0 < by0) y0 = by0;
    if (y1 > by1) y1 = by1;

    for (int y = y0; y <= y1; y++) {
        int dy = y - cy;
        if (dy < 0) {                           /* the dome */
            int t = r * r - dy * dy;
            if (t < 0) continue;
            int hw = (int)gb_isqrt((uint32_t)t);
            fb_hspan(cx - hw, cx + hw, y, col);
        } else if (dy < r - skirt) {            /* the straight sides */
            fb_hspan(cx - r, cx + r, y, col);
        } else {                                /* the scallop */
            for (int dx = -r; dx <= r; dx++) {
                int u = (dx + r) % hump;
                int hc = hump / 2;
                int off = u - hc;
                if (off < 0) off = -off;
                int cut = hc ? (off * skirt) / hc : 0;
                if (dy <= r - cut) fb_px(cx + dx, y, col);
            }
        }
    }
}

static void ghost_eyes(int cx, int cy, int r, int dir, uint16_t white,
                       uint16_t pupil, int by0, int by1)
{
    int er = r / 3; if (er < 1) er = 1;
    int ex = r / 2; if (ex < 1) ex = 1;
    int ey = -r / 4;
    int px = (DX[dir & 3] * (er + 1)) / 2;
    int py = (DY[dir & 3] * (er + 1)) / 2;
    int pr = er / 2;

    for (int side = -1; side <= 1; side += 2) {
        int x = cx + side * ex;
        int y = cy + ey;
        if (y + er < by0 || y - er > by1) continue;
        if (er <= 1) {
            fb_px(x, y, white);
            fb_px(x + px, y + py, pupil);
        } else {
            fb_disc(x, y, er, white);
            fb_disc(x + px, y + py, pr < 1 ? 1 : pr, pupil);
        }
    }
}

void draw_eyes_only(int cx, int cy, int r, int dir)
{
    int by0, by1;
    if (!band_rows(&by0, &by1)) return;
    ghost_eyes(cx, cy, r, dir, C_EYE, C_PUPIL, by0, by1);
}

void draw_ghost(const ghost_t *g, int flash)
{
    int by0, by1;
    if (!band_rows(&by0, &by1)) return;

    int cx = cam_sx(g->m.x), cy = cam_sy(g->m.y), r = ent_radius();

    if (g->st == GH_EYES || g->st == GH_ENTER) {
        /* Eaten: what walks home is a pair of eyes. Drawing the body in some
         * dimmer colour would read as a fifth ghost. */
        ghost_eyes(cx, cy, r, g->m.dir, C_EYE, C_PUPIL, by0, by1);
        return;
    }

    if (g->fright) {
        uint16_t body = flash ? C_FLASH : C_FRIGHT;
        uint16_t face = flash ? C_FRIGHT : C_FLASH;
        ghost_body(cx, cy, r, body, by0, by1);
        /* Two eyes and a wavy mouth, and only if there are pixels to spend on
         * them: below three pixels of radius the face is noise, and a plain
         * blue blob still says "edible" because nothing else on the board is
         * that colour. */
        if (r >= 3) {
            int e = r / 3; if (e < 1) e = 1;
            fb_box(cx - r / 2 - e / 2, cy - r / 4 - e / 2, e, e, face);
            fb_box(cx + r / 2 - e / 2, cy - r / 4 - e / 2, e, e, face);
            for (int dx = -r + 1; dx <= r - 1; dx++) {
                int step = ((dx + r) / 2) & 1;
                fb_px(cx + dx, cy + r / 3 - step, face);
            }
        }
        return;
    }

    ghost_body(cx, cy, r, GHOST_COL[g->kind & 3], by0, by1);
    ghost_eyes(cx, cy, r, g->m.dir, C_EYE, C_PUPIL, by0, by1);
}

/* ================================================================== fruit */

void draw_fruit(int32_t wx, int32_t wy, int kind)
{
    int by0, by1;
    if (!band_rows(&by0, &by1)) return;

    int cx = cam_sx(wx), cy = cam_sy(wy), r = ent_radius();
    int br = r / 2; if (br < 2) br = 2;

    static const uint16_t FRUIT_COL[4] = {
        GB_RGB(230,  30,  40),      /* cherry     */
        GB_RGB(255, 140,  40),      /* orange     */
        GB_RGB(240, 240, 100),      /* apple-ish  */
        GB_RGB(120, 230, 120),      /* melon      */
    };
    uint16_t c = FRUIT_COL[kind & 3];

    if (cy + r < by0 || cy - r > by1) return;

    fb_disc(cx - br / 2, cy + br / 2, br, c);
    fb_disc(cx + br / 2, cy + br / 2, br, gb_mix565(c, GB_BLACK, 40));
    /* A stem, because two dots of the same colour is a powerup and two dots
     * with a stem is fruit. */
    fb_box(cx, cy - r, 1, r, GB_RGB(90, 200, 90));
}

/* ============================================================== the hints */
/*
 * These are the only things in the program drawn by READING the band buffer
 * back before writing it.
 *
 * There is no alpha in the panel, in RGB565 or in api->gfx - the OS rasteriser
 * writes colours, and blending is not one of the things it was worth putting
 * across the table for. It does not have to be: the surface is the guest's own
 * memory, handed to the OS one call at a time, so a guest that wants to mix
 * with what is already there just reads it. Two lines, no ABI.
 *
 * Which is the whole reason the buffer belongs to the guest rather than to the
 * OS. A display server would have had to grow a compositing model to allow
 * this; here it is arithmetic on an array you already own.
 */
static void blend_px(int x, int y, uint16_t c, int t)
{
    int by = y - GS.org_y;
    if (by < 0 || by >= GS.h) return;
    if (x < 0 || x >= GS.w) return;
    /* The clip rectangle is the OS's business everywhere else in this file, so
     * honour it here too rather than leaving one primitive that quietly does
     * not. */
    if (GS.clip_x1 > GS.clip_x0 && GS.clip_y1 > GS.clip_y0 &&
        (x < GS.clip_x0 || x >= GS.clip_x1 ||
         y < GS.clip_y0 || y >= GS.clip_y1)) return;
    uint16_t *p = &GS.px[by * GS.w + x];
    *p = gb_mix565(*p, c, t);
}

/*
 * The size of the hints, in pixels.
 *
 * Three times what they started at, because at eleven pixels across they were
 * something you had to go looking for, and the whole job of these is to be
 * readable out of the corner of an eye that is busy watching a ghost. At this
 * size the arrow is a third of the panel's width and unmissable, which is the
 * right trade on a screen this small: the thing it covers is a corner of the
 * maze you are not in.
 */
#define HINT_HEAD    15         /* half-width of the head, and its length */
#define HINT_SHAFT    9
#define HINT_INSET   19         /* centre, in from the panel edge */

/*
 * An arrow, as a triangle on a stub of shaft, built out of the direction
 * vector and its perpendicular so that one loop draws all four headings.
 * Solid rather than outlined: over a maze, an outline is a smudge.
 */
static void blend_arrow(int cx, int cy, int dir, uint16_t c, int t)
{
    int ux = DX[dir & 3], uy = DY[dir & 3];
    int vx = -uy, vy = ux;

    /* Put the tip half the total length out from the centre, so the arrow is
     * centred on the point it was asked for whichever way it faces. */
    int tx = cx + ux * ((HINT_HEAD + HINT_SHAFT) / 2);
    int ty = cy + uy * ((HINT_HEAD + HINT_SHAFT) / 2);

    for (int i = 0; i <= HINT_HEAD; i++) {
        int bx = tx - ux * i, by = ty - uy * i;
        for (int j = -i; j <= i; j++)
            blend_px(bx + vx * j, by + vy * j, c, t);
    }
    for (int i = HINT_HEAD + 1; i <= HINT_HEAD + HINT_SHAFT; i++) {
        int bx = tx - ux * i, by = ty - uy * i;
        for (int j = -HINT_HEAD / 3; j <= HINT_HEAD / 3; j++)
            blend_px(bx + vx * j, by + vy * j, c, t);
    }
}

/*
 * One arrow, with the shadow that makes it readable.
 *
 * These sit over the board, and the board is two colours: black corridors and
 * navy walls. A single translucent yellow reads well on one and disappears
 * into the other. So the same trick the text uses - an offset pass in
 * near-black underneath - which costs a second blend and works on both,
 * because what it adds is an edge rather than a colour.
 *
 * The bigger the arrow, the less of it should be paint: at this size a solid
 * fill would be a yellow slab over a third of the board, so the weights are
 * lower than they were when it was eleven pixels across, and the shadow does
 * more of the work of making it legible.
 */
static void hint_arrow(int cx, int cy, int dir, int strong)
{
    blend_arrow(cx + 2, cy + 2, dir, GB_RGB(0, 0, 0), strong ? 170 : 120);
    blend_arrow(cx, cy, dir, C_PAC, strong ? 215 : 115);
}

void draw_hints(int dir_l, int dir_r, int armed, int flipped)
{
    /* Beside the buttons, and the buttons are on one short edge of the board.
     * Held the other way up they are at the other end of the panel and the
     * hands have swapped, so the hints follow them rather than staying put. */
    /* The furthest any pixel of the arrow gets from its centre, which is the
     * head's half width rather than its length - a left-pointing arrow is
     * wider top to bottom than it is long. */
    int reach = HINT_HEAD + 1;
    int y  = flipped ? VIEW_Y0 + reach : VIEW_Y0 + VIEW_H - 1 - reach;
    int lx = flipped ? g_w - 1 - HINT_INSET : HINT_INSET;
    int rx = flipped ? HINT_INSET : g_w - 1 - HINT_INSET;

    /* A pending turn is drawn stronger: it has stopped being advice about what
     * a press would do and become a thing that has been asked for and is
     * waiting for somewhere to happen. */
    hint_arrow(lx, y, dir_l, armed < 0);
    hint_arrow(rx, y, dir_r, armed > 0);
}

/* ============================================================== HUD pieces */

void draw_life_icon(int x, int y, int r)
{
    int by0 = GS.org_y, by1 = GS.org_y + GS.h - 1;
    int y0 = y - r, y1 = y + r;
    if (y0 < by0) y0 = by0;
    if (y1 > by1) y1 = by1;

    for (int py = y0; py <= y1; py++) {
        int dy = py - y;
        int t = r * r - dy * dy;
        if (t < 0) continue;
        int hw = (int)gb_isqrt((uint32_t)t);
        /* A fixed 45-degree mouth facing right: at three pixels of radius
         * this is the whole difference between a life and a pellet. */
        int cut = (dy < 0 ? -dy : dy) - 1;
        if (cut > hw) cut = hw;
        fb_hspan(x - hw, x + cut, py, C_PAC);
    }
}
