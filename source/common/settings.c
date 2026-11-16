#include "settings.h"

#include <stdio.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>

#include "log.h"

// Volume is read/written against the speaker target; this is the value the
// system Quick Settings slider controls for the active output.
#define VOLUME_TARGET AudioTarget_Speaker

static inline Result init_setsys(void) {
    smInitialize();
    Result rc = setsysInitialize();
    smExit();
    return rc;
}

static inline Result init_lbl(void) {
    smInitialize();
    Result rc = lblInitialize();
    smExit();
    return rc;
}

static inline Result init_audctl(void) {
    smInitialize();
    Result rc = audctlInitialize();
    smExit();
    return rc;
}

static inline Result init_psm(void) {
    smInitialize();
    Result rc = psmInitialize();
    smExit();
    return rc;
}

void settings_init(void) {
    // Intentionally empty: all Horizon services are now opened lazily on demand
    // to avoid holding persistent sessions in the background.
}

void settings_exit(void) {
    // Intentionally empty: all lazy sessions are closed immediately after each request.
}

// --- theme --------------------------------------------------------------------

bool settings_get_theme(bool *out_dark) {
    if (R_FAILED(init_setsys()))
        return false;
    ColorSetId id = ColorSetId_Light;
    Result rc = setsysGetColorSetId(&id);
    setsysExit();
    if (R_FAILED(rc))
        return false;
    *out_dark = (id == ColorSetId_Dark);
    return true;
}

bool settings_set_theme(bool dark) {
    if (R_FAILED(init_setsys()))
        return false;
    Result rc = setsysSetColorSetId(dark ? ColorSetId_Dark : ColorSetId_Light);
    setsysExit();
    return R_SUCCEEDED(rc);
}

// --- nickname -----------------------------------------------------------------

bool settings_get_nickname(char *out, size_t outsz) {
    if (R_FAILED(init_setsys()))
        return false;
    SetSysDeviceNickName nn = {0};
    Result rc = setsysGetDeviceNickname(&nn);
    setsysExit();
    if (R_FAILED(rc))
        return false;
    snprintf(out, outsz, "%s", nn.nickname);
    return true;
}

bool settings_set_nickname(const char *name) {
    if (R_FAILED(init_setsys()))
        return false;
    SetSysDeviceNickName nn = {0};
    snprintf(nn.nickname, sizeof(nn.nickname), "%s", name);
    Result rc = setsysSetDeviceNickname(&nn);
    setsysExit();
    return R_SUCCEEDED(rc);
}

// --- brightness ---------------------------------------------------------------

bool settings_get_brightness(float *out) {
    if (R_FAILED(init_lbl()))
        return false;
    Result rc = lblGetCurrentBrightnessSetting(out);
    lblExit();
    return R_SUCCEEDED(rc);
}

bool settings_set_brightness(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    if (R_FAILED(init_lbl()))
        return false;
    Result rc = lblSetCurrentBrightnessSetting(value);
    if (R_SUCCEEDED(rc)) {
        lblApplyCurrentBrightnessSettingToBacklight();
    }
    lblExit();
    return R_SUCCEEDED(rc);
}

// --- volume -------------------------------------------------------------------

bool settings_get_volume(float *out) {
    if (R_FAILED(init_audctl()))
        return false;
    s32 vol = 0, vmin = 0, vmax = 0;
    Result rc1 = audctlGetTargetVolume(&vol, VOLUME_TARGET);
    Result rc2 = audctlGetTargetVolumeMin(&vmin);
    Result rc3 = audctlGetTargetVolumeMax(&vmax);
    audctlExit();
    if (R_FAILED(rc1) || R_FAILED(rc2) || R_FAILED(rc3))
        return false;
    if (vmax <= vmin) {
        *out = 0.0f;
        return true;
    }
    *out = (float)(vol - vmin) / (float)(vmax - vmin);
    return true;
}

bool settings_set_volume(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    if (R_FAILED(init_audctl()))
        return false;
    s32 vmin = 0, vmax = 0;
    Result rc1 = audctlGetTargetVolumeMin(&vmin);
    Result rc2 = audctlGetTargetVolumeMax(&vmax);
    if (R_FAILED(rc1) || R_FAILED(rc2) || vmax <= vmin) {
        audctlExit();
        return false;
    }
    s32 vol = vmin + (s32)(value * (float)(vmax - vmin) + 0.5f);
    Result rc = audctlSetTargetVolume(VOLUME_TARGET, vol);
    audctlExit();
    return R_SUCCEEDED(rc);
}

// --- wireless / airplane mode -------------------------------------------------

bool settings_disable_wireless(void) {
    // nifm is initialized by netif_init(); this call goes through that session.
    return R_SUCCEEDED(nifmSetWirelessCommunicationEnabled(false));
}

// --- battery ------------------------------------------------------------------

bool settings_get_battery(uint32_t *out_percent, bool *out_charging) {
    if (R_FAILED(init_psm()))
        return false;
    u32 pct = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    Result rc = psmGetBatteryChargePercentage(&pct);
    if (R_SUCCEEDED(rc)) {
        psmGetChargerType(&charger); // best-effort
    }
    psmExit();
    if (R_FAILED(rc))
        return false;
    *out_percent = pct;
    *out_charging = (charger != PsmChargerType_Unconnected);
    return true;
}

// --- automatic clock sync -----------------------------------------------------

bool settings_get_auto_time(bool *out_enabled) {
    if (R_FAILED(init_setsys()))
        return false;
    bool en = false;
    Result rc = setsysIsUserSystemClockAutomaticCorrectionEnabled(&en);
    setsysExit();
    if (R_FAILED(rc))
        return false;
    *out_enabled = en;
    return true;
}

bool settings_set_auto_time(bool enabled) {
    if (R_FAILED(init_setsys()))
        return false;
    Result rc = setsysSetUserSystemClockAutomaticCorrectionEnabled(enabled);
    setsysExit();
    return R_SUCCEEDED(rc);
}

// --- date / time / timezone ---------------------------------------------------

bool settings_get_datetime(DateTime *out) {
    smInitialize();
    Result rc = timeInitialize();
    smExit();
    if (R_FAILED(rc))
        return false;

    u64 ts = 0;
    rc = timeGetCurrentTime(TimeType_UserSystemClock, &ts);
    if (R_FAILED(rc)) {
        timeExit();
        return false;
    }

    TimeCalendarTime cal = {0};
    TimeCalendarAdditionalInfo info = {0};
    if (R_FAILED(timeToCalendarTimeWithMyRule(ts, &cal, &info))) {
        timeExit();
        return false;
    }

    out->year   = cal.year;
    out->month  = cal.month;
    out->day    = cal.day;
    out->hour   = cal.hour;
    out->minute = cal.minute;
    out->second = cal.second;

    TimeLocationName loc = {0};
    if (R_SUCCEEDED(timeGetDeviceLocationName(&loc)))
        snprintf(out->timezone, sizeof(out->timezone), "%s", loc.name);
    else
        out->timezone[0] = '\0';

    timeExit();
    return true;
}

bool settings_set_datetime(const DateTime *dt) {
    smInitialize();
    Result rc = timeInitialize();
    smExit();
    if (R_FAILED(rc))
        return false;

    // Convert the wall-clock fields (interpreted in the device's current
    // timezone) to a POSIX timestamp.
    TimeCalendarTime cal = {0};
    cal.year   = (u16)dt->year;
    cal.month  = (u8)dt->month;
    cal.day    = (u8)dt->day;
    cal.hour   = (u8)dt->hour;
    cal.minute = (u8)dt->minute;
    cal.second = (u8)dt->second;

    u64 posix = 0;
    s32 count = 0;
    if (R_FAILED(timeToPosixTimeWithMyRule(&cal, &posix, 1, &count)) || count < 1) {
        timeExit();
        return false;
    }

    // Write the NETWORK system clock: that's the one a sysmodule is permitted
    // to set (the user/local clocks reject it with rc 0x274), and the displayed
    // time follows it. Verified on hardware fixing a badly-skewed clock.
    rc = timeSetCurrentTime(TimeType_NetworkSystemClock, posix);
    timeExit();
    if (R_FAILED(rc)) {
        LOGF("settings: SetCurrentTime(Network) rc=0x%x\n", rc);
        return false;
    }
    return true;
}

#else // host / test build: deterministic placeholders

void settings_init(void) {}
void settings_exit(void) {}

bool settings_get_theme(bool *out_dark) { *out_dark = true; return true; }
bool settings_set_theme(bool dark) { (void)dark; return true; }

bool settings_get_nickname(char *out, size_t outsz) {
    snprintf(out, outsz, "Test Switch");
    return true;
}
bool settings_set_nickname(const char *name) { (void)name; return true; }

bool settings_get_brightness(float *out) { *out = 0.5f; return true; }
bool settings_set_brightness(float value) { (void)value; return true; }

bool settings_get_volume(float *out) { *out = 0.5f; return true; }
bool settings_set_volume(float value) { (void)value; return true; }

bool settings_disable_wireless(void) { return true; }

bool settings_get_battery(uint32_t *out_percent, bool *out_charging) {
    *out_percent = 75;
    *out_charging = true;
    return true;
}

bool settings_get_auto_time(bool *out_enabled) { *out_enabled = true; return true; }
bool settings_set_auto_time(bool enabled) { (void)enabled; return true; }

bool settings_get_datetime(DateTime *out) {
    out->year = 2026; out->month = 6; out->day = 19;
    out->hour = 12; out->minute = 0; out->second = 0;
    snprintf(out->timezone, sizeof(out->timezone), "America/New_York");
    return true;
}
bool settings_set_datetime(const DateTime *dt) { (void)dt; return true; }

#endif
