#pragma once
#include <stdbool.h>
#include <stdio.h>
#include "traffic_minute.h"

bool traffic_csv_write(FILE *file, const traffic_minute_t *data,
                       bool (*lookup_name)(uint32_t ip, char name[256]));
