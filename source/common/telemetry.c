#include "telemetry.h"
#include "process.h"
#include "settings.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#ifdef __SWITCH__
#include <switch.h>

#define FAN_DEVICE_CODE 0x3d000001u

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

    if (R_FAILED(smInitialize()))
        return;
    Result rc = tsInitialize();
    smExit();
    if (R_FAILED(rc)) {
        LOGF("telemetry: tsInitialize failed rc=0x%x\n", rc);
        return;
    }

    if (hosversionAtLeast(10, 0, 0)) {
        TsSession sess;
        float temp_f = 0.0f;
        if (R_SUCCEEDED(tsOpenSession(&sess, TsDeviceCode_LocationExternal))) {
            if (R_SUCCEEDED(tsSessionGetTemperature(&sess, &temp_f)))
                stats->soc_temp_c = (int32_t)(temp_f + 0.5f);
            tsSessionClose(&sess);
        }
        if (R_SUCCEEDED(tsOpenSession(&sess, TsDeviceCode_LocationInternal))) {
            if (R_SUCCEEDED(tsSessionGetTemperature(&sess, &temp_f)))
                stats->pcb_temp_c = (int32_t)(temp_f + 0.5f);
            tsSessionClose(&sess);
        }
    } else {
        s32 t = 0;
        if (R_SUCCEEDED(tsGetTemperature(TsLocation_External, &t)))
            stats->soc_temp_c = (int32_t)t;
        if (R_SUCCEEDED(tsGetTemperature(TsLocation_Internal, &t)))
            stats->pcb_temp_c = (int32_t)t;
    }
    tsExit();
}

static void read_fan_info(TelemetryStats *stats) {
    stats->fan_percent = -1;

    if (R_FAILED(smInitialize()))
        return;
    Result rc = fanInitialize();
    smExit();
    if (R_FAILED(rc))
        return;

    FanController fc;
    if (R_SUCCEEDED(fanOpenController(&fc, FAN_DEVICE_CODE))) {
        float level = 0.0f;
        if (R_SUCCEEDED(fanControllerGetRotationSpeedLevel(&fc, &level))) {
            if (level < 0.0f) level = 0.0f;
            if (level > 1.0f) level = 1.0f;
            stats->fan_percent = (int32_t)(level * 100.0f + 0.5f);
        }
        fanControllerClose(&fc);
    }
    fanExit();
}

static void read_battery_info(TelemetryStats *stats) {
    stats->battery_percent = 0;
    stats->battery_charging = false;
    stats->battery_temp_c = -128;
    stats->battery_volt_mv = 0;

    if (R_FAILED(smInitialize()))
        return;
    Result rc = psmInitialize();
    smExit();
    if (R_FAILED(rc)) {
        LOGF("telemetry: psmInitialize failed rc=0x%x\n", rc);
        return;
    }

    u32 pct = 0;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct)))
        stats->battery_percent = pct;

    PsmChargerType charger = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetChargerType(&charger)))
        stats->battery_charging = (charger != PsmChargerType_Unconnected);

    PsmBatteryChargeInfoFields fields;
    memset(&fields, 0, sizeof(fields));
    rc = psmGetBatteryChargeInfoFields(&fields);
    if (R_SUCCEEDED(rc)) {
        stats->battery_temp_c = (int32_t)(fields.temperature_celcius / 1000);
        stats->battery_volt_mv = fields.battery_charge_milli_voltage;
    } else {
        LOGF("telemetry: psmGetBatteryChargeInfoFields failed rc=0x%x\n", rc);
    }
    psmExit();
}

static void read_sd_space(TelemetryStats *stats) {
    stats->sd_free_bytes = 0;
    stats->sd_total_bytes = 0;
    struct statvfs st;
    if (statvfs("sdmc:/", &st) == 0) {
        stats->sd_free_bytes = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
        stats->sd_total_bytes = (uint64_t)st.f_blocks * (uint64_t)st.f_frsize;
    }
}

bool telemetry_collect(TelemetryStats *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    read_clocks(out);
    read_temperatures(out);
    read_fan_info(out);
    read_battery_info(out);
    read_sd_space(out);

    u64 pid = 0, tid = 0;
    bool running = false;
    if (process_get_running(&running, &pid, &tid, NULL, 0)) {
        out->process_running = running;
        out->process_pid = pid;
        out->process_title_id = tid;
    }

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
    out->fan_percent = 35;
    out->battery_percent = 95;
    out->battery_charging = true;
    out->battery_temp_c = 31;
    out->battery_volt_mv = 4150;
    out->sd_free_bytes = 64ULL * 1024 * 1024 * 1024;
    out->sd_total_bytes = 256ULL * 1024 * 1024 * 1024;
    out->process_running = true;
    out->process_pid = 123;
    out->process_title_id = 0x010000000000100DULL;
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

    return snprintf(buf, bufsz,
        "{"
        "\"clocks\":{\"cpuHz\":%u,\"gpuHz\":%u,\"emcHz\":%u},"
        "\"temperatures\":{\"soc\":%d,\"pcb\":%d,\"battery\":%d},"
        "\"fan\":{\"percent\":%d},"
        "\"battery\":{\"percent\":%u,\"charging\":%s,\"voltageMv\":%u},"
        "\"sdCard\":{\"freeBytes\":%llu,\"totalBytes\":%llu},"
        "%s"
        "}",
        stats->cpu_hz, stats->gpu_hz, stats->emc_hz,
        stats->soc_temp_c, stats->pcb_temp_c, stats->battery_temp_c,
        stats->fan_percent,
        stats->battery_percent, stats->battery_charging ? "true" : "false",
        stats->battery_volt_mv,
        (unsigned long long)stats->sd_free_bytes,
        (unsigned long long)stats->sd_total_bytes,
        proc_sub);
}
