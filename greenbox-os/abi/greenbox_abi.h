/*
 * greenbox_abi.h - the contract between the OS and a guest program.
 *
 * This header is compiled into BOTH sides, so it must not include anything
 * from ESP-IDF or from libc beyond stdint/stddef. A guest sees the OS only
 * through the gb_api_t table handed to its entry point: no linking against
 * IDF, no libc, no relocation against OS symbols. That is what keeps a guest
 * a couple of kilobytes instead of two hundred.
 *
 * Rule for changing this file: any change to gb_api_t's layout - including
 * appending at the end - is a breaking change and must bump GB_ABI_VERSION.
 * A guest built against a longer table would read past the end of an older
 * OS's table. The loader refuses a mismatch outright rather than letting a
 * guest jump through a stale pointer.
 */
#ifndef GREENBOX_ABI_H
#define GREENBOX_ABI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define GB_ABI_VERSION   7u
#define GB_MAGIC         0x31584247u        /* "GBX1" little-endian */

/* ------------------------------------------------------------------ colour */
/* RGB565, same convention as the OS panel driver. */
#define GB_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define GB_BLACK   GB_RGB(0,0,0)
#define GB_WHITE   GB_RGB(255,255,255)
#define GB_RED     GB_RGB(255,0,0)
#define GB_GREEN   GB_RGB(0,255,0)
#define GB_BLUE    GB_RGB(0,0,255)
#define GB_YELLOW  GB_RGB(255,255,0)
#define GB_CYAN    GB_RGB(0,255,255)
#define GB_GREY    GB_RGB(128,128,128)
#define GB_DKGREY  GB_RGB(48,48,48)
#define GB_ORANGE  GB_RGB(255,140,0)

/* ------------------------------------------------------------------ events */
/*
 * The OS owns the buttons and does the debouncing and press-length
 * classification. Both long presses fire the moment the hold is earned, not on
 * release, and the two holds are not the same length: accept is a second,
 * escape is a deliberate three, because escape is also the kill.
 *
 * A left hold over a running guest also draws on its screen. One second in,
 * the OS paints a progress bar labelled "quit" across the bottom 13 rows and
 * fills it over the remaining two seconds, so that a three-second hold is not
 * three seconds of no feedback. It cannot ask the guest to make room, and it
 * cannot put back what it covered - the panel is write-only on this board - so
 * a cancelled hold leaves that strip in the theme background colour. A guest
 * that repaints its whole frame never notices; one that repaints only what
 * changed does best to treat the bottom strip as volatile.
 *
 * A guest never sees GB_EV_KILL - that one is consumed by the OS to tear the
 * guest down. It does see GB_EV_L_LONG, and that is now the same gesture: the
 * OS delivers the event and starts the teardown at the same instant, so a
 * guest that returns from gb_main promptly exits cleanly and one that ignores
 * it is killed a few hundred milliseconds later. Either way the left hold
 * gets the user out.
 */
typedef enum {
    GB_EV_NONE      = 0,
    GB_EV_L_SHORT   = 1,   /* left  tap     - "back" / previous */
    GB_EV_R_SHORT   = 2,   /* right tap     - "next" */
    GB_EV_L_LONG    = 3,   /* left  held 3s - "escape"; the kill lands with it */
    GB_EV_R_LONG    = 4,   /* right held 1s - "accept" */
    GB_EV_KILL      = 5,   /* OS only, guests never receive it */
} gb_event_t;

/* Live button state, for api->buttons(). Held, not pressed: the bit is set for
 * as long as the button is down. */
#define GB_BTN_L  0x01u
#define GB_BTN_R  0x02u

/* ------------------------------------------------------------------- time */
typedef struct {
    uint16_t year;      /* full year, e.g. 2026 */
    uint8_t  mon;       /* 1-12 */
    uint8_t  day;       /* 1-31 */
    uint8_t  hour;      /* 0-23 */
    uint8_t  min;
    uint8_t  sec;
    uint8_t  wday;      /* 0 = Sunday */
    bool     valid;     /* false until something has actually set the clock */
} gb_tm_t;

/* -------------------------------------------------- settings and themes */
/*
 * The OS keeps one small settings record and lets guests read and write it.
 * Two fields today, both of them things the user picks once and expects the
 * whole board to respect.
 *
 * `rotation` is the SYSTEM orientation. The launcher and the info screen are
 * drawn in it, and the panel is already in it when gb_main is entered, so a
 * guest that does nothing at all honours it for free.
 *
 * It is a preference, not a rule. A program whose layout only works one way -
 * a vertical scroller, a top-down shooter - is expected to call set_rotation()
 * and render sideways to the user's choice. Declining costs it nothing: the OS
 * puts the system orientation back when the guest exits, including after a
 * kill, so the launcher always comes back the way the user left it.
 *
 * `theme` is a palette, not a stylesheet: seven colour roles the launcher
 * paints itself with, offered to guests through theme_get() so that a board
 * set to amber does not go navy the moment a program starts. A guest is free
 * to ignore them too - a game paints what a game paints.
 */
#define GB_THEME_NAME_MAX 10

typedef struct {
    uint16_t bg;        /* page background */
    uint16_t surface;   /* title bars, selected rows - one step up from bg */
    uint16_t fg;        /* primary text, on bg or on surface */
    uint16_t dim;       /* secondary text: values, unselected entries */
    uint16_t muted;     /* footers and rules - read only if you go looking */
    uint16_t accent;    /* the one bright colour: headings, cursors, markers */
    uint16_t warn;      /* wrong, missing or refused */
    char     name[GB_THEME_NAME_MAX];   /* NUL-terminated, e.g. "midnight" */
} gb_theme_t;

typedef struct {
    uint8_t rotation;   /* 0,2 portrait 135x240;  1,3 landscape 240x135 */
    uint8_t theme;      /* index into the OS theme table */
} gb_oscfg_t;

/* ------------------------------------------------------------------ radio */
/*
 * WiFi, in the only mode the OS offers a guest: listening.
 *
 * There is no association here, no IP stack and no credentials anywhere in the
 * API. A guest can ask what is on the air and how strongly it arrives, and
 * that is the whole of it - which is enough for a site survey, a channel
 * census or a hunt for one particular radio, and is not enough to do anything
 * to a network. Bringing the radio up costs about 21 KB of heap - measured on
 * the board, with the receive-only buffer sizes in sdkconfig.defaults - so it
 * stays down until a guest asks and the OS drops it again when the guest
 * exits.
 *
 * Two ways to listen, because the two questions want different answers:
 *
 *   wifi_scan()  asks "what is out there". It costs a dwell per channel and
 *                comes back with a sorted census.
 *   wifi_watch() asks "how strong is THIS one, right now". It parks the radio
 *                on one channel and hands over every frame that BSSID sends,
 *                which for a beacon alone is about ten a second.
 *
 * A scan cancels a watch - one radio, one channel - and the OS does not put
 * the watch back afterwards. A guest that interleaves the two re-arms its own
 * watch, so that the moment the radio goes back to the target is a decision
 * the guest made rather than one it inherited.
 */
#define GB_SSID_MAX   33        /* 32 octets plus the NUL */
#define GB_BSSID_LEN   6

typedef enum {
    GB_AUTH_OPEN = 0,
    GB_AUTH_WEP,
    GB_AUTH_WPA,
    GB_AUTH_WPA2,
    GB_AUTH_WPA3,
    GB_AUTH_ENTERPRISE,
    GB_AUTH_OTHER,
} gb_auth_t;

typedef struct {
    uint8_t bssid[GB_BSSID_LEN];
    uint8_t channel;            /* 1..13 here */
    int8_t  rssi;               /* dBm: about -30 touching it, -95 at the edge */
    uint8_t auth;               /* gb_auth_t */
    uint8_t hidden;             /* 1 if the beacon carried no SSID */
    uint8_t reserved[2];
    char    ssid[GB_SSID_MAX];  /* NUL-terminated; empty when hidden */
} gb_ap_t;

/* One frame heard from the watched BSSID. `kind` separates the AP's own
 * heartbeat from its traffic: beacons arrive on a timer whether anyone is
 * using the network or not, which is what makes them the thing to measure. */
#define GB_HIT_BEACON  0
#define GB_HIT_MGMT    1        /* probe response, and the rest of management */
#define GB_HIT_DATA    2

typedef struct {
    uint32_t t_ms;              /* millis() when the radio received it */
    int8_t   rssi;
    uint8_t  kind;              /* GB_HIT_* */
    uint8_t  channel;
    uint8_t  reserved;
} gb_hit_t;

/* --------------------------------------------------------------- graphics */
/*
 * A software rasteriser, and the one part of this table that touches no
 * hardware at all.
 *
 * The panel calls above are the whole of what a utility needs: fill a rect,
 * print a line, let the OS's 5x7 font do the rest. A game cannot use them.
 * api->pixel() is a windowed SPI transaction - three commands and four payload
 * bytes of address window, for one pixel - so a starfield drawn with it
 * manages single-digit frames per second, and anything drawn straight onto the
 * panel tears against the band that went down the bus a moment ago.
 *
 * So every game here assembles its frame in RAM one horizontal band at a time
 * and blits whole rows. astro wrote the primitives for that; pinball copied
 * them, and said so in its header; pacman would have been the third copy.
 * Three copies of the same two hundred lines of integer geometry is the point
 * at which the code belongs on the other side of the table.
 *
 * Note what the argument for that is NOT. Moving code from a guest to the OS
 * moves it from the 75 KB executable IRAM heap into IROM, which is plentiful,
 * and that sounds like the whole case until it is measured: a guest pays one
 * indirect call through this table where it used to pay a local one, and for a
 * guest whose drawing is per-pixel that is most of what the removal saved.
 * Measured across the change, pinball came out 1,084 bytes of text smaller and
 * astro 144. The case is that there is now ONE implementation of the clipping,
 * one circle rasteriser to get the band boundaries right, and a guest that
 * wants to draw starts by drawing.
 *
 * Two rules keep this from growing into a display server:
 *
 *   Nothing here touches hardware. Every call writes into a buffer the GUEST
 *   owns and passes in, which is why none of them is wrapped in the syscall
 *   guard: there is no SPI bus to be caught holding, no allocation to reclaim,
 *   and a guest deleted mid-disc leaves the OS holding no state whatsoever.
 *   Getting the finished band onto the glass is still api->blit(), and that
 *   one is guarded.
 *
 *   Nothing here is speculative. The table is the intersection of what astro
 *   and pinball had already written for themselves, not a wish list. Every
 *   entry existed twice before it was moved once.
 */

/*
 * The destination. `px` is w*h pixels of RGB565 that the guest allocated and
 * goes on owning; the OS reads the descriptor for the length of one call and
 * keeps nothing.
 *
 * Drawing coordinates are SCREEN coordinates - the ones api->blit() takes -
 * and (org_x, org_y) says where px[0] sits in them. A band renderer walks
 * org_y down the screen and changes nothing else, so drawing code is written
 * once in screen coordinates and simply called once per band.
 *
 * The clip rectangle is in those same screen coordinates, half open: x0 and y0
 * are in, x1 and y1 are the first column and row that are out. A rectangle
 * with no area means no clipping, so a surface that was memset to zero and
 * handed a buffer draws everywhere that buffer reaches. It is here for the one
 * thing a band cannot express - a viewport with a HUD above and below it,
 * where the world has to stop at row 12 even though the band it lands in
 * started at row 8.
 */
typedef struct {
    uint16_t *px;               /* w*h RGB565, guest-owned */
    int16_t   w, h;
    int16_t   org_x, org_y;     /* screen coordinates of px[0] */
    int16_t   clip_x0, clip_y0; /* inclusive */
    int16_t   clip_x1, clip_y1; /* exclusive; empty rectangle = no clip */
} gb_surf_t;

/*
 * Sprites are ASCII in the guest's source: one string per row, one character
 * per pixel, a dot for transparent, and a palette saying what each character
 * paints. It costs a small lookup per pixel - a rounding error next to the SPI
 * transfer that follows - and in exchange the art is editable in a text editor
 * by anyone, with no tool chain and no asset pipeline.
 */
typedef struct { char ch; uint16_t col; } gb_spal_t;

typedef struct {
    const char *const *rows;    /* nrows strings, ncols characters each */
    const gb_spal_t   *pal;
    int16_t  nrows, ncols, npal;
    int16_t  scale;             /* pixels per source pixel; 0 or 1 = 1:1 */
    int16_t  lean;              /* x offset of the top row against the bottom */
    uint16_t tint_to;           /* mix every colour towards this... */
    int16_t  tint;              /* ...by tint/256. 0 leaves the palette alone */
} gb_sprite_t;

typedef struct gb_gfx {
    /* -- spans and boxes ----------------------------------------------- */
    void (*row)(const gb_surf_t *s, int16_t y, uint16_t c);
    void (*hspan)(const gb_surf_t *s, int16_t x0, int16_t x1, int16_t y,
                  uint16_t c);                              /* x1 inclusive */
    void (*px)(const gb_surf_t *s, int16_t x, int16_t y, uint16_t c);
    void (*box)(const gb_surf_t *s, int16_t x, int16_t y,
                int16_t w, int16_t h, uint16_t c);
    void (*frame)(const gb_surf_t *s, int16_t x, int16_t y,
                  int16_t w, int16_t h, uint16_t c);

    /* -- curves --------------------------------------------------------- */
    /*
     * The outlines - ring and ellipse - are seeded from the row above the
     * band. An outline that forgot how wide it was on the previous row leaves
     * a gap wherever the curve moves sideways faster than one row down, and on
     * sixteen-row bands a twenty-pixel halo straddles a boundary most of the
     * time, so the gap would land in a different place every frame.
     */
    void (*disc)(const gb_surf_t *s, int16_t cx, int16_t cy, int16_t r, uint16_t c);
    void (*ring)(const gb_surf_t *s, int16_t cx, int16_t cy, int16_t r, uint16_t c);
    void (*ellipse)(const gb_surf_t *s, int16_t cx, int16_t cy,
                    int16_t rx, int16_t ry, uint16_t c);
    void (*line)(const gb_surf_t *s, int16_t x0, int16_t y0,
                 int16_t x1, int16_t y1, uint16_t c);
    /* A round-capped bar, drawn as a run of discs along the line: at r=4 and
     * 26 pixels long that is 27 discs, which costs less than working out four
     * rotated corners and filling between them. */
    void (*bar)(const gb_surf_t *s, int16_t x0, int16_t y0,
                int16_t x1, int16_t y1, int16_t r, uint16_t c);

    /* -- the band font --------------------------------------------------- */
    /* 3x5 on a 4-pixel pitch, scaled by whole pixels. Deliberately not
     * api->text(): that one paints onto the panel and would tear against a
     * band already sent, and at size 1 it is 5x7, which is too tall to label
     * the corner of a 135-pixel screen. */
    void    (*text)(const gb_surf_t *s, int16_t x, int16_t y, const char *str,
                    uint16_t c, uint8_t scale);
    int16_t (*text_w)(const char *str, uint8_t scale);

    /* -- sprites --------------------------------------------------------- */
    void (*sprite)(const gb_surf_t *s, const gb_sprite_t *sp,
                   int16_t x, int16_t y);

    /* -- the integer maths that goes with all of it ----------------------- */
    /* There is no libm on this side of the table and no reason to want one:
     * soft float drags in libgcc routines that cost more than the geometry
     * they were meant to simplify. */
    uint32_t (*isqrt)(uint32_t v);          /* exact floor(sqrt(v)) */
    int16_t  (*isin)(int16_t turn);         /* 256 turns to the circle, Q8 */
    uint16_t (*mix565)(uint16_t a, uint16_t b, int16_t t);  /* t 0..256 */
    uint32_t (*rnd)(void);                  /* xorshift32 */
    void     (*rnd_seed)(uint32_t s);
} gb_gfx_t;

/* ---------------------------------------------------------------- the API */
/*
 * Function-pointer table. Guests hold the pointer for their whole lifetime;
 * the OS guarantees it stays valid until gb_main returns.
 *
 * Everything here is safe to call from the guest task and only from the guest
 * task. The OS wraps each call in a syscall guard so that a kill request
 * arriving mid-call waits for the call to finish rather than deleting a task
 * that holds the SPI bus.
 */
typedef struct gb_api {
    uint32_t abi_version;        /* == GB_ABI_VERSION, checked by the loader */

    /* -- display ------------------------------------------------------- */
    int16_t (*width)(void);
    int16_t (*height)(void);
    void    (*fill)(uint16_t colour);
    void    (*fill_rect)(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c);
    void    (*rect)(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c);
    void    (*pixel)(int16_t x, int16_t y, uint16_t c);
    void    (*blit)(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px);
    /* Built-in 5x7 font scaled by `size`; returns the x just past the text. */
    int16_t (*text)(int16_t x, int16_t y, const char *s,
                    uint16_t fg, uint16_t bg, uint8_t size);
    int16_t (*text_width)(const char *s, uint8_t size);
    void    (*backlight)(bool on);
    /* Pick the panel orientation for the lifetime of this guest:
     *   0, 2 = portrait  135x240      1, 3 = landscape 240x135
     * width() and height() follow immediately. The OS restores its own
     * orientation when the guest exits, including after a kill, so a guest
     * never has to put it back. Changing rotation does not clear the panel. */
    void    (*set_rotation)(uint8_t rot);

    /* -- input --------------------------------------------------------- */
    /* Non-blocking; returns GB_EV_NONE when the queue is empty. */
    gb_event_t (*poll_event)(void);
    /* Blocks up to timeout_ms. Returns GB_EV_NONE on timeout. A guest that
     * blocks here still gets killed promptly: the OS wakes it first. */
    gb_event_t (*wait_event)(uint32_t timeout_ms);
    /*
     * The debounced buttons as they are right now, a mask of GB_BTN_*.
     *
     * The queue above deals in gestures, and a gesture is only known after the
     * fact: a tap when the button comes up, a hold when its threshold is
     * earned. Neither says when a hold ends, so nothing built on events alone
     * can move while a button is down and stop when it is let go. A game needs
     * that; a menu does not, which is why both exist.
     *
     * Sampled by the input task every 10 ms, so this can be one poll behind
     * the physical button and never more.
     */
    uint8_t (*buttons)(void);

    /* -- time ---------------------------------------------------------- */
    uint32_t (*millis)(void);           /* since boot, wraps after 49 days */
    void     (*get_time)(gb_tm_t *out); /* wall clock; .valid says if it is real */
    uint32_t (*unix_time)(void);        /* 0 if never set */
    void     (*sleep_ms)(uint32_t ms);
    /* Set the wall clock from broken-down LOCAL time - the same convention
     * get_time hands back, so a read-modify-write round-trips exactly. Only
     * year..sec are read; .wday and .valid are ignored and recomputed. False
     * means the fields did not describe a date the OS will accept, in which
     * case the clock is left alone. */
    bool     (*set_time)(const gb_tm_t *tm);

    /* -- lifecycle ----------------------------------------------------- */
    /* True once the OS wants the guest gone. Poll it in any long loop; a
     * guest that returns promptly is shut down cleanly, not hard-killed. */
    bool  (*should_stop)(void);
    void  (*log)(const char *msg);
    int   (*snprintf)(char *buf, size_t n, const char *fmt, ...);

    /* -- memory -------------------------------------------------------- */
    /* Tracked per guest and reclaimed automatically when the guest exits,
     * including after a hard kill. */
    void *(*alloc)(size_t n);
    void  (*free)(void *p);

    /* -- persistence --------------------------------------------------- */
    /* Small key/value blobs in NVS, namespaced per guest. Returns bytes
     * transferred, or -1. Writes are capped at GB_STORE_MAX. */
    int  (*store_get)(const char *key, void *buf, size_t len);
    int  (*store_put)(const char *key, const void *buf, size_t len);

    /* -- OS settings --------------------------------------------------- */
    /* The live settings record. A write persists immediately and takes effect
     * for the launcher as soon as it has the panel back; nothing on screen
     * moves under the caller's feet, so a guest that changes `rotation` and
     * wants to see it now calls set_rotation() as well. False means the record
     * was out of range and nothing was stored. */
    void (*oscfg_get)(gb_oscfg_t *out);
    bool (*oscfg_set)(const gb_oscfg_t *in);

    /* The theme table. Valid indices are 0..theme_count()-1, and theme_get
     * returns false past the end, so a guest never has to hardcode how many
     * themes this OS happens to ship. */
    uint8_t (*theme_count)(void);
    bool    (*theme_get)(uint8_t idx, gb_theme_t *out);

    /* -- radio --------------------------------------------------------- */
    /*
     * Power. The first wifi_scan() or wifi_watch() brings the radio up on its
     * own, so this is only for a guest that wants to choose the moment - or to
     * give the ~21 KB back without exiting. The OS powers it down when the
     * guest ends, kill or no kill, so nothing has to be put back.
     */
    bool (*wifi_power)(bool on);

    /*
     * One scan. `channel` 1..13 dwells on that channel alone; 0 sweeps all of
     * them, which costs thirteen dwells and is the slow way to ask a question
     * a guest usually wants answered eight times a second. `dwell_ms` is the
     * listen time per channel, clamped to something the radio will accept; 0
     * takes the OS default.
     *
     * Writes at most `max` records, strongest first - the sort is here rather
     * than in every caller - and returns how many, or -1 if the radio would
     * not come up. `max` above GB_SCAN_MAX is treated as GB_SCAN_MAX. When
     * more networks answer than fit, the ones dropped are the weakest.
     *
     * Blocks for the dwell. It returns early if the OS asks the guest to stop,
     * so a scan is never the reason a kill has to wait.
     */
    int (*wifi_scan)(gb_ap_t *out, int max, uint8_t channel, uint16_t dwell_ms);

    /*
     * Park on `channel` and record every frame `bssid` transmits. Returns
     * false if the radio would not come up; a NULL bssid stops the watch.
     *
     * The OS keeps the last GB_HIT_RING frames in a ring and overwrites the
     * oldest, so a guest that polls a few times a second never misses one and
     * a guest that stops polling loses the stale end rather than the fresh.
     */
    bool (*wifi_watch)(const uint8_t *bssid, uint8_t channel);

    /* Drain up to `max` frames, oldest first; returns how many, 0 when the
     * radio has heard nothing since the last call, -1 if no watch is armed. */
    int (*wifi_watch_poll)(gb_hit_t *out, int max);

    /* -- graphics ------------------------------------------------------ */
    /* The rasteriser described above. Never NULL: it is a static table in
     * the OS image, not something a guest has to ask for or test for. */
    const gb_gfx_t *gfx;
} gb_api_t;

#define GB_STORE_MAX 256

/* How many frames the OS holds for a watching guest. Ten beacons a second
 * plus whatever traffic the AP is carrying, so this is a couple of seconds
 * of silence from the guest rather than a couple of frames. */
/* The most networks one wifi_scan() will report. A dense block of flats runs
 * to about thirty beacons on 2.4 GHz, and a guest that asks for more than this
 * gets this - the ones it loses are the weakest, which are the ones a survey
 * has the least to say about. */
#define GB_SCAN_MAX  32

#define GB_HIT_RING  64

/* Every guest defines exactly this. Returning from it exits the guest. */
typedef int (*gb_entry_fn)(const gb_api_t *api);

/* ------------------------------------------------- binary image format */
/*
 * A .gbx file is: header, then the text image, then the data image, then the
 * relocation table. Text and data are each 4-byte aligned and the header
 * records the aligned lengths, so the loader finds each part by addition.
 *
 * The text image is copied into the executable-IRAM heap and the data image
 * into ordinary DRAM. They therefore land at two unrelated addresses, which
 * is why a relocation entry has to say both WHERE the word to patch lives and
 * WHICH of the two bases to add to it. mkguest.py normalises every patch site
 * to hold a plain offset from its section's origin, so the loader's job is a
 * single add.
 *
 * .rodata goes in the DATA image, not the text image, on purpose: IRAM on the
 * ESP32 only tolerates aligned 32-bit access, and string and font tables get
 * read a byte at a time.
 */
#define GB_REL_OFF_MASK   0x0FFFFFFFu   /* offset of the word to patch */
#define GB_REL_IN_DATA    0x10000000u   /* patch site is in the data image */
#define GB_REL_TO_DATA    0x20000000u   /* add data_base, not text_base */

#define GB_NAME_MAX 24

typedef struct {
    uint32_t magic;                 /* GB_MAGIC */
    uint16_t abi;                   /* GB_ABI_VERSION at build time */
    uint16_t hdr_len;               /* == sizeof(gb_hdr_t) */
    uint32_t text_len;              /* bytes, 4-aligned */
    uint32_t data_len;              /* rodata + data, bytes, 4-aligned */
    uint32_t bss_len;               /* zeroed immediately after data */
    uint32_t entry;                 /* offset of gb_main within the text image */
    uint32_t nrel;                  /* relocation entries following the data */
    uint32_t stack;                 /* requested task stack, 0 = OS default */
    char     name[GB_NAME_MAX];     /* NUL-padded, shown in the launcher */
    uint32_t crc32;                 /* over text + data + relocs */
} gb_hdr_t;

#endif /* GREENBOX_ABI_H */
