"""Exercise real SQLite and the SD VFS in a VS x64 Developer Command Prompt.

Run idf.py build first (downloads the pinned SQLite amalgamation), or provide
--sqlite-source pointing to that extracted archive.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sqlite-source", type=Path)
parser.add_argument("--fatfs-seek", action="store_true",
                    help="Emulate FATFS lseek extending writable files past EOF")
args = parser.parse_args()
source = args.sqlite_source or root / "build/_deps/sqlite_source-src"
if not args.sqlite_source and not (source / "sqlite3.c").exists():
    cache = (root / "build/CMakeCache.txt").read_text(encoding="utf-8")
    match = re.search(r"^FETCHCONTENT_SOURCE_DIR_SQLITE_SOURCE:[^=]+=(.+)$", cache, re.M)
    if match:
        source = Path(match[1])
if not (source / "sqlite3.c").exists():
    parser.error("SQLite source not found; run idf.py build or use --sqlite-source")
source = source.resolve()

with tempfile.TemporaryDirectory(prefix="ip-cache-db-") as directory:
    work = Path(directory)
    (work / "freertos").mkdir()
    (work / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
    (work / "esp_log.h").write_text(
        '#pragma once\n#include <stdio.h>\n'
        '#define ESP_LOGW(tag, ...) do { printf("%s: ", tag); printf(__VA_ARGS__); putchar(10); } while (0)\n')
    (work / "esp_random.h").write_text(
        "#pragma once\n#include <stdlib.h>\n"
        "static void esp_fill_random(void *out, size_t n) {\n"
        " unsigned char *p=out; while(n--) *p++=(unsigned char)rand(); }\n")
    (work / "freertos/FreeRTOS.h").write_text("#pragma once\n#define pdMS_TO_TICKS(ms) (ms)\n")
    (work / "freertos/task.h").write_text("#pragma once\nstatic void vTaskDelay(int ms) { (void)ms; }\n")
    vfs = root / "components/sqlite/sd_sqlite_vfs.c"
    if args.fatfs_seek:
        # ESP-IDF's FATFS lseek calls f_lseek: on a writable descriptor it
        # extends the file immediately, unlike Windows/POSIX file positioning.
        (work / "fatfs_seek.h").write_text('''
#include <io.h>
static long test_fatfs_lseek(int fd, long offset, int origin) {
    struct _stat info;
    if (_fstat(fd, &info) != 0) return -1;
    long pos = _lseek(fd, offset, origin);
    if (pos > info.st_size && _chsize_s(fd, pos) != 0) {
        if (_lseek(fd, info.st_size, SEEK_SET) < 0) return -1;
    }
    return pos;
}
''')
        vfs_text = vfs.read_text(encoding="utf-8").replace(
            "#define lseek _lseek", '#include "fatfs_seek.h"\n#define lseek test_fatfs_lseek')
        vfs = work / "sd_sqlite_vfs.c"
        vfs.write_text(vfs_text, encoding="utf-8")
    defines = ["SQLITE_OS_OTHER=1", "SQLITE_THREADSAFE=0", "SQLITE_TEMP_STORE=3",
               "SQLITE_OMIT_LOAD_EXTENSION", "SQLITE_OMIT_WAL", "SQLITE_OMIT_SHARED_CACHE",
               "SQLITE_DEFAULT_MEMSTATUS=0", "SQLITE_DEFAULT_CACHE_SIZE=-64",
               "SQLITE_MAX_MMAP_SIZE=0", "SQLITE_MAX_LENGTH=1048576",
               "SQLITE_MAX_SQL_LENGTH=16384", "SQLITE_DQS=0", "_CRT_SECURE_NO_WARNINGS"]
    common = ["cl", "/nologo", "/std:c11", "/O1", *[f"/D{x}" for x in defines],
              f"/I{source}", f"/I{work}", f"/I{root / 'main'}"]
    subprocess.run([*common, "/w", "/c", str(source / "sqlite3.c"),
                    f"/Fo{work / 'sqlite.obj'}"], cwd=work, check=True)
    exe = work / "test.exe"
    subprocess.run([*common, "/W4", "/WX", "/wd4996", str(root / "tests/test_ip_cache_db.c"),
                    str(root / "main/ip_cache_db.c"), str(root / "main/ip_cache.c"),
                    str(vfs), str(work / "sqlite.obj"),
                    f"/Fo{work}\\", f"/Fe{exe}"], cwd=work, check=True)
    subprocess.run([str(exe)], cwd=work, check=True)
    crash = subprocess.run([str(exe), "crash"], cwd=work)
    assert crash.returncode == 23, crash.returncode
    subprocess.run([str(exe), "recover"], cwd=work, check=True)
