#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SD_LOG_CHECKING,
    SD_LOG_UNAVAILABLE,
    SD_LOG_READY,
    SD_LOG_WRITING,
    SD_LOG_WRITE_ERROR,
} sd_log_status_t;

sd_log_status_t sd_log_get_status(void);

// Cached by the minute space check; false until a valid reading is available.
bool sd_log_get_space(uint64_t *used_bytes, uint64_t *total_bytes);

// Start after display_init(), app_clock_init() and ip_info_init().
esp_err_t sd_log_init(void);
