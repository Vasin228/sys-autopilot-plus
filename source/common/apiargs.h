#pragma once

#include "json.h"

#include <stdbool.h>
#include <stdint.h>

// Shared JSON argument extraction for input endpoints (REST bodies and MCP
// tool arguments use identical shapes).

// {"buttons":["A","B"], ...} -> combined mask. *err set on failure.
bool args_get_buttons(const JsonDoc *doc, int obj, uint64_t *out_mask,
                      const char **err);

// Optional {"durationMs": N}; returns fallback when absent.
int args_get_duration(const JsonDoc *doc, int obj, int fallback);

// {"side":"left|right","x":F,"y":F,"durationMs":N} -> parsed stick command.
bool args_get_stick(const JsonDoc *doc, int obj, int *out_side, float *out_x,
                    float *out_y, int *out_duration, const char **err);

// {"x":X,"y":Y,"x2":X2,"y2":Y2,"durationMs":N} -> parsed touch/swipe command.
// out_x2/out_y2 are set to -1 when no swipe end coordinate is provided.
bool args_get_touch(const JsonDoc *doc, int obj, int *out_x, int *out_y,
                    int *out_x2, int *out_y2, int *out_duration,
                    int default_duration, const char **err);
