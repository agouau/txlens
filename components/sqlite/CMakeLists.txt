# Official SQLite amalgamation, pinned and verified for reproducible builds.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(PRIV_REQUIRES esp_timer esp_hw_support)
    return()
endif()
include(FetchContent)
FetchContent_Declare(sqlite_source
    URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032)
FetchContent_MakeAvailable(sqlite_source)
idf_component_register(SRCS "${sqlite_source_SOURCE_DIR}/sqlite3.c" "sd_sqlite_vfs.c"
                       INCLUDE_DIRS "${sqlite_source_SOURCE_DIR}"
                       PRIV_REQUIRES esp_timer esp_hw_support)
# One connection on the SD worker. All temporary storage stays in RAM.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SQLITE_OS_OTHER=1 SQLITE_THREADSAFE=0 SQLITE_TEMP_STORE=3
    SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_WAL SQLITE_OMIT_SHARED_CACHE
    SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_DEFAULT_CACHE_SIZE=-64
    SQLITE_MAX_MMAP_SIZE=0 SQLITE_MAX_LENGTH=1048576 SQLITE_MAX_SQL_LENGTH=16384
    SQLITE_DQS=0)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-unused-variable)
set_source_files_properties("${sqlite_source_SOURCE_DIR}/sqlite3.c" PROPERTIES
                            COMPILE_OPTIONS "-Os;-Wno-unused-value")
