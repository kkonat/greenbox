/*
 * os_main.c - bring-up order and nothing else.
 *
 * greenbox OS for the LILYGO TTGO T-Display. A launcher plus a program loader:
 * guests are a few kilobytes of relocatable machine code that reach the
 * hardware only through the table in gapi.c, so they carry no copy of IDF.
 *
 * Order matters here:
 *   NVS      - osconf, ostime and the guest key/value store all need it
 *   settings - the panel has to be brought up in the user's orientation
 *   panel    - so a failure after this point can be shown rather than logged
 *   input    - the kill gesture should work even if a later step wedges
 *   storage  - where the programs live
 *   launcher - takes over the main task and never returns
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "board.h"
#include "osconf.h"
#include "st7789.h"
#include "input.h"
#include "guest.h"
#include "ostime.h"
#include "console.h"
#include "launcher.h"

static const char *TAG = "os";

static void mount_storage(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = GB_PROG_DIR,
        .partition_label        = "storage",
        .max_files              = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(err));
        return;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK)
        ESP_LOGI(TAG, "storage %u/%u bytes used", (unsigned)used, (unsigned)total);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    osconf_init();
    ostime_init();

    /* The user's orientation, not the build's - straight into the right one
     * rather than flipping visibly a moment after boot. */
    st7789_init(osconf_rotation());
    if (!st7789_ready()) {
        ESP_LOGE(TAG, "panel did not come up - running headless");
    }
    st7789_backlight(true);

    guest_init();
    input_start();
    mount_storage();
    console_start();

    ESP_LOGI(TAG, "heap %u free, exec %u largest",
             (unsigned)esp_get_free_heap_size(), (unsigned)guest_exec_free());

    launcher_run();                 /* never returns */
}
