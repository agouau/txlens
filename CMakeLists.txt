cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR
        "This board requires ESP32-S3 for its RGB LCD and PSRAM. "
        "Run 'idf.py set-target esp32s3', then build again.")
endif()
idf_build_set_property(MINIMAL_BUILD ON)
project(wifi_router)
