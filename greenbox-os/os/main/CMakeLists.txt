idf_component_register(
    SRCS "os_main.c" "st7789.c" "input.c" "guest.c" "gapi.c"
         "launcher.c" "ostime.c" "console.c" "osconf.c" "oswifi.c" "osgfx.c"
    INCLUDE_DIRS "." "../../abi"
    REQUIRES driver nvs_flash spiffs esp_partition app_update esp_timer
             esp_wifi esp_event)
