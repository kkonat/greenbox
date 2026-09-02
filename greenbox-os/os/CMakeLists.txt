cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(greenbox)

# Programs are built separately by guests/build.ps1 and packed into the SPIFFS
# image from there. Build them first; an empty directory just yields an empty
# filesystem and a launcher that says so.
spiffs_create_partition_image(storage "${CMAKE_CURRENT_LIST_DIR}/../guests/out"
                              FLASH_IN_PROJECT)
