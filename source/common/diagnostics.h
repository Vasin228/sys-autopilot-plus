#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __SWITCH__
#include <switch.h>
#else
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Scans sdmc:/atmosphere/crash_reports for the latest .log crash report.
// Memory safe: uses O(1) comparison on directory stream, reads up to 4KB with UTF-8 sanitize.
bool diagnostics_get_last_crash(u64 filter_title_id, bool clear,
                                char *out_log, size_t out_log_sz,
                                char *out_filename, size_t out_filename_sz,
                                bool *out_found, char *err, size_t err_sz);

#ifdef __cplusplus
}
#endif
