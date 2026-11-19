#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t cpu_hz;
    uint32_t gpu_hz;
    uint32_t emc_hz;
    int32_t  soc_temp_c;
    int32_t  pcb_temp_c;
    int32_t  fan_percent;
    uint32_t battery_percent;
    bool     battery_charging;
    int32_t  battery_temp_c;
    uint32_t battery_volt_mv;
    uint64_t sd_free_bytes;
    uint64_t sd_total_bytes;
    bool     process_running;
    uint64_t process_pid;
    uint64_t process_title_id;
} TelemetryStats;

// Gathers telemetry from system services and active game logs.
bool telemetry_collect(TelemetryStats *out);

// Formats telemetry as a compact JSON string into buf.
int telemetry_format_json(const TelemetryStats *stats, char *buf, size_t bufsz);
