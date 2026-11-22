#pragma once

#include <stdint.h>

// Single-threaded server shared scratch buffer (512 KB).
// Reused across mutually exclusive operations (JPEG capture, file I/O,
// NSP/XCI install chunks, RAM search, and MCP request streaming) to minimize
// System Pool BSS footprint without reducing any buffer limits.
#define SCRATCH_BUF_SIZE 0x80000

extern uint8_t g_scratch_buf[SCRATCH_BUF_SIZE];
