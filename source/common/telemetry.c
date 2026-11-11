#include "telemetry.h"
#include "process.h"
#include "settings.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>

static void read_clocks(TelemetryStats *stats) {
    smInitialize();
    Result rc = clkrstInitialize();
    smExit();
    if (R_SUCCEEDED(rc)) {
        ClkrstSession session;
        if (R_SUCCEEDED(clkrstOpenSession(&session, PcvModuleId_CpuBus, 3))) {
            clkrstGetClockRate(&session, &stats->cpu_hz);
            clkrstCloseSession(&session);
        }
        if (R_SUCCEEDED(clkrstOpenSession(&session, PcvModuleId_GPU, 3))) {
            clkrstGetClockRate(&session, &stats->gpu_hz);
            clkrstCloseSession(&session);
        }
        if (R_SUCCEEDED(clkrstOpenSession(&session, PcvModuleId_EMC, 3))) {
            clkrstGetClockRate(&session, &stats->emc_hz);
            clkrstCloseSession(&session);
        }
        clkrstExit();
    } else {
        // Fallback to pcv on older kernels
        smInitialize();
        rc = pcvInitialize();
        smExit();
        if (R_SUCCEEDED(rc)) {
            pcvGetClockRate(PcvModule_CpuBus, &stats->cpu_hz);
            pcvGetClockRate(PcvModule_GPU, &stats->gpu_hz);
            pcvGetClockRate(PcvModule_EMC, &stats->emc_hz);
            pcvExit();
        }
    }
}

static void read_temperatures(TelemetryStats *stats) {
    stats->soc_temp_c = -128;
    stats->pcb_temp_c = -128;

    smInitialize();
    Result rc = tsInitialize();
    smExit();
    if (R_SUCCEEDED(rc)) {
        s32 t = 0;
        if (R_SUCCEEDED(tsGetTemperature(TsLocation_External, &t)))
            stats->soc_temp_c = (int32_t)t;
        if (R_SUCCEEDED(tsGetTemperature(TsLocation_Internal, &t)))
            stats->pcb_temp_c = (int32_t)t;
        tsExit();
    }
}

static void read_battery_info(TelemetryStats *stats) {
    stats->battery_percent = 0;
    stats->battery_charging = false;
    stats->battery_temp_c = -128;
    stats->battery_volt_mv = 0;

    settings_get_battery(&stats->battery_percent, &stats->battery_charging);

    PsmBatteryChargeInfoFields fields;
    memset(&fields, 0, sizeof(fields));
    if (R_SUCCEEDED(psmGetBatteryChargeInfoFields(&fields))) {
        stats->battery_temp_c = (int32_t)(fields.temperature_celcius / 1000);
        stats->battery_volt_mv = fields.battery_charge_milli_voltage;
    }
}

static void read_timeshift_stats(TelemetryStats *stats) {
    stats->has_game_stats = false;
    stats->game_frame_count = 0;
    stats->game_swap_count = 0;

    FILE *f = fopen("sdmc:/switch/timeshift/timeshift.log", "rb");
    if (!f)
        return;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return;
    }

    long read_len = sz > 4096 ? 4096 : sz;
    if (fseek(f, sz - read_len, SEEK_SET) != 0) {
        fclose(f);
        return;
    }

    char buf[4097];
    size_t n = fread(buf, 1, read_len, f);
    fclose(f);
    if (n == 0)
        return;
    buf[n] = '\0';

    // Search backwards for the latest "Progress: frame #%llu, swap_count=%llu"
    const char *p = strstr(buf, "Progress: frame #");
    const char *last = NULL;
    while (p) {
        last = p;
        p = strstr(p + 1, "Progress: frame #");
    }
    if (last) {
        unsigned long long frame = 0, swap = 0;
        if (sscanf(last, "Progress: frame #%llu, swap_count=%llu", &frame, &swap) == 2) {
            stats->has_game_stats = true;
            stats->game_frame_count = (uint64_t)frame;
            stats->game_swap_count = (uint64_t)swap;
        }
    }
}

bool telemetry_collect(TelemetryStats *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    read_clocks(out);
    read_temperatures(out);
    read_battery_info(out);

    u64 pid = 0, tid = 0;
    bool running = false;
    if (process_get_running(&running, &pid, &tid, NULL, 0)) {
        out->process_running = running;
        out->process_pid = pid;
        out->process_title_id = tid;
    }

    read_timeshift_stats(out);
    return true;
}

#else

// Mock implementation for host builds / tests
bool telemetry_collect(TelemetryStats *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->cpu_hz = 1020000000;
    out->gpu_hz = 768000000;
    out->emc_hz = 1600000000;
    out->soc_temp_c = 42;
    out->pcb_temp_c = 38;
    out->battery_percent = 95;
    out->battery_charging = true;
    out->battery_temp_c = 31;
    out->battery_volt_mv = 4150;
    out->process_running = true;
    out->process_pid = 123;
    out->process_title_id = 0x010000000000100DULL;
    out->has_game_stats = true;
    out->game_frame_count = 13560;
    out->game_swap_count = 13560;
    return true;
}

#endif

int telemetry_format_json(const TelemetryStats *stats, char *buf, size_t bufsz) {
    if (!stats || !buf || bufsz == 0)
        return -1;

    char proc_sub[128];
    if (stats->process_running) {
        snprintf(proc_sub, sizeof(proc_sub),
                 "\"process\":{\"running\":true,\"pid\":%llu,\"titleId\":\"%016llx\"}",
                 (unsigned long long)stats->process_pid,
                 (unsigned long long)stats->process_title_id);
    } else {
        snprintf(proc_sub, sizeof(proc_sub), "\"process\":{\"running\":false}");
    }

    char game_sub[128];
    if (stats->has_game_stats) {
        snprintf(game_sub, sizeof(game_sub),
                 ",\"game\":{\"active\":true,\"frames\":%llu,\"swaps\":%llu}",
                 (unsigned long long)stats->game_frame_count,
                 (unsigned long long)stats->game_swap_count);
    } else {
        game_sub[0] = '\0';
    }

    return snprintf(buf, bufsz,
        "{"
        "\"clocks\":{\"cpuHz\":%u,\"gpuHz\":%u,\"emcHz\":%u},"
        "\"temperatures\":{\"soc\":%d,\"pcb\":%d,\"battery\":%d},"
        "\"battery\":{\"percent\":%u,\"charging\":%s,\"voltageMv\":%u},"
        "%s"
        "%s"
        "}",
        stats->cpu_hz, stats->gpu_hz, stats->emc_hz,
        stats->soc_temp_c, stats->pcb_temp_c, stats->battery_temp_c,
        stats->battery_percent, stats->battery_charging ? "true" : "false",
        stats->battery_volt_mv,
        proc_sub, game_sub);
}
