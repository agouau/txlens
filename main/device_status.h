#pragma once

#include <stddef.h>

// Called once a second by the UI task, regardless of the selected tab.
void device_status_sample(void);
void device_status_format(char *text, size_t size);
