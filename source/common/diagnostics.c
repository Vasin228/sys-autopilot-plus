#include "diagnostics.h"
#include "files.h"
#include "json.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

bool diagnostics_get_last_crash(u64 filter_title_id, bool clear,
                                char *out_log, size_t out_log_sz,
                                char *out_filename, size_t out_filename_sz,
                                bool *out_found, char *err, size_t err_sz) {
    if (out_found) *out_found = false;
    if (out_log && out_log_sz > 0) out_log[0] = '\0';
    if (out_filename && out_filename_sz > 0) out_filename[0] = '\0';

    char dir_path[768];
    snprintf(dir_path, sizeof(dir_path), "%s/atmosphere/crash_reports", FILES_ROOT);

    DIR *dir = opendir(dir_path);
    if (!dir) {
        // Directory may not exist if no crashes have occurred yet
        if (out_found) *out_found = false;
        return true;
    }

    char filter_hex[32] = {0};
    if (filter_title_id != 0) {
        snprintf(filter_hex, sizeof(filter_hex), "%016llx", (unsigned long long)filter_title_id);
    }

    char latest_name[256] = {0};
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        size_t nlen = strlen(ent->d_name);
        if (nlen < 20 || nlen >= sizeof(latest_name))
            continue;

        // Must end with .log
        if (strcasecmp(ent->d_name + nlen - 4, ".log") != 0)
            continue;

        // Optional title_id filter
        if (filter_hex[0] != '\0') {
            char name_lower[256];
            for (size_t i = 0; i <= nlen; i++) {
                char c = ent->d_name[i];
                name_lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            if (!strstr(name_lower, filter_hex))
                continue;
        }

        // Lexicographical compare because filename starts with timestamp YYYYMMDDHHMMSS
        if (strcmp(ent->d_name, latest_name) > 0) {
            strncpy(latest_name, ent->d_name, sizeof(latest_name) - 1);
            latest_name[sizeof(latest_name) - 1] = '\0';
        }
    }
    closedir(dir);

    if (latest_name[0] == '\0') {
        if (out_found) *out_found = false;
        return true;
    }

    if (out_filename && out_filename_sz > 0) {
        strncpy(out_filename, latest_name, out_filename_sz - 1);
        out_filename[out_filename_sz - 1] = '\0';
    }

    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, latest_name);

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        if (err && err_sz > 0) snprintf(err, err_sz, "failed to open crash log: %s", latest_name);
        return false;
    }

    size_t to_read = 4096;
    if (out_log_sz > 0 && out_log_sz - 1 < to_read)
        to_read = out_log_sz - 1;

    size_t got = 0;
    if (out_log && to_read > 0) {
        got = fread(out_log, 1, to_read, f);
        out_log[got] = '\0';
        utf8_trim_partial(out_log);
    }
    fclose(f);

    if (clear) {
        remove(full_path);
        // Also remove matching .bin if present
        size_t plen = strlen(full_path);
        if (plen > 4 && strcmp(full_path + plen - 4, ".log") == 0) {
            memcpy(full_path + plen - 4, ".bin", 4);
            remove(full_path);
        }
    }

    if (out_found) *out_found = true;
    return true;
}
