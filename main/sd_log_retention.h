#pragma once

#include <stdbool.h>
#include <stdint.h>

// Delete oldest hours of YYYY/MM/DD/HH/mm.csv files until free space recovers.
// The caller owns the mounted card. False means space cannot be ensured.
// Outputs contain the last space reading, or zero if the query failed.
bool sd_log_prune(const char *root, uint64_t min_free_bytes,
                  uint64_t *total_bytes, uint64_t *free_bytes);
