/*
 * guest.c - the loader.
 *
 * A .gbx image is position-independent in the only sense that matters here:
 * every absolute address inside it is stored as an offset from one of two
 * bases, and the loader adds the real base at load time. That costs four
 * bytes per patch site in the file and about twenty instructions per site at
 * load, and in exchange the image survives every OS rebuild - no fixed link
 * addresses, no regenerating guest linker scripts when the OS grows.
 *
 * Two bases, not one, because the two halves of a guest land in different
 * kinds of memory:
 *
 *   .text + literal pools  -> executable IRAM  (32-bit aligned access only)
 *   .rodata + .data + .bss -> ordinary DRAM    (byte addressable)
 *
 * Putting .rodata in IRAM would look like it worked until the first guest
 * read a string one byte at a time and took a LoadStoreError.
 *
 * Internal SRAM is not cached on the ESP32, so freshly written instructions
 * are visible to the fetch unit with no cache maintenance. Nothing here needs
 * an icache invalidate; that would only be true for XIP images in flash.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "board.h"
#include "osconf.h"
#include "st7789.h"
#include "guest.h"
#include "input.h"
#include "oswifi.h"

static const char *TAG = "guest";

#define GUEST_STACK_DEFAULT  4096
#define GUEST_PRIO           4          /* below input (6), above idle */
#define GUEST_GRACE_MS       400        /* how long a polite exit may take */

typedef struct alloc_node {
    struct alloc_node *next;
} alloc_node_t;

static struct {
    bool              running;
    volatile bool     stop;
    TickType_t        stop_at;     /* when the kill was asked for */
    TaskHandle_t      task;
    void             *text;         /* IRAM  */
    void             *data;         /* DRAM: rodata + data + bss */
    gb_entry_fn       entry;
    char              name[GB_NAME_MAX + 1];
    SemaphoreHandle_t done;
    SemaphoreHandle_t syscall;      /* held across short, hardware-touching calls */
    alloc_node_t     *allocs;
} g;

/* ------------------------------------------------------------ utilities */

/* Standard CRC-32, the zlib one, so mkguest.py can produce the same number
 * with zlib.crc32 and nothing has to agree about which ROM variant is which.
 * Bitwise rather than table-driven: it runs once per load over a few
 * kilobytes, which is not worth a kilobyte of table. */
static uint32_t crc32_le(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

/* IRAM tolerates 32-bit accesses only, so the usual memcpy is not an option
 * for the text image. Both ends are 4-aligned by construction. */
static void word_copy(void *dst, const void *src, size_t len)
{
    uint32_t       *d = (uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    for (size_t i = 0; i < len / 4; i++) d[i] = s[i];
}

size_t guest_exec_free(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
}

/* ---------------------------------------------------------- relocation */

static bool apply_relocs(const uint32_t *rel, uint32_t nrel,
                         const gb_hdr_t *h)
{
    const uint32_t tbase = (uint32_t)g.text;
    const uint32_t dbase = (uint32_t)g.data;

    for (uint32_t i = 0; i < nrel; i++) {
        uint32_t r    = rel[i];
        uint32_t off  = r & GB_REL_OFF_MASK;
        bool     in_d = (r & GB_REL_IN_DATA) != 0;
        uint32_t lim  = in_d ? h->data_len : h->text_len;

        if (off + 4 > lim || (off & 3)) {
            ESP_LOGE(TAG, "reloc %u out of range: off=%u in_%s lim=%u",
                     (unsigned)i, (unsigned)off, in_d ? "data" : "text",
                     (unsigned)lim);
            return false;
        }

        uint32_t *p    = (uint32_t *)((uint8_t *)(in_d ? g.data : g.text) + off);
        uint32_t  base = (r & GB_REL_TO_DATA) ? dbase : tbase;
        *p += base;                 /* read-modify-write, 32-bit, aligned */
    }
    return true;
}

/* -------------------------------------------------------------- loading */

static void guest_free_image(void)
{
    /* Anything the guest allocated through the API, whether or not it got to
     * free it itself. */
    alloc_node_t *n = g.allocs;
    while (n) { alloc_node_t *next = n->next; free(n); n = next; }
    g.allocs = NULL;

    if (g.text) { heap_caps_free(g.text); g.text = NULL; }
    if (g.data) { free(g.data);           g.data = NULL; }
    g.entry = NULL;
}

static void guest_trampoline(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "start %s entry=%p", g.name, (void *)g.entry);
    int rc = g.entry(&g_gb_api);
    ESP_LOGI(TAG, "exit %s rc=%d", g.name, rc);

    g.running = false;
    xSemaphoreGive(g.done);
    vTaskDelete(NULL);
}

esp_err_t guest_start(const char *path)
{
    if (g.running) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "open %s failed", path); return ESP_ERR_NOT_FOUND; }

    gb_hdr_t h;
    if (fread(&h, 1, sizeof h, f) != sizeof h) {
        fclose(f); ESP_LOGE(TAG, "%s: short header", path); return ESP_ERR_INVALID_SIZE;
    }
    if (h.magic != GB_MAGIC) {
        fclose(f); ESP_LOGE(TAG, "%s: bad magic %08x", path, (unsigned)h.magic);
        return ESP_ERR_INVALID_ARG;
    }
    if (h.abi != GB_ABI_VERSION) {
        fclose(f);
        ESP_LOGE(TAG, "%s: built for ABI %u, this OS speaks %u - rebuild it",
                 path, h.abi, (unsigned)GB_ABI_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    if (h.hdr_len != sizeof(gb_hdr_t) || (h.text_len | h.data_len) & 3 ||
        h.entry >= h.text_len) {
        fclose(f); ESP_LOGE(TAG, "%s: malformed header", path);
        return ESP_ERR_INVALID_ARG;
    }

    const size_t body = h.text_len + h.data_len + h.nrel * 4;

    /* The body is read into ordinary heap first so the CRC can be checked
     * before a single byte is placed anywhere executable. */
    uint8_t *buf = malloc(body);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = fread(buf, 1, body, f);
    fclose(f);
    if (got != body) {
        free(buf); ESP_LOGE(TAG, "%s: truncated (%u/%u)", path,
                            (unsigned)got, (unsigned)body);
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t crc = crc32_le(buf, body);
    if (crc != h.crc32) {
        free(buf);
        ESP_LOGE(TAG, "%s: crc %08x, expected %08x", path,
                 (unsigned)crc, (unsigned)h.crc32);
        return ESP_ERR_INVALID_CRC;
    }

    g.text = heap_caps_malloc(h.text_len ? h.text_len : 4,
                              MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    g.data = calloc(1, h.data_len + h.bss_len + 4);
    if (!g.text || !g.data) {
        ESP_LOGE(TAG, "%s: need %u B exec (largest free %u) + %u B data",
                 path, (unsigned)h.text_len, (unsigned)guest_exec_free(),
                 (unsigned)(h.data_len + h.bss_len));
        guest_free_image(); free(buf);
        return ESP_ERR_NO_MEM;
    }

    word_copy(g.text, buf, h.text_len);
    memcpy(g.data, buf + h.text_len, h.data_len);
    /* bss is already zero: calloc. */

    if (!apply_relocs((const uint32_t *)(buf + h.text_len + h.data_len),
                      h.nrel, &h)) {
        guest_free_image(); free(buf);
        return ESP_ERR_INVALID_ARG;
    }
    free(buf);

    /* A previous run that had to be hard-killed may have left the exit
     * semaphore signalled. Clear it before anyone waits on it. */
    xSemaphoreTake(g.done, 0);

    memcpy(g.name, h.name, GB_NAME_MAX);
    g.name[GB_NAME_MAX] = 0;
    g.entry   = (gb_entry_fn)((uint8_t *)g.text + h.entry);
    g.stop    = false;
    g.running = true;

    input_drain();      /* the long-press that launched this is not the guest's */

    uint32_t stack = h.stack ? h.stack : GUEST_STACK_DEFAULT;
    if (xTaskCreate(guest_trampoline, g.name, stack, NULL, GUEST_PRIO, &g.task)
        != pdPASS) {
        g.running = false;
        guest_free_image();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "%s loaded: text %u@%p data %u+%u@%p relocs %u",
             g.name, (unsigned)h.text_len, g.text,
             (unsigned)h.data_len, (unsigned)h.bss_len, g.data,
             (unsigned)h.nrel);
    return ESP_OK;
}

/* ---------------------------------------------------------- lifecycle */

void guest_request_kill(void)
{
    if (!g.running || g.stop) return;
    g.stop    = true;
    g.stop_at = xTaskGetTickCount();
    /* A guest parked in wait_event needs something to return from. An empty
     * event is enough - it will fall through to its should_stop check. */
    input_inject(GB_EV_NONE);
}

/* Delete the task outright. Only reached when a guest ignored the stop flag
 * past the grace period. */
static bool guest_hard_kill(void)
{
    ESP_LOGW(TAG, "%s ignored the stop flag, deleting the task", g.name);

    /* Do not delete a task that is inside a syscall: it could be holding the
     * SPI bus. Getting the guard means the guest is either between calls or
     * blocked somewhere harmless. */
    if (xSemaphoreTake(g.syscall, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "%s wedged inside a syscall; leaking its memory", g.name);
        g.running = false;
        return false;              /* deliberately do NOT free the image */
    }
    vTaskDelete(g.task);
    xSemaphoreGive(g.syscall);
    g.running = false;
    return true;
}

/* Defined with the rest of the quit overlay, below. */
static void quit_hint_release(void);

void guest_supervise(void)
{
    if (!g.running && !g.text) return;

    bool reclaim = true;
    for (;;) {
        /* Clean exit: the trampoline gives this after gb_main returns. */
        if (xSemaphoreTake(g.done, pdMS_TO_TICKS(50)) == pdTRUE) break;

        if (g.stop &&
            (xTaskGetTickCount() - g.stop_at) > pdMS_TO_TICKS(GUEST_GRACE_MS)) {
            reclaim = guest_hard_kill();
            break;
        }
    }

    if (reclaim) guest_free_image();

    /* Hand the quit bar's scanlines back before the launcher repaints into
     * them: a clipped launcher would keep a stale band of the guest's last
     * frame along the bottom until something else made it redraw. */
    quit_hint_release();

    /* Whatever the guest did to the panel, the launcher expects the system
     * orientation back - including when the guest declined it and ran sideways
     * on purpose. Skipped in the wedged case: that guest is still alive
     * somewhere inside a syscall and may well be holding the SPI bus, and a
     * second writer on the bus is worse than a sideways launcher. */
    if (reclaim) st7789_set_rotation(osconf_rotation());

    /* And the radio, for the same reason and with the same exception. A guest
     * that woke it is the only thing on the board that wanted it up, so it
     * goes down here rather than leaving the next program to wonder where 21 KB
     * of heap went. The wedged guest keeps it: that task is still alive and
     * may be inside a scan, and tearing the driver out from under it would
     * turn a leak into a crash. */
    if (reclaim) oswifi_release();

    input_drain();
}

bool        guest_is_running(void)   { return g.running; }
const char *guest_name(void)         { return g.name; }
bool        guest_stop_requested(void) { return g.stop; }

void guest_syscall_enter(void) { xSemaphoreTake(g.syscall, portMAX_DELAY); }
void guest_syscall_exit(void)  { xSemaphoreGive(g.syscall); }

/* ------------------------------------------------------- the quit overlay */
/*
 * A left hold is three seconds, and on a guest it ends in a kill. Three
 * seconds of nothing happening is indistinguishable from a board that has
 * stopped listening, so one second in, the OS starts drawing a bar across the
 * bottom of the screen labelled "quit" and fills it over the remaining two.
 * Letting go before it fills cancels, and the bar is the only thing that says
 * so.
 *
 * Drawn from the input task, on top of whatever the guest has on screen, which
 * is why it takes the syscall guard - and why it takes it with a timeout of a
 * few milliseconds rather than waiting. The input task is the one task that
 * must never wait on a guest: a program wedged inside a syscall holds that
 * guard indefinitely, and blocking here would mean the kill gesture stopped
 * working in precisely the case it exists for. The bound is what matters, not
 * its exact value - short against the 10 ms poll, long enough to outlast the
 * single full-screen fill a busy guest is usually inside. A missed pass of the
 * overlay is the right thing to lose, and costs nothing: the fill is computed
 * from how long the button has been down, so the next pass that does get the
 * guard paints the bar where it should be by then.
 *
 * Priority over the guest is not a matter of drawing after it. Repainting the
 * bar into rows a guest clears forty times a second is a race the OS loses
 * about half the time, and half a bar is worse than none. So the strip is
 * *taken*: st7789_reserve() clips every guest draw above it for as long as the
 * hold lasts, and the OS lifts that clip only for its own painting. A guest
 * that repaints its whole frame now leaves the bar standing, and one that
 * repaints only what changed does too.
 *
 * What it covers is still gone. MISO is not wired on this board, so the panel
 * cannot be read back and a cancelled hold has nothing to restore; the strip
 * is erased to the theme background as the rows are handed back, and the guest
 * repaints them on its own schedule.
 */

#define QUIT_BAR_H       13
#define QUIT_BAR_X       32   /* past the label, which is four characters */
#define QUIT_BAR_WAIT_MS  4   /* bounded, never indefinite - see above */

static bool    s_hint_shown;
static int16_t s_hint_px;   /* how much of the bar is already filled */
static uint8_t s_hint_rot;  /* the rotation it was painted at */

/* Give the strip back to whoever draws next, without drawing anything.
 *
 * Under the syscall guard, so an overlay pass already in flight cannot reserve
 * the rows again behind us. If the guard is unavailable the guest is wedged
 * inside a syscall and holding it - in which case the overlay, which only ever
 * takes it with a timeout, cannot be in there either, so clearing without it
 * is safe. */
static void quit_hint_release(void)
{
    bool guarded = xSemaphoreTake(g.syscall, pdMS_TO_TICKS(20)) == pdTRUE;
    st7789_reserve(0);
    s_hint_shown = false;
    s_hint_px    = 0;
    if (guarded) xSemaphoreGive(g.syscall);
}

void guest_quit_hint(uint32_t held_ms)
{
    bool want = g.running && held_ms >= IN_QUIT_HINT_MS;

    if (!want && !s_hint_shown) return;         /* the common case, no lock */
    if (xSemaphoreTake(g.syscall, pdMS_TO_TICKS(QUIT_BAR_WAIT_MS)) != pdTRUE)
        return;                                 /* guest has the bus, try later */

    /* Re-read under the guard. guest_supervise hands the strip back on this
     * same guard, so this is what stops a pass that began while a guest was
     * alive from reserving rows the launcher is about to draw in. */
    want = want && g.running;

    const gb_theme_t *t = osconf_theme();
    const int16_t W  = st7789_width(), H = st7789_height();
    const int16_t y  = (int16_t)(H - QUIT_BAR_H);
    const int16_t bw = (int16_t)(W - QUIT_BAR_X - 4);

    if (!want) {
        /* Give the rows back before erasing them: the erase is the OS's last
         * draw in that strip and wants no special treatment. Only if there is
         * still a guest to erase it for - once the guest is gone the launcher
         * owns the panel and has already repainted it. */
        st7789_reserve(0);
        if (g.running) st7789_fill_rect(0, y, W, QUIT_BAR_H, t->bg);
        s_hint_shown = false;
        s_hint_px    = 0;

    } else {
        /* Claim the strip before the first pixel goes into it, and lift the
         * clip for this pass only. From here to the override(false) below, the
         * OS is the only thing that can write those rows. */
        st7789_reserve(QUIT_BAR_H);
        st7789_reserve_override(true);

        /* A guest may rotate the panel mid-hold - `settings` does exactly that
         * while previewing an orientation - and a bar painted along the old
         * bottom edge is nowhere near the new one. Repaint rather than carry on
         * filling something that is no longer there. */
        if (!s_hint_shown || s_hint_rot != st7789_rotation()) {
            st7789_fill_rect(0, y, W, QUIT_BAR_H, t->surface);
            st7789_text(4, (int16_t)(y + 3), "quit", t->warn, t->surface, 1);
            st7789_fill_rect(QUIT_BAR_X, (int16_t)(y + 4), bw, 5, t->muted);
            s_hint_shown = true;
            s_hint_px    = 0;
            s_hint_rot   = st7789_rotation();
        }

        /* Only the newly earned pixels are painted. At a 10 ms poll that is a
         * one-pixel-wide write per pass instead of a full strip forty times a
         * second, on a bus the guest is trying to use for its own frames. */
        const uint32_t span = IN_LONG_L_MS - IN_QUIT_HINT_MS;
        uint32_t into = held_ms - IN_QUIT_HINT_MS;
        if (into > span) into = span;

        int16_t px = (int16_t)((uint32_t)bw * into / span);
        if (px > s_hint_px) {
            st7789_fill_rect((int16_t)(QUIT_BAR_X + s_hint_px),
                             (int16_t)(y + 4),
                             (int16_t)(px - s_hint_px), 5, t->warn);
            s_hint_px = px;
        }

        st7789_reserve_override(false);
    }

    xSemaphoreGive(g.syscall);
}

void *guest_track_alloc(size_t n)
{
    alloc_node_t *node = malloc(sizeof(alloc_node_t) + n);
    if (!node) return NULL;
    node->next = g.allocs;
    g.allocs   = node;
    return node + 1;
}

void guest_track_free(void *p)
{
    if (!p) return;
    alloc_node_t *want = (alloc_node_t *)p - 1;
    alloc_node_t **pp = &g.allocs;
    while (*pp) {
        if (*pp == want) { *pp = want->next; free(want); return; }
        pp = &(*pp)->next;
    }
    /* Not ours. A guest passing a foreign pointer is a guest bug, not an OS
     * one; refusing to free it is the safe response. */
}

void guest_init(void)
{
    g.done    = xSemaphoreCreateBinary();
    g.syscall = xSemaphoreCreateMutex();
}
