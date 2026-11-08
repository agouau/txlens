#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Initializes the startup-only background synchronization task.
esp_err_t app_clock_init(void);
bool app_clock_is_ready(void);
// Reads the local system clock; never accesses the network.
void app_clock_format(char *buffer, size_t size);
