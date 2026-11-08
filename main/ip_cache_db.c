#include "ip_cache_db.h"
#include "sqlite3.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"

#define CACHE_APPLICATION_ID 1229996872 // 'IPCH'
#define CACHE_DB_MAX_BYTES (8 * 1024 * 1024)
static const char *TAG = "ip_cache_db";
static const char s_schema[] =
    "CREATE TABLE ip_cache(ip INTEGER PRIMARY KEY,accessed INTEGER NOT NULL,"
    "domain TEXT NOT NULL,location TEXT NOT NULL,network TEXT NOT NULL,"
    "dns_checked INTEGER NOT NULL,provider_checked INTEGER NOT NULL,"
    "provider_missing INTEGER NOT NULL,dns_expires_at INTEGER NOT NULL,provider_expires_at INTEGER NOT NULL)";

static int scalar(sqlite3 *db, const char *sql, sqlite3_int64 *value)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) { *value = sqlite3_column_int64(stmt, 0); rc = SQLITE_OK; }
        else if (rc == SQLITE_DONE) rc = SQLITE_CORRUPT;
    }
    sqlite3_finalize(stmt);
    return rc;
}

static int validate(sqlite3 *db)
{
    sqlite3_int64 value;
    int rc = scalar(db, "PRAGMA application_id", &value);
    if (rc != SQLITE_OK) return rc;
    if (value != CACHE_APPLICATION_ID) {
        ESP_LOGW(TAG, "Application ID mismatch: got %" PRId64 ", expected %d", (int64_t)value, CACHE_APPLICATION_ID);
        return SQLITE_NOTADB;
    }
    rc = scalar(db, "PRAGMA user_version", &value);
    if (rc != SQLITE_OK) return rc;
    if (value != 4) {
        ESP_LOGW(TAG, "Format version mismatch: got %" PRId64 ", expected 4", (int64_t)value);
        return SQLITE_NOTADB;
    }
    rc = scalar(db, "SELECT count(*) FROM sqlite_schema", &value);
    if (rc != SQLITE_OK) return rc;
    if (value != 1) {
        ESP_LOGW(TAG, "Unexpected schema objects: %" PRId64, (int64_t)value);
        return SQLITE_NOTADB; // Reject foreign tables/triggers/views.
    }
    const char *queries[] = {"PRAGMA quick_check", "SELECT sql FROM sqlite_schema WHERE name='ip_cache'"};
    const char *expected[] = {"ok", s_schema};
    for (size_t i = 0; i < 2; ++i) {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db, queries[i], -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                const char *text = (const char *)sqlite3_column_text(stmt, 0);
                rc = text && strcmp(text, expected[i]) == 0 ? SQLITE_OK : SQLITE_CORRUPT;
                if (rc != SQLITE_OK)
                    ESP_LOGW(TAG, "%s failed: %s", i == 0 ? "Integrity check" : "Table format check",
                             i == 0 && text ? text : "unexpected database format");
                if (rc == SQLITE_OK) {
                    int next = sqlite3_step(stmt);
                    if (next != SQLITE_DONE) rc = next == SQLITE_ROW ? SQLITE_CORRUPT : next;
                }
            } else if (rc == SQLITE_DONE) rc = SQLITE_CORRUPT;
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_OK) return rc;
    }
    return SQLITE_OK;
}

static bool invalid(int rc)
{
    return (rc & 255) == SQLITE_CORRUPT || (rc & 255) == SQLITE_NOTADB ||
           (rc & 255) == SQLITE_TOOBIG;
}

static int erase_db(const char *path)
{
    // Only this application's database and its SQLite sidecars are removed.
    const char *suffixes[] = {"-journal", "-wal", "-shm", ""};
    char name[256];
    for (size_t i = 0; i < 4; ++i) {
        int n = snprintf(name, sizeof(name), "%s%s", path, suffixes[i]);
        if (n < 0 || (size_t)n >= sizeof(name)) return SQLITE_CANTOPEN;
        if (remove(name) != 0 && errno != ENOENT) {
            ESP_LOGW(TAG, "Cannot remove %s: errno=%d (%s)", name, errno, strerror(errno));
            return SQLITE_IOERR_DELETE;
        }
    }
    return SQLITE_OK;
}

static int open_once(const char *path, sqlite3 **db)
{
    struct stat info;
    bool exists = stat(path, &info) == 0;
    if (!exists && errno != ENOENT) {
        ESP_LOGW(TAG, "Cannot stat %s: errno=%d (%s)", path, errno, strerror(errno));
        return SQLITE_IOERR_FSTAT;
    }
    if (exists && info.st_size > CACHE_DB_MAX_BYTES) {
        ESP_LOGW(TAG, "Database too large: %ld bytes", (long)info.st_size);
        return SQLITE_NOTADB;
    }
    int rc = sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        ESP_LOGW(TAG, "Open failed: SQLite %d (%s)", rc, *db ? sqlite3_errmsg(*db) : sqlite3_errstr(rc));
        return rc;
    }
    sqlite3_limit(*db, SQLITE_LIMIT_LENGTH, 4096);
    sqlite3_db_config(*db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL);
    sqlite3_db_config(*db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
    const char *stage = exists ? "Validate existing database" : "Create table";
    if (!exists) {
        rc = sqlite3_exec(*db, s_schema, NULL, NULL, NULL);
        if (rc == SQLITE_OK) {
            stage = "Set application ID/version";
            rc = sqlite3_exec(*db, "PRAGMA application_id=1229996872; PRAGMA user_version=4", NULL, NULL, NULL);
        }
    } else rc = validate(*db);
    if (rc == SQLITE_OK) {
        stage = "Configure journal/sync";
        rc = sqlite3_exec(*db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA temp_store=MEMORY;"
                              "PRAGMA cache_size=-64; PRAGMA max_page_count=2048", NULL, NULL, NULL);
    }
    if (rc != SQLITE_OK)
        ESP_LOGW(TAG, "%s failed: SQLite %d (%s); existed=%d, bytes=%ld", stage, rc,
                 sqlite3_errcode(*db) == SQLITE_OK ? sqlite3_errstr(rc) : sqlite3_errmsg(*db),
                 exists, exists ? (long)info.st_size : 0L);
    return rc;
}

static int open_db(const char *path, sqlite3 **db)
{
    *db = NULL;
    int rc = open_once(path, db);
    if (rc != SQLITE_OK) {
        sqlite3_close(*db); *db = NULL;
        if (invalid(rc)) {
            ESP_LOGW(TAG, "Rebuilding invalid database (SQLite %d)", rc);
            rc = erase_db(path);
            if (rc == SQLITE_OK) rc = open_once(path, db);
            if (rc != SQLITE_OK) ESP_LOGW(TAG, "Rebuild failed: SQLite %d", rc);
        }
    }
    if (rc != SQLITE_OK) { sqlite3_close(*db); *db = NULL; }
    return rc;
}

static bool column_text(sqlite3_stmt *stmt, int column, char *out, size_t capacity)
{
    if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) return false;
    const char *text = (const char *)sqlite3_column_text(stmt, column);
    int length = sqlite3_column_bytes(stmt, column);
    if (!text || length < 0 || (size_t)length >= capacity || memchr(text, 0, length)) return false;
    memcpy(out, text, (size_t)length + 1);
    return true;
}

int ip_cache_db_load(const char *path, ip_cache_t *snapshot)
{
    memset(snapshot->entries, 0, snapshot->count * sizeof(*snapshot->entries));
    snapshot->sequence = 0;
    sqlite3 *db = NULL;
    int rc = open_db(path, &db);
    if (rc != SQLITE_OK) return rc;
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT ip,accessed,domain,location,network,dns_checked,provider_checked,"
                               "provider_missing,dns_expires_at,provider_expires_at FROM ip_cache ORDER BY accessed DESC,ip LIMIT ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, (sqlite3_int64)snapshot->count);
    size_t count = 0;
    while (rc == SQLITE_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ip_cache_entry_t *entry = &snapshot->entries[count];
        sqlite3_int64 ip = sqlite3_column_int64(stmt, 0), rank = sqlite3_column_int64(stmt, 1);
        bool good = sqlite3_column_type(stmt, 0) == SQLITE_INTEGER && ip > 0 && ip <= UINT32_MAX &&
                    sqlite3_column_type(stmt, 1) == SQLITE_INTEGER && rank > 0;
        for (int col = 5; col <= 7; ++col)
            good = good && sqlite3_column_type(stmt, col) == SQLITE_INTEGER &&
                   (sqlite3_column_int64(stmt, col) == 0 || sqlite3_column_int64(stmt, col) == 1);
        good = good && sqlite3_column_type(stmt, 8) == SQLITE_INTEGER && sqlite3_column_int64(stmt, 8) >= 0 &&
                       sqlite3_column_type(stmt, 9) == SQLITE_INTEGER && sqlite3_column_int64(stmt, 9) >= 0 &&
                       column_text(stmt, 2, entry->info.result.ptr, sizeof(entry->info.result.ptr)) &&
                       column_text(stmt, 3, entry->info.result.location, sizeof(entry->info.result.location)) &&
                       column_text(stmt, 4, entry->info.result.network, sizeof(entry->info.result.network));
        if (!good) { rc = SQLITE_CORRUPT; break; }
        entry->info.result.ip = (uint32_t)ip;
        entry->info.dns_checked = sqlite3_column_int(stmt, 5) != 0;
        entry->info.provider_checked = sqlite3_column_int(stmt, 6) != 0;
        entry->info.provider_missing = sqlite3_column_int(stmt, 7) != 0;
        entry->info.dns_expires_at = sqlite3_column_int64(stmt, 8);
        entry->info.provider_expires_at = sqlite3_column_int64(stmt, 9);
        entry->used = true;
        entry->accessed = snapshot->count - count;
        ++count;
        rc = SQLITE_OK;
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    sqlite3_finalize(stmt);
    int closed = sqlite3_close(db);
    if (rc == SQLITE_OK) rc = closed;
    if (invalid(rc)) {
        rc = erase_db(path);
        if (rc == SQLITE_OK) rc = open_db(path, &db);
        if (rc == SQLITE_OK) rc = sqlite3_close(db);
        memset(snapshot->entries, 0, snapshot->count * sizeof(*snapshot->entries));
    }
    snapshot->sequence = snapshot->count;
    return rc;
}

int ip_cache_db_save(const char *path, const ip_cache_t *snapshot)
{
    sqlite3 *db = NULL;
    int rc = open_db(path, &db);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_exec(db, "BEGIN IMMEDIATE; DELETE FROM ip_cache", NULL, NULL, NULL);
    sqlite3_stmt *stmt = NULL;
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(db, "INSERT INTO ip_cache VALUES(?,?,?,?,?,?,?,?,?,?)", -1, &stmt, NULL);
    for (size_t i = 0; rc == SQLITE_OK && i < snapshot->count; ++i) {
        const ip_cache_entry_t *entry = &snapshot->entries[i];
        if (!entry->used) continue;
        const ip_cache_info_t *info = &entry->info;
        rc = sqlite3_bind_int64(stmt, 1, info->result.ip);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)entry->accessed);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, info->result.ptr, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, info->result.location, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, info->result.network, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 6, info->dns_checked);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 7, info->provider_checked);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 8, info->provider_missing);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 9, info->dns_expires_at);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 10, info->provider_expires_at);
        if (rc == SQLITE_OK) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) rc = SQLITE_OK;
        }
        int reset = sqlite3_reset(stmt);
        if (rc == SQLITE_OK) rc = reset;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_OK) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    int closed = sqlite3_close(db);
    return rc == SQLITE_OK ? closed : rc;
}
