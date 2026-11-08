#include "ip_cache_db.h"
#include "sqlite3.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

static ip_cache_entry_t entries[1000], restored[1000];
static ip_cache_t live = {.entries = entries, .count = 1000};
static ip_cache_t disk = {.entries = restored, .count = 1000};
static const char *path = "ip_cache.sqlite";

static int fail_read, fail_write, fail_sync;
static sqlite3_vfs *real_vfs;
static sqlite3_io_methods real_methods, fault_methods;
static int fault_read(sqlite3_file *file, void *data, int size, sqlite3_int64 offset)
{
    if (fail_read) { fail_read = 0; return SQLITE_IOERR_READ; }
    return real_methods.xRead(file, data, size, offset);
}
static int fault_write(sqlite3_file *file, const void *data, int size, sqlite3_int64 offset)
{
    if (fail_write) { int rc = fail_write; fail_write = 0; return rc; }
    return real_methods.xWrite(file, data, size, offset);
}
static int fault_sync(sqlite3_file *file, int flags)
{
    if (fail_sync) { fail_sync = 0; return SQLITE_IOERR_FSYNC; }
    return real_methods.xSync(file, flags);
}
static int fault_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags, int *out)
{
    (void)vfs;
    int rc = real_vfs->xOpen(real_vfs, name, file, flags, out);
    if (rc == SQLITE_OK) {
        real_methods = *file->pMethods;
        fault_methods = real_methods;
        fault_methods.xRead = fault_read;
        fault_methods.xWrite = fault_write;
        fault_methods.xSync = fault_sync;
        file->pMethods = &fault_methods;
    }
    return rc;
}
static void exec(const char *sql)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}
static int rows(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM ip_cache", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    assert(sqlite3_close(db) == SQLITE_OK);
    return count;
}
static void seed(void)
{
    for (uint32_t i = 1; i <= 1000; ++i) {
        ip_cache_entry_t *entry = ip_cache_acquire(&live, i);
        snprintf(entry->info.result.ptr, sizeof(entry->info.result.ptr), "host-%u.example", i);
        strcpy(entry->info.result.location, "CN / Guangdong");
        strcpy(entry->info.result.network, "Org: Example\nASN: 1234");
        entry->info.dns_checked = true;
        entry->info.provider_checked = true;
        entry->info.dns_expires_at = 1800000000 + i;
        entry->info.provider_expires_at = 1800010000 + i;
    }
    entries[0].info.result.ptr[0] = 0; // Preserve negative answers too.
    entries[0].info.provider_missing = true;
    entries[0].last_seen_us = 789012;
    entries[0].info.result.request_id = 42;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(sqlite3_initialize() == SQLITE_OK);
    real_vfs = sqlite3_vfs_find(NULL);
    static sqlite3_vfs fault_vfs;
    fault_vfs = *real_vfs;
    fault_vfs.zName = "fault";
    fault_vfs.xOpen = fault_open;
    assert(sqlite3_vfs_register(&fault_vfs, 1) == SQLITE_OK);
    if (argc == 1) {
        // SQLite probes the change counter at offset 24 even in a new file.
        // Reading it must report a zero-filled short read without growing it.
        sqlite3_file *file = calloc(1, (size_t)real_vfs->szOsFile);
        assert(file);
        assert(real_vfs->xOpen(real_vfs, "empty.sqlite", file,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB,
                              NULL) == SQLITE_OK);
        char data[16];
        memset(data, 0xa5, sizeof(data));
        assert(file->pMethods->xRead(file, data, sizeof(data), 24) == SQLITE_IOERR_SHORT_READ);
        for (size_t i = 0; i < sizeof(data); ++i) assert(data[i] == 0);
        sqlite3_int64 size;
        assert(file->pMethods->xFileSize(file, &size) == SQLITE_OK);
        if (size != 0) fprintf(stderr, "FAIL: read at offset 24 grew empty database to %lld bytes\n", size);
        assert(size == 0);
        assert(file->pMethods->xWrite(file, "abcd", 4, 0) == SQLITE_OK);
        memset(data, 0xa5, sizeof(data));
        assert(file->pMethods->xRead(file, data, 4, 2) == SQLITE_IOERR_SHORT_READ);
        assert(!memcmp(data, "cd\0\0", 4));
        assert(file->pMethods->xRead(file, data, sizeof(data), 24) == SQLITE_IOERR_SHORT_READ);
        for (size_t i = 0; i < sizeof(data); ++i) assert(data[i] == 0);
        assert(file->pMethods->xFileSize(file, &size) == SQLITE_OK && size == 4);
        assert(file->pMethods->xClose(file) == SQLITE_OK);
        free(file);
        assert(remove("empty.sqlite") == 0);
    }
    if (argc > 1 && strcmp(argv[1], "recover") == 0) {
        assert(_access("ip_cache.sqlite-journal", 0) == 0);
        assert(ip_cache_db_load(path, &disk) == SQLITE_OK);
        assert(strcmp(ip_cache_find(&disk, 1000)->info.result.ptr, "host-1000.example") == 0);
        assert(strcmp(ip_cache_find(&disk, 2)->info.result.ptr, "host-2.example") == 0);
        assert(!ip_cache_find(&disk, 1)->info.result.ptr[0]);
        assert(ip_cache_find(&disk, 2)->info.dns_expires_at == 1800000002);
        assert(ip_cache_find(&disk, 1000)->info.dns_expires_at == 1800001000);
        assert(ip_cache_find(&disk, 1000)->info.provider_expires_at == 1800011000);
        assert(rows() == 1000);
        puts("PASS: abrupt process exit during a spilled transaction; hot journal restores previous snapshot");
        return 0;
    }
    seed();
    if (argc > 1 && strcmp(argv[1], "crash") == 0) {
        assert(ip_cache_db_save(path, &live) == SQLITE_OK);
        sqlite3 *db = NULL;
        assert(sqlite3_open(path, &db) == SQLITE_OK);
        assert(sqlite3_exec(db, "PRAGMA cache_size=1; BEGIN IMMEDIATE; UPDATE ip_cache SET domain='interrupted'",
                            NULL, NULL, NULL) == SQLITE_OK);
        _exit(23); // Deliberately skip close/rollback, as with removal or power loss.
    }
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK && rows() == 1000);
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK);
    assert(disk.entries[0].info.result.ip == 1000 && disk.entries[999].info.result.ip == 1);
    assert(disk.entries[0].info.dns_expires_at == 1800001000);
    assert(disk.entries[0].info.provider_expires_at == 1800011000);
    assert(disk.entries[999].info.dns_checked && disk.entries[999].info.provider_missing);
    assert(!disk.entries[999].info.result.ptr[0]);
    assert(!disk.entries[999].last_seen_us && !disk.entries[999].dns_expires_us);
    assert(!disk.entries[999].info.result.request_id);
    assert(strcmp(disk.entries[0].info.result.network, "Org: Example\nASN: 1234") == 0);
    fail_read = 1;
    assert(ip_cache_db_load(path, &disk) != SQLITE_OK);
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 1000);

    // Write/full/flush errors preserve the previous snapshot and allow retry.
    strcpy(entries[999].info.result.ptr, "updated.example");
    const int errors[] = {SQLITE_IOERR_WRITE, SQLITE_FULL};
    for (size_t i = 0; i < 2; ++i) {
        fail_write = errors[i];
        assert(ip_cache_db_save(path, &live) != SQLITE_OK);
        assert(ip_cache_db_load(path, &disk) == SQLITE_OK);
        assert(strcmp(disk.entries[0].info.result.ptr, "host-1000.example") == 0);
    }
    fail_sync = 1;
    assert(ip_cache_db_save(path, &live) != SQLITE_OK);
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK);
    assert(strcmp(disk.entries[0].info.result.ptr, "host-1000.example") == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK);
    assert(strcmp(disk.entries[0].info.result.ptr, "updated.example") == 0);
    assert(ip_cache_db_load("missing-directory/cache.sqlite", &disk) != SQLITE_OK);
    assert(rows() == 1000); // Transient open errors do not delete the database.

    disk.count = 3;
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && disk.entries[0].info.result.ip == 1000);
    assert(ip_cache_db_save(path, &disk) == SQLITE_OK && rows() == 3);
    disk.count = 1000;
    assert(ip_cache_db_save(path, &live) == SQLITE_OK && rows() == 1000);

    // Malformed row data is rejected before anything is merged into live RAM.
    exec("UPDATE ip_cache SET domain=CAST(X'610062' AS TEXT) WHERE ip=1");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(!disk.entries[0].used);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    exec("UPDATE ip_cache SET dns_checked=7 WHERE ip=1");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    exec("UPDATE ip_cache SET dns_checked=4294967296 WHERE ip=1");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    exec("UPDATE ip_cache SET dns_expires_at=-1 WHERE ip=1");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    exec("UPDATE ip_cache SET dns_expires_at='invalid' WHERE ip=1");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    exec("UPDATE ip_cache SET provider_expires_at=-1 WHERE ip=1");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    exec("UPDATE ip_cache SET provider_expires_at='invalid' WHERE ip=1");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    exec("UPDATE ip_cache SET domain=printf('%0300d',1) WHERE ip=1");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    exec("CREATE TABLE foreign_data(value TEXT)");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    exec("PRAGMA user_version=99");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    exec("PRAGMA user_version=1"); // Rebuild old formats; no migration path.
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    exec("PRAGMA user_version=2");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    exec("ALTER TABLE ip_cache DROP COLUMN provider_expires_at; PRAGMA user_version=3");
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    FILE *file = fopen(path, "r+b");
    assert(file && _chsize_s(_fileno(file), 600) == 0);
    fclose(file);
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    file = fopen(path, "r+b");
    assert(file && _chsize_s(_fileno(file), 8 * 1024 * 1024 + 1) == 0);
    fclose(file);
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    file = fopen(path, "wb");
    assert(file && fwrite("unrelated user file", 1, 19, file) == 19);
    fclose(file);
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    file = fopen(path, "wb");
    assert(file);
    for (int i = 0; i < 24; ++i) assert(fputc(0, file) != EOF);
    assert(fclose(file) == 0);
    assert(ip_cache_db_load(path, &disk) == SQLITE_OK && rows() == 0);
    assert(ip_cache_db_save(path, &live) == SQLITE_OK);
    puts("PASS: 1000-entry roundtrip, bounded snapshots, negative answers, corrupt/foreign/truncated files, I/O/full/fsync retry");
    return 0;
}
