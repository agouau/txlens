"""Exercise real CSV cleanup on disk from a Visual Studio developer shell."""
from pathlib import Path
import subprocess
import tempfile


# Match ESP-IDF's dirent API, backed by real Windows directory operations.
DIRENT = r'''
#pragma once
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DT_REG 8
#define DT_DIR 4
#define DT_LNK 10
struct dirent { int d_type; char d_name[MAX_PATH]; };
typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    struct dirent entry;
    int first;
} DIR;
static DIR *opendir(const char *path) {
    DIR *dir = calloc(1, sizeof(*dir));
    if (!dir) { errno = ENOMEM; return NULL; }
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s/*", path);
    dir->handle = FindFirstFileA(pattern, &dir->data);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir); errno = EIO; return NULL;
    }
    dir->first = 1;
    return dir;
}
static struct dirent *readdir(DIR *dir) {
    if (!dir->first && !FindNextFileA(dir->handle, &dir->data)) {
        errno = GetLastError() == ERROR_NO_MORE_FILES ? 0 : EIO;
        return NULL;
    }
    dir->first = 0;
    strcpy(dir->entry.d_name, dir->data.cFileName);
    DWORD attrs = dir->data.dwFileAttributes;
    dir->entry.d_type = attrs & FILE_ATTRIBUTE_REPARSE_POINT ? DT_LNK :
                       attrs & FILE_ATTRIBUTE_DIRECTORY ? DT_DIR : DT_REG;
    return &dir->entry;
}
static int closedir(DIR *dir) {
    BOOL ok = FindClose(dir->handle);
    free(dir);
    if (!ok) { errno = EIO; return -1; }
    return 0;
}
'''

PLATFORM = r'''
#pragma once
#include <assert.h>
#include <direct.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define rmdir _rmdir
#define ESP_OK 0
#define ESP_LOGI(...) ((void)TAG)
extern unsigned yields;
extern uint64_t fake_free, release_per_file;
extern unsigned removed_count;
static void vTaskDelay(int ticks) { assert(ticks == 1); ++yields; }
static int host_remove(const char *path) {
    int rc = remove(path);
    if (rc == 0) { ++removed_count; fake_free += release_per_file; }
    return rc;
}
#define remove host_remove
int esp_vfs_fat_info(const char *root, uint64_t *total, uint64_t *available);
'''

TEST = r'''
#include "platform.h"
#include "sd_log_retention.h"
unsigned yields;
uint64_t fake_free, release_per_file;
unsigned removed_count, info_calls, fail_info_at;
int esp_vfs_fat_info(const char *root, uint64_t *total, uint64_t *available) {
    (void)root;
    if (++info_calls == fail_info_at) return 1;
    *total = UINT64_C(64) * 1073741824;
    *available = fake_free;
    return ESP_OK;
}
int main(int argc, char **argv) {
    assert(argc == 6);
    uint64_t minimum = _strtoui64(argv[2], NULL, 10);
    fake_free = _strtoui64(argv[3], NULL, 10);
    release_per_file = _strtoui64(argv[4], NULL, 10);
    fail_info_at = (unsigned)atoi(argv[5]);
    uint64_t total, available;
    bool ok = sd_log_prune(argv[1], minimum, &total, &available);
    bool query_failed = info_calls == fail_info_at;
    assert(total == (query_failed ? 0 : UINT64_C(64) * 1073741824));
    assert(available == (query_failed ? 0 : fake_free));
    printf("cleanup=%d removed=%u queries=%u yields=%u\n", ok, removed_count, info_calls, yields);
    return ok ? 0 : 1;
}
'''


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="sd-space-") as directory:
        work = Path(directory)
        (work / "dirent.h").write_text(DIRENT)
        (work / "platform.h").write_text(PLATFORM)
        (work / "unistd.h").write_text('#include "platform.h"\n')
        (work / "esp_log.h").write_text('#include "platform.h"\n')
        (work / "esp_vfs_fat.h").write_text('#include "platform.h"\n')
        (work / "freertos").mkdir()
        for name in ("FreeRTOS.h", "task.h"):
            (work / "freertos" / name).write_text('#include "platform.h"\n')
        source = work / "test.c"
        source.write_text(TEST)
        exe = work / "test.exe"
        subprocess.run([
            "cl", "/nologo", "/std:c11", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
            f"/I{work}", f"/I{root / 'main'}", str(source),
            str(root / "main/sd_log_retention.c"), f"/Fo{work}\\", f"/Fe{exe}",
        ], cwd=work, check=True)
        card = work / "sd"
        card.mkdir()

        def put(name):
            path = card / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"keep original bytes\r\n")
            return path

        minimum = 100 * 1024 * 1024

        def cleanup(available, released=1, fail_query=0, success=True):
            result = subprocess.run([
                str(exe), str(card), str(minimum), str(available), str(released), str(fail_query),
            ], cwd=work, capture_output=True, text=True)
            assert result.returncode == (0 if success else 1), result.stdout + result.stderr
            return result.stdout

        keep_names = [
            "ip_cache.sqlite", "ip_cache.sqlite-journal", "notes.csv",
            "2025/01/01/00/notes.txt", "2025/01/01/00/60.csv", "2025/01/01/00/1.csv",
            "2025/01/01/00/01.csv.bak", "2025/01/01/00/02.CSV",
            "2025/02/29/00/00.csv", "2025/13/01/00/00.csv", "2025/01/01/24/00.csv",
            "2025/01/32/00/00.csv", "2025/1/01/00/00.csv", "other/2025/01/01/00/00.csv",
            "2023/01/01/00/01.csv/keep.txt",  # A directory bearing a CSV name.
        ]
        kept = {put(name): b"keep original bytes\r\n" for name in keep_names}
        # Create newest first: deletion follows path dates, not creation order.
        newest = put("2027/01/01/00/00.csv")
        middle = put("2026/11/07/12/00.csv")
        oldest = [put("2024/02/29/23/10.csv"), put("2024/02/29/23/59.csv")]
        all_csvs = [newest, middle, *oldest]
        for available in (minimum + 1, minimum):
            assert "removed=0" in cleanup(available)
            assert all(path.exists() for path in all_csvs)  # Even years-old logs stay.
        # One CSV would recover the threshold, but finish the entire oldest hour.
        assert "removed=2" in cleanup(minimum - 1)
        assert all(not path.exists() for path in oldest)
        assert not (card / "2024").exists()
        assert newest.exists() and middle.exists()
        assert all(path.read_bytes() == content for path, content in kept.items())
        assert "removed=0" in cleanup(minimum)
        # Delete as many whole hours as necessary, including across a year boundary.
        assert "removed=2" in cleanup(minimum - 3, released=2)
        assert not middle.exists() and not newest.exists()
        cleanup(minimum - 1, success=False)  # No recognized CSV left; no endless retry loop.
        assert all(path.read_bytes() == content for path, content in kept.items())

        # Nonempty old directories stay; failed deletes stop before newer hours.
        blocked = put("2020/01/01/00/00.csv")
        later = put("2020/01/01/01/00.csv")
        blocked.chmod(0o444)
        try:
            cleanup(minimum - 1, success=False)
            assert blocked.exists() and later.exists()
        finally:
            blocked.chmod(0o666)
        cleanup(minimum - 1)
        assert not blocked.exists() and later.exists()
        # A failed space query cannot trigger speculative deletion.
        cleanup(minimum - 1, fail_query=1, success=False)
        assert later.exists()
        after_error = put("2020/01/01/02/00.csv")
        cleanup(minimum - 2, fail_query=2, success=False)
        assert not later.exists() and after_error.exists()
        cleanup(minimum - 1)
        assert not after_error.exists()
        # Deleting an hour keeps unrelated files in that same directory.
        mixed = put("2025/01/01/00/05.csv")
        cleanup(minimum - 1)
        assert not mixed.exists() and (card / "2025/01/01/00/notes.txt").exists()
        # Backlog yields to other tasks and is consumed one full hour at a time.
        backlog = [put(f"2019/12/31/23/{minute:02d}.csv") for minute in range(60)]
        output = cleanup(minimum - 1)
        assert "removed=60" in output and "yields=0" not in output
        assert all(not path.exists() for path in backlog)
        assert not (card / "2019").exists()
        assert all(path.read_bytes() == content for path, content in kept.items())
        # Remove each empty level, stopping at the first parent with other data.
        siblings = [put(name) for name in (
            "2030/01/01/00/00.csv", "2030/01/01/01/00.csv",
            "2030/01/02/00/00.csv", "2030/02/01/00/00.csv",
        )]
        cleanup(minimum - 1)
        assert not siblings[0].parent.exists() and siblings[1].exists()
        assert (card / "2030/01/01").is_dir()
        cleanup(minimum - 1)
        assert not (card / "2030/01/01").exists() and siblings[2].exists()
        assert (card / "2030/01").is_dir()
        cleanup(minimum - 1)
        assert not (card / "2030/01").exists() and siblings[3].exists()
        cleanup(minimum - 1)
        assert not (card / "2030").exists() and card.is_dir()
        assert all(path.read_bytes() == content for path, content in kept.items())
        print("PASS: free-space threshold, oldest whole hours, empty hour/day/month/year removal, unrelated files, errors and retry")


if __name__ == "__main__":
    main()
