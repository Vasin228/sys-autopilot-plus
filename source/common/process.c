#include "process.h"
#include "files.h"
#include "log.h"
#include "input.h"
#include "buttons.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __SWITCH__

static inline Result init_pmdmnt(void) {
    smInitialize();
    Result rc = pmdmntInitialize();
    smExit();
    return rc;
}

static inline Result init_pmshell(void) {
    smInitialize();
    Result rc = pmshellInitialize();
    smExit();
    return rc;
}

bool process_get_running(bool *out_running, u64 *out_pid, u64 *out_title_id, char *err, size_t err_sz) {
    if (out_running) *out_running = false;
    if (out_pid) *out_pid = 0;
    if (out_title_id) *out_title_id = 0;

    Result rc = init_pmdmnt();
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "pmdmntInitialize failed: 0x%x", rc);
        return false;
    }

    u64 pid = 0;
    rc = pmdmntGetApplicationProcessId(&pid);
    if (rc == 0x20f) { // MAKERESULT(Module_Pm, 1) - no application running
        pmdmntExit();
        return true;
    }
    if (R_FAILED(rc)) {
        pmdmntExit();
        if (err && err_sz > 0) snprintf(err, err_sz, "pmdmntGetApplicationProcessId failed: 0x%x", rc);
        return false;
    }

    u64 program_id = 0;
    rc = pmdmntGetProgramId(&program_id, pid);
    pmdmntExit();
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "pmdmntGetProgramId failed: 0x%x", rc);
        return false;
    }

    if (out_running) *out_running = true;
    if (out_pid) *out_pid = pid;
    if (out_title_id) *out_title_id = program_id;
    return true;
}

bool process_terminate(char *err, size_t err_sz) {
    bool running = false;
    u64 pid = 0, title_id = 0;
    if (!process_get_running(&running, &pid, &title_id, err, err_sz))
        return false;
    if (!running || pid == 0)
        return true;

    Result rc = init_pmshell();
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "pmshellInitialize failed: 0x%x", rc);
        return false;
    }
    rc = pmshellTerminateProcess(pid);
    pmshellExit();
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "pmshellTerminateProcess failed: 0x%x", rc);
        return false;
    }

    // Wait up to 2 seconds (40 iterations * 50ms) for memory pool to be reaped
    bool reaped = false;
    for (int i = 0; i < 40; i++) {
        svcSleepThread(50000000ULL); // 50ms
        if (R_SUCCEEDED(init_pmdmnt())) {
            u64 check_pid = 0;
            Result check_rc = pmdmntGetApplicationProcessId(&check_pid);
            pmdmntExit();
            if (check_rc == 0x20f) {
                reaped = true;
                break;
            }
        }
    }

    if (reaped) {
        // Allow Horizon OS Application Manager to register termination and present
        // the modal "Software was closed" prompt, then dismiss it cleanly with 'A'.
        svcSleepThread(1500000000ULL); // 1.5s for dialog animation & focus
        input_tap(BTN_A, 200);
    }
    return true;
}

bool process_launch_title(u64 title_id, u64 *out_pid, char *err, size_t err_sz) {
    if (!process_terminate(err, err_sz))
        return false;

    NcmProgramLocation loc = {
        .program_id = title_id,
        .storageID  = NcmStorageId_None,
    };

    u64 pid = 0;
    Result rc = init_pmshell();
    if (R_SUCCEEDED(rc)) {
        rc = pmshellLaunchProgram(0, &loc, &pid);
        pmshellExit();
    }

    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "pmshellLaunchProgram failed: 0x%x", rc);
        return false;
    }

    if (out_pid) *out_pid = pid;
    return true;
}

bool process_launch_nro(const char *path, const char *args, u64 host_title_id, u64 *out_pid, char *err, size_t err_sz) {
    char fspath[768];
    const char *r_err = NULL;
    if (!files_resolve(path, fspath, sizeof(fspath), &r_err)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "invalid NRO path: %s", r_err ? r_err : "resolve failed");
        return false;
    }

    FILE *f = fopen(fspath, "rb");
    if (!f) {
        if (err && err_sz > 0) snprintf(err, err_sz, "NRO file not found: %s", path);
        return false;
    }
    fclose(f);

    mkdir("sdmc:/switch", 0777);
    FILE *cfg = fopen("sdmc:/switch/hbmenu_nextload.cfg", "wb");
    if (!cfg) {
        if (err && err_sz > 0) snprintf(err, err_sz, "failed to create hbmenu_nextload.cfg");
        return false;
    }
    if (args && args[0])
        fprintf(cfg, "%s %s\n", path, args);
    else
        fprintf(cfg, "%s\n", path);
    fclose(cfg);

    u64 tid = host_title_id ? host_title_id : 0x010000000000100DULL;
    return process_launch_title(tid, out_pid, err, err_sz);
}

#else

// --- Host Mock Implementation for Tests ---

static bool g_mock_running = false;
static u64 g_mock_pid = 0;
static u64 g_mock_title_id = 0;

bool process_get_running(bool *out_running, u64 *out_pid, u64 *out_title_id, char *err, size_t err_sz) {
    (void)err; (void)err_sz;
    if (out_running) *out_running = g_mock_running;
    if (out_pid) *out_pid = g_mock_pid;
    if (out_title_id) *out_title_id = g_mock_title_id;
    return true;
}

bool process_terminate(char *err, size_t err_sz) {
    (void)err; (void)err_sz;
    g_mock_running = false;
    g_mock_pid = 0;
    g_mock_title_id = 0;
    return true;
}

bool process_launch_title(u64 title_id, u64 *out_pid, char *err, size_t err_sz) {
    (void)err; (void)err_sz;
    g_mock_running = true;
    g_mock_pid = 0x1234;
    g_mock_title_id = title_id;
    if (out_pid) *out_pid = g_mock_pid;
    return true;
}

bool process_launch_nro(const char *path, const char *args, u64 host_title_id, u64 *out_pid, char *err, size_t err_sz) {
    (void)args;
    char fspath[768];
    const char *r_err = NULL;
    if (!files_resolve(path, fspath, sizeof(fspath), &r_err)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "invalid NRO path: %s", r_err ? r_err : "resolve failed");
        return false;
    }
    u64 tid = host_title_id ? host_title_id : 0x010000000000100DULL;
    return process_launch_title(tid, out_pid, err, err_sz);
}

#endif
