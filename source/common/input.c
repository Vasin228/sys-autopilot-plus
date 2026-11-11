#include "input.h"
#include "buttons.h"
#include "log.h"

#include <assert.h>
#include <string.h>
#include <strings.h>

// buttons.h mirrors the libnx bit values so it stays host-testable; make
// sure they can never drift.
static_assert(BTN_A == HidNpadButton_A, "button mask drift");
static_assert(BTN_B == HidNpadButton_B, "button mask drift");
static_assert(BTN_X == HidNpadButton_X, "button mask drift");
static_assert(BTN_Y == HidNpadButton_Y, "button mask drift");
static_assert(BTN_LSTICK == HidNpadButton_StickL, "button mask drift");
static_assert(BTN_RSTICK == HidNpadButton_StickR, "button mask drift");
static_assert(BTN_L == HidNpadButton_L, "button mask drift");
static_assert(BTN_R == HidNpadButton_R, "button mask drift");
static_assert(BTN_ZL == HidNpadButton_ZL, "button mask drift");
static_assert(BTN_ZR == HidNpadButton_ZR, "button mask drift");
static_assert(BTN_PLUS == HidNpadButton_Plus, "button mask drift");
static_assert(BTN_MINUS == HidNpadButton_Minus, "button mask drift");
static_assert(BTN_LEFT == HidNpadButton_Left, "button mask drift");
static_assert(BTN_UP == HidNpadButton_Up, "button mask drift");
static_assert(BTN_RIGHT == HidNpadButton_Right, "button mask drift");
static_assert(BTN_DOWN == HidNpadButton_Down, "button mask drift");
static_assert(BTN_HOME == HiddbgNpadButton_Home, "button mask drift");
static_assert(BTN_CAPTURE == HiddbgNpadButton_Capture, "button mask drift");

static u8 g_workmem[0x1000] __attribute__((aligned(0x1000)));
static HiddbgHdlsSessionId g_session_id;
static HiddbgHdlsHandle g_handle;
static HiddbgHdlsState g_state;
static bool g_workbuf_attached;
static bool g_attached;

// The HDLS work buffer is transfer memory mapped into the hid sysmodule.
// It is attached lazily (first input request) and MUST be released across
// sleep: holding it during a sleep transition breaks hid's own power
// handling and crashes the console (omm aborts with psc error 2165-1001).
static Result ensure_workbuf(void) {
    if (g_workbuf_attached)
        return 0;
    Result rc = hiddbgAttachHdlsWorkBuffer(&g_session_id, g_workmem, sizeof(g_workmem));
    if (R_FAILED(rc)) {
        LOGF("input: hiddbgAttachHdlsWorkBuffer failed rc=0x%x\n", rc);
        return rc;
    }
    g_workbuf_attached = true;
    return 0;
}

void input_suspend(void) {
    hiddbgUnsetTouchScreenAutoPilotState();
    if (g_attached) {
        hiddbgDetachHdlsVirtualDevice(g_handle);
        g_attached = false;
    }
    if (g_workbuf_attached) {
        hiddbgReleaseHdlsWorkBuffer(g_session_id);
        g_workbuf_attached = false;
    }
    memset(&g_state, 0, sizeof(g_state));
    g_state.battery_level = 4;
}

void input_exit(void) {
    input_suspend();
}

Result input_attach(void) {
    Result rc = ensure_workbuf();
    if (R_FAILED(rc))
        return rc;
    if (g_attached)
        return 0;

    HiddbgHdlsDeviceInfo device = {0};
    device.deviceType = HidDeviceType_FullKey3; // Pro Controller
    device.npadInterfaceType = HidNpadInterfaceType_Bluetooth;
    device.singleColorBody = RGBA8_MAXALPHA(60, 60, 60);
    device.singleColorButtons = RGBA8_MAXALPHA(230, 230, 230);
    device.colorLeftGrip = RGBA8_MAXALPHA(60, 60, 230);
    device.colorRightGrip = RGBA8_MAXALPHA(230, 60, 60);

    rc = hiddbgAttachHdlsVirtualDevice(&g_handle, &device);
    if (R_FAILED(rc)) {
        LOGF("input: hiddbgAttachHdlsVirtualDevice failed rc=0x%x\n", rc);
        return rc;
    }
    g_attached = true;

    // Push an initial neutral state so the controller registers.
    memset(&g_state, 0, sizeof(g_state));
    g_state.battery_level = 4;
    g_state.flags = 1; // IsPowered
    rc = hiddbgSetHdlsState(g_handle, &g_state);
    if (R_FAILED(rc)) {
        LOGF("input: initial set_state failed rc=0x%x\n", rc);
        g_attached = false;
        hiddbgDetachHdlsVirtualDevice(g_handle);
    }
    return rc;
}

Result input_detach(void) {
    if (!g_attached)
        return 0;
    Result rc = hiddbgDetachHdlsVirtualDevice(g_handle);
    g_attached = false;
    return rc;
}

bool input_is_attached(void) {
    return g_attached;
}

static Result apply_state(void) {
    Result rc = hiddbgSetHdlsState(g_handle, &g_state);
    if (R_FAILED(rc)) {
        LOGF("input: apply_state failed rc=0x%x, retrying with fresh device\n", rc);
        g_attached = false;
        hiddbgDetachHdlsVirtualDevice(g_handle);
        HiddbgHdlsState desired = g_state;
        rc = input_attach();
        if (R_SUCCEEDED(rc)) {
            g_state = desired;
            g_state.battery_level = 4;
            g_state.flags = 1; // IsPowered
            rc = hiddbgSetHdlsState(g_handle, &g_state);
        }
    }
    return rc;
}

static int clamp_duration(int ms) {
    if (ms <= 0)
        return INPUT_DEFAULT_TAP_MS;
    if (ms > INPUT_MAX_DURATION_MS)
        return INPUT_MAX_DURATION_MS;
    return ms;
}

Result input_handshake(void) {
    return input_tap(BTN_L | BTN_R, 300);
}

Result input_tap(u64 mask, int duration_ms) {
    Result rc = input_attach();
    if (R_FAILED(rc))
        return rc;

    duration_ms = clamp_duration(duration_ms);

    g_state.buttons |= mask;
    rc = apply_state();
    if (R_FAILED(rc))
        return rc;

    svcSleepThread((s64)duration_ms * 1000000LL);

    g_state.buttons &= ~mask;
    return apply_state();
}

Result input_hold(u64 mask) {
    Result rc = input_attach();
    if (R_FAILED(rc))
        return rc;
    g_state.buttons |= mask;
    return apply_state();
}

Result input_release(u64 mask) {
    Result rc = input_attach();
    if (R_FAILED(rc))
        return rc;
    g_state.buttons &= ~mask;
    return apply_state();
}

Result input_clear(void) {
    hiddbgUnsetTouchScreenAutoPilotState();
    if (!g_attached)
        return 0;
    g_state.buttons = 0;
    memset(&g_state.analog_stick_l, 0, sizeof(g_state.analog_stick_l));
    memset(&g_state.analog_stick_r, 0, sizeof(g_state.analog_stick_r));
    return apply_state();
}

static s32 stick_value(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (s32)(v * JOYSTICK_MAX);
}

Result input_stick(int side, float x, float y, int duration_ms) {
    Result rc = input_attach();
    if (R_FAILED(rc))
        return rc;

    HidAnalogStickState *stick = (side == 0) ? &g_state.analog_stick_l
                                             : &g_state.analog_stick_r;
    stick->x = stick_value(x);
    stick->y = stick_value(y);
    rc = apply_state();
    if (R_FAILED(rc))
        return rc;

    if (duration_ms > 0) {
        if (duration_ms > INPUT_MAX_DURATION_MS)
            duration_ms = INPUT_MAX_DURATION_MS;
        svcSleepThread((s64)duration_ms * 1000000LL);
        stick->x = 0;
        stick->y = 0;
        rc = apply_state();
    }
    return rc;
}

static u32 clamp_coord(int v, int max_val) {
    if (v < 0) return 0;
    if (v >= max_val) return (u32)(max_val - 1);
    return (u32)v;
}

Result input_touch(int x, int y, int x2, int y2, int duration_ms) {
    u32 sx = clamp_coord(x, 1280);
    u32 sy = clamp_coord(y, 720);
    bool is_swipe = (x2 >= 0 && y2 >= 0 && (x2 != x || y2 != y));
    u32 ex = is_swipe ? clamp_coord(x2, 1280) : sx;
    u32 ey = is_swipe ? clamp_coord(y2, 720) : sy;
    duration_ms = clamp_duration(duration_ms);

    int steps = 1;
    if (is_swipe) {
        steps = duration_ms / 16;
        if (steps < 2) steps = 2;
        if (steps > 120) steps = 120;
    }
    s64 step_ns = ((s64)duration_ms * 1000000LL) / steps;

    HidTouchState st = {0};
    st.finger_id = 0;
    st.diameter_x = 30;
    st.diameter_y = 30;

    for (int i = 0; i < steps; i++) {
        st.delta_time = (i == 0) ? (u64)step_ns : 0;
        st.x = (u32)((s32)sx + ((s32)ex - (s32)sx) * i / steps);
        st.y = (u32)((s32)sy + ((s32)ey - (s32)sy) * i / steps);
        st.attributes = 0;
        Result rc = hiddbgSetTouchScreenAutoPilotState(&st, 1);
        if (R_FAILED(rc)) {
            LOGF("input: touch step %d failed rc=0x%x\n", i, rc);
            hiddbgUnsetTouchScreenAutoPilotState();
            return rc;
        }
        svcSleepThread(step_ns);
    }

    if (is_swipe) {
        st.delta_time = 0;
        st.x = ex;
        st.y = ey;
        st.attributes = 0;
        hiddbgSetTouchScreenAutoPilotState(&st, 1);
        svcSleepThread(step_ns);
    }

    hiddbgSetTouchScreenAutoPilotState(NULL, 0);
    svcSleepThread(20000000ULL);
    return hiddbgUnsetTouchScreenAutoPilotState();
}
