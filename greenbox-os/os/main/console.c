/*
 * console.c - a handful of serial commands over UART0.
 *
 * Not a shell and not trying to become one. It exists because two buttons is a
 * thin interface for bringing a system up: this is how the clock gets set
 * before there is any NTP, and how the button gestures get exercised without
 * a finger on the board.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "console.h"
#include "input.h"
#include "guest.h"
#include "osconf.h"
#include "ostime.h"
#include "oswifi.h"
#include "launcher.h"

#define CONSOLE_LINE_MAX 96

static void cmd_help(void)
{
    printf("commands:\n"
           "  ls                list programs\n"
           "  run <name>        launch one\n"
           "  kill              stop the running program\n"
           "  settime <epoch>   set UTC seconds since 1970\n"
           "  tz <minutes>      minutes east of UTC\n"
           "  conf              show the OS settings\n"
           "  conf rot <0-3>    0,2 portrait  1,3 landscape\n"
           "  conf theme <n>    colour theme\n"
           "  free              heap report\n"
           "  l r L R           inject tap-L, tap-R, hold-L, hold-R\n"
           "  reboot\n");
}

static void cmd_free(void)
{
    printf("heap  %u free, %u largest\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    printf("exec  %u largest (biggest guest that fits)\n",
           (unsigned)guest_exec_free());
    printf("min   %u ever free\n", (unsigned)esp_get_minimum_free_heap_size());
    /* Worth its own line: a heap report that did not say whether the radio
     * was up would read as 21 KB having gone missing. */
    printf("radio %s\n", oswifi_is_up() ? "up - about 21 KB of the above is its"
                                        : "down");
}

/* The button-free way to change what the settings program changes - handy when
 * the orientation is wrong enough that reading the screen is the problem. */
static void cmd_conf(char *arg)
{
    gb_oscfg_t c;
    osconf_get(&c);

    if (arg && *arg) {
        char *val = strchr(arg, ' ');
        if (val) { *val++ = 0; while (*val == ' ') val++; }
        if (!val || !*val) { printf("conf <rot|theme> <value>\n"); return; }

        if      (!strcmp(arg, "rot"))   c.rotation = (uint8_t)atoi(val);
        else if (!strcmp(arg, "theme")) c.theme    = (uint8_t)atoi(val);
        else { printf("no setting called %s\n", arg); return; }

        if (!osconf_set(&c)) { printf("out of range - nothing changed\n"); return; }
        /* The launcher owns the panel: it repaints itself in the new palette
         * and orientation, or does so as soon as the running guest is done. */
        launcher_repaint();
    }

    printf("rot   %u  %s\n", osconf_rotation(),
           (osconf_rotation() & 1) ? "landscape 240x135" : "portrait 135x240");

    osconf_get(&c);
    gb_theme_t th;
    for (uint8_t i = 0; osconf_theme_at(i, &th); i++)
        printf("theme %u  %-10s%s\n", i, th.name, i == c.theme ? "  <" : "");
}

static void handle(char *line)
{
    while (*line == ' ') line++;
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }

    if      (!strcmp(line, ""))       { }
    else if (!strcmp(line, "help"))   cmd_help();
    else if (!strcmp(line, "free"))   cmd_free();
    else if (!strcmp(line, "reboot")) esp_restart();
    else if (!strcmp(line, "ls"))     { launcher_rescan(); }
    else if (!strcmp(line, "kill"))   {
        if (guest_is_running()) { printf("killing %s\n", guest_name());
                                  guest_request_kill(); }
        else printf("nothing running\n");
    }
    else if (!strcmp(line, "run")) {
        if (!arg || !*arg) printf("run <name>\n");
        else if (guest_is_running()) printf("%s is already running\n", guest_name());
        /* The launcher task does the actual launching - the panel and the guest
         * lifecycle have exactly one owner, and it is not this task. */
        else if (launcher_request_run(arg)) printf("launching %s\n", arg);
        else printf("%s not found\n", arg);
    }
    else if (!strcmp(line, "settime")) {
        if (!arg) { printf("settime <epoch>\n"); return; }
        uint32_t t = (uint32_t)strtoul(arg, NULL, 10);
        if (t < 1600000000u) { printf("that is not a plausible epoch\n"); return; }
        ostime_set(t);
    }
    else if (!strcmp(line, "conf")) cmd_conf(arg);
    else if (!strcmp(line, "tz")) {
        if (!arg) { printf("tz is %+d minutes\n", ostime_tz()); return; }
        ostime_set_tz((int16_t)atoi(arg));
    }
    else if (!strcmp(line, "l")) input_inject(GB_EV_L_SHORT);
    else if (!strcmp(line, "r")) input_inject(GB_EV_R_SHORT);
    /* Faithful to the button: a left hold is the escape event and the kill
     * request together. `kill` on its own is still there for a guest that has
     * stopped reading events and would never see the escape. */
    else if (!strcmp(line, "L")) { input_inject(GB_EV_L_LONG);
                                   guest_request_kill(); }
    else if (!strcmp(line, "R")) input_inject(GB_EV_R_LONG);
    else printf("? %s (try help)\n", line);
}

static void console_task(void *arg)
{
    (void)arg;
    char line[CONSOLE_LINE_MAX];
    int  n = 0;

    cmd_help();
    for (;;) {
        uint8_t ch;
        int got = uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY);
        if (got != 1) continue;

        if (ch == '\r' || ch == '\n') {
            if (n) { line[n] = 0; printf("\n"); handle(line); n = 0; }
            printf("> ");
            fflush(stdout);
        } else if ((ch == 0x7f || ch == 8) && n) {
            n--; printf("\b \b"); fflush(stdout);
        } else if (n < CONSOLE_LINE_MAX - 1 && ch >= 0x20) {
            line[n++] = (char)ch;
            putchar(ch); fflush(stdout);
        }
    }
}

void console_start(void)
{
    const uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &cfg);

    xTaskCreate(console_task, "console", 3072, NULL, 3, NULL);
}
