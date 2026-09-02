/*
 * maze.c - the board, and the two questions everything else asks it.
 *
 * The map is ASCII, 28 columns by 31 rows, for the same reason the sprites in
 * astro are: it is the only format in which the thing you are editing looks
 * like the thing you are editing. It costs 868 bytes of rodata and one lookup
 * per query, against a table of packed bitfields that nobody could read.
 *
 *   #   wall
 *   .   pellet
 *   o   energizer - four of them, one in each corner region
 *   =   the ghost house door: a wall to Pac-Man, a doorway to a ghost
 *   space  open, and nothing to eat there
 *
 * The layout is the arcade's, transcribed: 240 pellets and 4 energizers, the
 * tunnel on row 14, the house in the middle with its door on row 12. Two
 * places deliberately have no pellet - the tiles Pac-Man starts on, and the
 * whole of the tunnel - which is also the arcade's, and is why the count comes
 * out at 244 rather than at every open tile.
 *
 * Row 14 is worth a second look, because it is the one row that decides
 * whether the board has a dead end anywhere. It runs: tunnel, the junction
 * with the left vertical corridor at column 6, two tiles of connector, and
 * then the passage down the outside of the ghost house at column 9. If those
 * two connector tiles were walls, the passage beside the house would still be
 * reachable from above and below - but the ghosts pick their next tile from
 * the exits that are not behind them, and a pocket with one exit is the one
 * shape that rule cannot answer. There are none here, and the simulator's
 * flood fill says so on every run.
 */

#include "pacman.h"

static const char *const MAP[MAZE_H] = {
    "############################",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#o####.#####.##.#####.####o#",
    "#.####.#####.##.#####.####.#",
    "#..........................#",
    "#.####.##.########.##.####.#",
    "#.####.##.########.##.####.#",
    "#......##....##....##......#",
    "######.##### ## #####.######",
    "######.##### ## #####.######",
    "######.##          ##.######",
    "######.## ###==### ##.######",
    "######.## #      # ##.######",
    "      .   #      #   .      ",
    "######.## #      # ##.######",
    "######.## ######## ##.######",
    "######.##          ##.######",
    "######.## ######## ##.######",
    "######.## ######## ##.######",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#.####.#####.##.#####.####.#",
    "#o..##.......  .......##..o#",
    "###.##.##.########.##.##.###",
    "###.##.##.########.##.##.###",
    "#......##....##....##......#",
    "#.##########.##.##########.#",
    "#.##########.##.##########.#",
    "#..........................#",
    "############################",
};

/* What is still on the floor. Rebuilt from MAP at the start of every level. */
static uint8_t s_dot[MAZE_H][MAZE_W];
static int16_t s_left;
static int16_t s_eaten;

/* Wrapping x here rather than in every caller is not a convenience: the tunnel
 * is the one place where "the tile to my left" is a different arithmetic, and
 * a version of that arithmetic living at each call site is a version that gets
 * it wrong somewhere. */
static inline int wrapx(int tx)
{
    tx %= MAZE_W;
    return tx < 0 ? tx + MAZE_W : tx;
}

uint8_t maze_tile(int tx, int ty)
{
    if (ty < 0 || ty >= MAZE_H) return T_WALL;
    char c = MAP[ty][wrapx(tx)];
    if (c == '#') return T_WALL;
    if (c == '=') return T_DOOR;
    return T_OPEN;
}

int maze_passable(int tx, int ty, int for_ghost)
{
    uint8_t t = maze_tile(tx, ty);
    if (t == T_OPEN) return 1;
    if (t == T_DOOR) return for_ghost;
    return 0;
}

uint8_t maze_dot(int tx, int ty)
{
    if (ty < 0 || ty >= MAZE_H) return 0;
    return s_dot[ty][wrapx(tx)];
}

void maze_eat(int tx, int ty)
{
    if (ty < 0 || ty >= MAZE_H) return;
    tx = wrapx(tx);
    if (!s_dot[ty][tx]) return;
    s_dot[ty][tx] = 0;
    s_left--;
    s_eaten++;
}

void maze_reset(void)
{
    s_left = s_eaten = 0;
    for (int y = 0; y < MAZE_H; y++)
        for (int x = 0; x < MAZE_W; x++) {
            char c = MAP[y][x];
            uint8_t d = (c == '.') ? 1 : (c == 'o') ? 2 : 0;
            s_dot[y][x] = d;
            if (d) s_left++;
        }
}

int maze_dots_left(void)  { return s_left; }
int maze_dots_eaten(void) { return s_eaten; }
