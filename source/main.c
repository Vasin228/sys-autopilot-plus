#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "common/config.h"
#include "common/device_info.h"
#include "common/input.h"
#include "common/install.h"
#include "common/log.h"
#include "common/netif.h"
#include "common/oauth.h"
#include "common/power.h"
#include "common/server.h"
#include "common/settings.h"

// Inner heap: socket transfer memory + stdio buffers + dir listing JSON +
// headroom for the title installer (ncm IPC, mounting the cnmt NCA). The large
// fixed buffers (JPEG, I/O, HDLS workmem, install chunk) are static bss.
#define INNER_HEAP_SIZE 0x100000

#ifdef __cplusplus
extern "C" {
#endif

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

// Sysmodules use time:s (the npdm grants it); used for the OAuth
// "# issued <date>" stamps and the read-only get_datetime tool.
u32 __nx_time_service_type = TimeServiceType_System;

// Internal libnx helper that wires newlib's time() to the time service.
void __libnx_init_time(void);

void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

// Keep socket buffers small to fit the sysmodule memory budget.
static const SocketInitConfig kSocketConfig = {
    .tcp_tx_buf_size     = 0x8000,
    .tcp_rx_buf_size     = 0x10000,
    .tcp_tx_buf_max_size = 0,       // fixed size
    .tcp_rx_buf_max_size = 0,       // fixed size
    .udp_tx_buf_size     = 0x2400,
    .udp_rx_buf_size     = 0xA500,
    .sb_efficiency       = 2,
    // Sessions in concurrent use: the TCP listener, one accepted client, and
    // the persistent mDNS/DNS-SD UDP socket. 3 is the minimum; use 4 for a
    // little headroom.
    .num_bsd_sessions    = 4,
    // Sysmodules in pool_partition 2 need bsd:s (system socket service).
    // BsdServiceType_Auto tries bsd:s first, then falls back to bsd:u.
    .bsd_service_type    = BsdServiceType_Auto,
};

void __appInit(void)
{
    Result rc;

    // 1. SM: In early boot (Atmosphere boot2), SM might still be initializing.
    // Retry up to 30 times (15 seconds) before giving up.
    rc = MAKERESULT(Module_Libnx, LibnxError_InitFail_SM);
    for (int i = 0; i < 30; i++) {
        rc = smInitialize();
        if (R_SUCCEEDED(rc))
            break;
        svcSleepThread(500000000ULL);
    }
    if (R_FAILED(rc))
        return;

    // 2. Horizon Firmware Version: must be retrieved before other services
    // (such as hid and caps) query hosversionGet().
    for (int i = 0; i < 30; i++) {
        if (R_SUCCEEDED(setsysInitialize())) {
            SetSysFirmwareVersion fw;
            if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
                setsysExit();
                break;
            }
            setsysExit();
        }
        svcSleepThread(500000000ULL);
    }

    // 3. Filesystem and SDMC: at boot, fsp-srv takes several seconds to detect and mount SD card.
    for (int i = 0; i < 30; i++) {
        rc = fsInitialize();
        if (R_SUCCEEDED(rc))
            break;
        svcSleepThread(500000000ULL);
    }

    for (int i = 0; i < 60; i++) {
        rc = fsdevMountSdmc();
        if (R_SUCCEEDED(rc))
            break;
        svcSleepThread(500000000ULL);
    }

    // 4. HID Debug (controller emulation)
    for (int i = 0; i < 30; i++) {
        rc = hiddbgInitialize();
        if (R_SUCCEEDED(rc))
            break;
        svcSleepThread(500000000ULL);
    }

    // 5. Optional: wall-clock time for OAuth token issuance stamps.
    rc = timeInitialize();
    if (R_SUCCEEDED(rc))
        __libnx_init_time();

    // 7. BSD socket service: bsd:u is started later in the boot sequence.
    // Retry up to 60 times (30 seconds) until the network stack is up.
    for (int i = 0; i < 60; i++) {
        rc = socketInitialize(&kSocketConfig);
        if (R_SUCCEEDED(rc))
            break;
        svcSleepThread(500000000ULL);
    }

    // 8. PSC power module: MUST be registered while SM is still open (before
    // smExit). Calling this later (after smExit) silently fails on Horizon
    // because pscmInitialize internally calls smGetService.
    // Non-fatal: if PSC is unavailable (rare on >=FW18), the server continues
    // without sleep/wake quiescing; the console will reboot on wake instead of
    // resuming, which matches the old behaviour and is better than a boot crash.
    power_init();

    // 9. SPSM for agent-requested sleep/restart/power-off.
    power_spsm_init();

    // 10. Device facts (model/firmware/Atmosphère) for mDNS TXT records.
    device_info_init();

    // 11. Network Interface Manager (nifm) for real LAN IP.
    for (int i = 0; i < 30; i++) {
        if (netif_init())
            break;
        svcSleepThread(500000000ULL);
    }

    // 12. System settings (lbl/audctl/psm)
    settings_init();

    smExit();
}

void __appExit(void)
{
    install_exit();
    settings_exit();
    netif_exit();
    power_spsm_exit();
    power_exit();
    timeExit();
    socketExit();
    hiddbgExit();
    fsdevUnmountAll();
    fsExit();
}

#ifdef __cplusplus
}
#endif

int main(int argc, char* argv[])
{
    Config cfg;
    config_load(&cfg);

    // OAuth state (config reference + persisted token list).
    oauth_init(&cfg);

    // Blocks forever (NULL idle callback).
    server_run(&cfg, NULL);

    input_exit();
    return 0;
}
