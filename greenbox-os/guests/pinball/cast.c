/*
 * cast.c - the two faces on the backglass, and everything that changes with
 * them.
 *
 * The table is the same table either way. What the cast changes is the wash
 * over the felt, the colour of the lamps and the rails, the three letters the
 * top lanes spell, and the portrait behind the playfield - which is picked
 * afresh at the start of every ball, so a three-ball game is normally one of
 * each rather than one of them all the way through.
 *
 * ------------------------------------------------------------------ the art
 *
 * Both faces were traced off a reference drawing rather than drawn by eye:
 * the reference was cropped to each head, box-filtered down to 34x30 cells,
 * and every cell resolved to whichever of the show's flat colours it was
 * nearest. That is why the palettes below are exact values - 89,209,237 for
 * Gumball, 255,117,19 for Darwin - and why the eyebrows, the pupils, the
 * muzzle and the tongue all land where they belong. What a quantiser cannot
 * do is invent what it could not see: Darwin stands in front of Gumball's
 * left cheek in the reference and Gumball's raised arm crosses the top right,
 * so those edges were closed by hand afterwards.
 *
 * The portrait is drawn tinted most of the way into the felt colour. At full
 * strength a 102x90 face reads as a picture standing in front of the table
 * and the ball disappears into it; at about three quarters of the way to the
 * felt it reads as printed on the playfield, which is what a real one is.
 */

#include "pinball.h"

/* =============================================================== gumball */
/*
 * Blue cat: a wide rounded head with a notch on top, two black eyebrow
 * strokes, eyes that take the whole upper half with pupils nearly as big, a
 * tan muzzle over an open pink mouth with two teeth in it.
 */
static const char *const GUMBALL_ART[CAST_H] = {
    "..................................",
    "...............kkkk...............",
    "..............kbbbk...............",
    "..............kbbbbkk.............",
    "...........kkkbbbbbbbkkk..........",
    ".........kkbbbbbbbbbbkkkkk........",
    "...kkkkkkkkkbbbbbbbbbkkkbbk.......",
    ".kkbbbbbbkkkbbbbbbbbbbbbbbk.......",
    ".kbbbbbbbbbbbbbbbbbbkkkkkkbk......",
    ".kbbbbbbkkkkkkbbbbbbbbbbbbbk......",
    ".kbbbbbbbbbbbbbbbbbbbwwwwbbk......",
    ".kbbbbbbbwwwwwbbbbbbwwwwwwbk......",
    "..kbbbbbwwwwwwwbbbbwwwwwwwbbk.....",
    "..kbbbbbwwwwwkkbbbbwwwwkkwwbk.....",
    "...kbbbwwwwwkkkbbbwwwwwkkwwbbkk...",
    "...kbbbwwwwwkkkbbbwwwwwkkwwbbbbk..",
    "...kbbbwwwwwwkkbbbbwwwwwwwwbbbbk..",
    "...kbbbbwwwwwwwbbbbwwwwwwwbbbbbbk.",
    "...kbbbbbwwwwwbbbbbnwwwwwbbbbbbbk.",
    "....kbbbbbwwwbwwwwnnnwwbbbbbbbbbk.",
    "....kbbbbbbbbbwwwwwnnwwbbbbbbbbbk.",
    "....kbbbbbbbbbwwpwwwwwwbbbbbbbbbk.",
    "....kbbbbbbbbbbppppppppbbbbbbbbbk.",
    ".....kbbbbbbbbbppppppppbbbbbbbbbk.",
    "......kbbbbbbbbqqqpppppbbbbbbbbbk.",
    ".......kbbbbbbbbqqqpppbbbbbbbbbk..",
    "........kkbbbbbbbqqqbbbbbbbbbbk...",
    "..........kkbbbbbbbbbbbbbbbkkk....",
    "............kkkkbbbbbbkkkkk.......",
    "................kkkkkk............",
};

static const spal_t GUMBALL_PAL[] = {
    { 'b', GB_RGB( 89, 209, 237) },     /* fur */
    { 'k', GB_RGB(  0,   0,   0) },     /* the show draws everything outlined */
    { 'w', GB_RGB(255, 255, 255) },     /* eye white */
    { 'n', GB_RGB(226,  86,  60) },     /* nose */
    { 'p', GB_RGB(240, 120, 130) },     /* open mouth */
    { 'q', GB_RGB(250, 176, 170) },     /* tongue */
};
#define GUMBALL_NPAL ((int)(sizeof GUMBALL_PAL / sizeof GUMBALL_PAL[0]))

/* ================================================================ darwin */
/*
 * Orange goldfish: rounder head, a fin off the left side, the same enormous
 * eyes, and a mouth open wide enough to show most of the tongue.
 */
static const char *const DARWIN_ART[CAST_H] = {
    "..................................",
    "............kkkkkkkk..............",
    "..........kkkkkoooookkk...........",
    "........kkoookoooooookkkk.........",
    ".......kooooooooooooookookk.......",
    ".......kooowwooooooooooooook......",
    ".......koowwwwwoooooooooooook.....",
    ".......kowwwwwwooooowwwwoooook....",
    ".......kwwwkwwwwooowwwwwwooook....",
    "......kowwkkkwwwooowwwwwwwooook...",
    ".....koowwkkkwwwoowwkkwwwwooook...",
    "....kooowwkkwwwwoowkkkwwwwoooook..",
    "..kkoooowwwwwwwwoowwkkwwwwoooook..",
    "..koooooowwwwwwoooowwwwwwwoooook..",
    ".kooooooooowwoooooowwwwwwooooook..",
    ".kooooooooooooooooooowwooooooook..",
    ".koooooooooooooooooooooooooooook..",
    ".koooooooorrrrrrrrrrrooooooooook..",
    ".kooooooorrrrrrrrrrrrrooooooook...",
    ".kooooooorrrrrrrrrrrrrooooooook...",
    "..koooooorrrrrrrrrrrrroooooooookk.",
    "..koooooorrrrtttttrrrooooooooooook",
    "...koooooorrttttttrrrooooooooooook",
    "..koooooooorttttttrroooooooooooook",
    "..koooooooooottttrooooooooooooook.",
    ".kooooookkkkkoooooooooooooooooook.",
    ".koooook.....kooooooooooooooookk..",
    "..kkoook.....koooooooooooooook....",
    "....kkk.......koooooooooooook.....",
    "..............kkkkkkkkkkkkkk......",
};

static const spal_t DARWIN_PAL[] = {
    { 'o', GB_RGB(255, 117,  19) },     /* scales */
    { 'k', GB_RGB(  0,   0,   0) },     /* outline, pupils, hair strands */
    { 'w', GB_RGB(255, 255, 255) },     /* eye white */
    { 'r', GB_RGB(200,  30,  40) },     /* open mouth */
    { 't', GB_RGB(232,  96, 104) },     /* tongue */
};
#define DARWIN_NPAL ((int)(sizeof DARWIN_PAL / sizeof DARWIN_PAL[0]))

/* ============================================================== the table */

static const cast_t CAST[2] = {
    {   .name     = "GUMBALL",
        .lane     = "GUM",
        .body     = GB_RGB( 89, 209, 237),
        .trim     = GB_RGB( 26, 112, 146),
        .glow     = GB_RGB(178, 240, 255),
        .felt_top = GB_RGB(  8,  30,  56),
        .felt_bot = GB_RGB(  4,  10,  26),
        .taunt    = "LETS DO THIS",
    },
    {   .name     = "DARWIN",
        .lane     = "DAR",
        .body     = GB_RGB(255, 117,  19),
        .trim     = GB_RGB(150,  58,   8),
        .glow     = GB_RGB(255, 190, 110),
        .felt_top = GB_RGB( 46,  20,   6),
        .felt_bot = GB_RGB( 16,   7,   4),
        .taunt    = "IM SO EXCITED",
    },
};

const cast_t *cast_get(int who)
{
    return &CAST[who & 1];
}

void cast_draw_backdrop(int who, int x, int y, int scale, uint16_t felt)
{
    if (who & 1)
        draw_sprite(DARWIN_ART, CAST_H, CAST_W, x, y,
                    DARWIN_PAL, DARWIN_NPAL, scale, felt, 100);
    else
        draw_sprite(GUMBALL_ART, CAST_H, CAST_W, x, y,
                    GUMBALL_PAL, GUMBALL_NPAL, scale, felt, 100);
}

void cast_draw_face(int who, int x, int y, int scale)
{
    if (who & 1)
        draw_sprite(DARWIN_ART, CAST_H, CAST_W, x, y,
                    DARWIN_PAL, DARWIN_NPAL, scale, 0, 0);
    else
        draw_sprite(GUMBALL_ART, CAST_H, CAST_W, x, y,
                    GUMBALL_PAL, GUMBALL_NPAL, scale, 0, 0);
}
