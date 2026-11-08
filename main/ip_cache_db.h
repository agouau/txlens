#pragma once
#include "ip_cache.h"

// Called only by the SD worker, while mounted. Return SQLite result codes.
// Load is staged: the caller merges only after SQLITE_OK. Invalid databases
// are deleted and recreated; transient I/O/full/read-only errors are retained.
int ip_cache_db_load(const char *path, ip_cache_t *snapshot);
int ip_cache_db_save(const char *path, const ip_cache_t *snapshot);
