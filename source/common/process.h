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

// Returns whether an application is currently running, its PID and TitleID.
bool process_get_running(bool *out_running, u64 *out_pid, u64 *out_title_id, char *err, size_t err_sz);

// Terminates the currently active application and waits for memory pool release (up to 2s).
bool process_terminate(char *err, size_t err_sz);

// Launches a title by 64-bit Title ID. If an app is running, terminates it first.
bool process_launch_title(u64 title_id, u64 *out_pid, char *err, size_t err_sz);

// Prepares /switch/hbmenu_nextload.cfg and launches host application to run an NRO.
bool process_launch_nro(const char *path, const char *args, u64 host_title_id, u64 *out_pid, char *err, size_t err_sz);

#ifdef __cplusplus
}
#endif
