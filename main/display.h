#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Board SD chip select on CH422G EXIO4. Call only after display_init().
esp_err_t display_sd_select(bool selected);

void display_init(void);
void display_process(void);

// Snapshot of the physical panel in RGB565, row-major from top left.
// Caller owns *pixels and must free() it. Does not call LVGL from the HTTP task.
esp_err_t display_capture_rgb565(uint16_t **pixels, int *width, int *height);
