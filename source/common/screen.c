#include "screen.h"
#include "log.h"
#include "scratch.h"

#include <string.h>

#define CAPTURE_TIMEOUT_NS 100000000ULL // 100ms

Result screen_capture_jpeg(ViLayerStack stack, const u8 **out_buf, u64 *out_size) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) {
        LOGF("screen: smInitialize failed rc=0x%x\n", rc);
        return rc;
    }
    rc = capsscInitialize();
    smExit();
    if (R_FAILED(rc)) {
        LOGF("screen: capsscInitialize failed rc=0x%x\n", rc);
        return rc;
    }

    u64 size = 0;
    rc = capsscCaptureJpegScreenShot(&size, g_scratch_buf, SCRATCH_BUF_SIZE,
                                     stack, CAPTURE_TIMEOUT_NS);
    capsscExit();

    if (R_FAILED(rc)) {
        LOGF("screen: capture failed rc=0x%x\n", rc);
        return rc;
    }
    if (size > SCRATCH_BUF_SIZE)
        size = SCRATCH_BUF_SIZE;
    *out_buf = g_scratch_buf;
    *out_size = size;
    return 0;
}

bool screen_parse_stack(const char *name, ViLayerStack *out) {
    if (strcasecmp(name, "screenshot") == 0)     *out = ViLayerStack_Screenshot;
    else if (strcasecmp(name, "default") == 0)   *out = ViLayerStack_Default;
    else if (strcasecmp(name, "lcd") == 0)       *out = ViLayerStack_Lcd;
    else if (strcasecmp(name, "recording") == 0) *out = ViLayerStack_Recording;
    else if (strcasecmp(name, "lastframe") == 0) *out = ViLayerStack_LastFrame;
    else return false;
    return true;
}
