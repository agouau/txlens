#include "sd_log_retention.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    char path[80];
    char oldest[80];
    int date[5];
    unsigned work;
} prune_context_t;

static const char *TAG = "sd_log";

static int date_part(const char *name, unsigned level, const int date[5])
{
    size_t digits = level == 0 ? 4 : 2;
    if (strlen(name) != digits + (level == 4 ? 4 : 0) ||
        (level == 4 && strcmp(name + digits, ".csv") != 0)) return -1;
    int value = 0;
    for (size_t i = 0; i < digits; ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        value = value * 10 + name[i] - '0';
    }
    const int minimum[] = {1970, 1, 1, 0, 0};
    int maximum[] = {9999, 12, 31, 23, 59};
    if (level == 2) {
        const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        bool leap = date[0] % 4 == 0 && (date[0] % 100 != 0 || date[0] % 400 == 0);
        maximum[2] = days[date[1] - 1] + (date[1] == 2 && leap);
    }
    return value >= minimum[level] && value <= maximum[level] ? value : -1;
}

static bool find_oldest_hour(prune_context_t *ctx, unsigned level)
{
    DIR *dir = opendir(ctx->path);
    if (dir == NULL) return false;
    bool success = true;
    size_t used = strlen(ctx->path);
    while (true) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) success = false;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        // Large cards can have many hour directories; let other tasks run too.
        if (++ctx->work % 64 == 0) vTaskDelay(1);
        int value = date_part(entry->d_name, level, ctx->date);
        bool expected_type = entry->d_type == (level == 4 ? DT_REG : DT_DIR);
        if (value < 0 || !expected_type) continue;
        if (level == 4) {
            strcpy(ctx->oldest, ctx->path);
            break;
        }
        int n = snprintf(ctx->path + used, sizeof(ctx->path) - used, "/%s", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(ctx->path) - used) success = false;
        else if (!ctx->oldest[0] || strcmp(ctx->path, ctx->oldest) < 0) {
            ctx->date[level] = value;
            success = find_oldest_hour(ctx, level + 1);
        }
        ctx->path[used] = '\0';
        if (!success) break;
    }
    if (closedir(dir) != 0) success = false;
    return success;
}

static bool delete_hour(char path[80], size_t root_length)
{
    DIR *dir = opendir(path);
    if (dir == NULL) return false;
    bool success = true;
    size_t used = strlen(path);
    while (true) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) success = false;
            break;
        }
        if (entry->d_type != DT_REG || date_part(entry->d_name, 4, NULL) < 0) continue;
        int n = snprintf(path + used, 80 - used, "/%s", entry->d_name);
        if (n < 0 || (size_t)n >= 80 - used || remove(path) != 0) success = false;
        path[used] = '\0';
        if (!success) break;
    }
    if (closedir(dir) != 0) success = false;
    if (!success) return false;
    ESP_LOGI(TAG, "Removed CSV hour %s", path);
    // Best effort: unknown files or other hours keep their parent directories.
    while (strlen(path) > root_length && rmdir(path) == 0)
        *strrchr(path, '/') = '\0';
    return true;
}

bool sd_log_prune(const char *root, uint64_t min_free_bytes,
                  uint64_t *total_bytes, uint64_t *free_bytes)
{
    *total_bytes = *free_bytes = 0;
    prune_context_t ctx = {0};
    int n = snprintf(ctx.path, sizeof(ctx.path), "%s", root);
    if (n < 0 || (size_t)n >= sizeof(ctx.path)) return false;
    while (true) {
        if (esp_vfs_fat_info(root, total_bytes, free_bytes) != ESP_OK) {
            *total_bytes = *free_bytes = 0;
            return false;
        }
        if (*free_bytes >= min_free_bytes) return true;
        ctx.oldest[0] = '\0';
        if (!find_oldest_hour(&ctx, 0) || !ctx.oldest[0] ||
            !delete_hour(ctx.oldest, strlen(root))) return false;
        vTaskDelay(1);
    }
}
