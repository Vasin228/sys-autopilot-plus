#include "memory.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

MemoryBase memory_parse_base(const char *str) {
    if (!str || !str[0]) return MemoryBase_Absolute;
    if (strcasecmp(str, "main") == 0) return MemoryBase_Main;
    if (strcasecmp(str, "heap") == 0) return MemoryBase_Heap;
    if (strcasecmp(str, "alias") == 0) return MemoryBase_Alias;
    return MemoryBase_Absolute;
}

#ifdef __SWITCH__

#include <switch.h>

static inline Result init_pmdmnt(void) {
    smInitialize();
    Result rc = pmdmntInitialize();
    smExit();
    return rc;
}

static inline Result init_ldrdmnt(void) {
    smInitialize();
    Result rc = ldrDmntInitialize();
    smExit();
    return rc;
}

static Result attach_target(Handle *out_handle, u64 *out_pid, u64 *out_title_id) {
    Result rc = init_pmdmnt();
    if (R_FAILED(rc))
        return rc;

    u64 pid = 0;
    rc = pmdmntGetApplicationProcessId(&pid);
    if (rc == 0x20f || R_FAILED(rc) || pid == 0) {
        pmdmntExit();
        return rc ? rc : MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    u64 tid = 0;
    pmdmntGetProgramId(&tid, pid);
    pmdmntExit();

    Handle h = 0;
    rc = svcDebugActiveProcess(&h, pid);
    if (R_FAILED(rc))
        return rc;

    *out_handle = h;
    if (out_pid) *out_pid = pid;
    if (out_title_id) *out_title_id = tid;
    return 0;
}

static void detach_target(Handle h) {
    if (h != 0) {
        svcCloseHandle(h);
    }
}

bool memory_get_layout(MemoryLayout *out, char *err, size_t err_sz) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    Handle h = 0;
    u64 pid = 0, tid = 0;
    Result rc = attach_target(&h, &pid, &tid);
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "no running application (rc=0x%x)", rc);
        return false;
    }

    out->running = true;
    out->pid = pid;
    out->title_id = tid;

    // Get Heap region
    svcGetInfo(&out->heap_base, InfoType_HeapRegionAddress, h, 0);
    svcGetInfo(&out->heap_size, InfoType_HeapRegionSize, h, 0);

    // Get Alias region
    svcGetInfo(&out->alias_base, InfoType_AliasRegionAddress, h, 0);
    svcGetInfo(&out->alias_size, InfoType_AliasRegionSize, h, 0);

    detach_target(h);

    // Get main NSO module info via ldr:dmnt
    if (R_SUCCEEDED(init_ldrdmnt())) {
        LoaderModuleInfo mods[4] = {0};
        s32 num_mods = 0;
        if (R_SUCCEEDED(ldrDmntGetProcessModuleInfo(pid, mods, 4, &num_mods)) && num_mods > 0) {
            out->main_base = mods[0].base_address;
            out->main_size = mods[0].size;
            for (int i = 0; i < 32; i++) {
                snprintf(out->build_id + i * 2, 3, "%02X", mods[0].build_id[i]);
            }
        }
        ldrDmntExit();
    }
    return true;
}

bool memory_query(uint64_t address, MemoryBase base, MemoryRegionInfo *out, char *err, size_t err_sz) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    uint64_t target_addr = address;
    if (base != MemoryBase_Absolute) {
        MemoryLayout layout;
        if (!memory_get_layout(&layout, err, err_sz))
            return false;
        if (base == MemoryBase_Main) target_addr = layout.main_base + address;
        else if (base == MemoryBase_Heap) target_addr = layout.heap_base + address;
        else if (base == MemoryBase_Alias) target_addr = layout.alias_base + address;
    }

    Handle h = 0;
    Result rc = attach_target(&h, NULL, NULL);
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "failed to attach (rc=0x%x)", rc);
        return false;
    }

    MemoryInfo meminfo = {0};
    u32 pageinfo = 0;
    rc = svcQueryDebugProcessMemory(&meminfo, &pageinfo, h, target_addr);
    detach_target(h);

    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "svcQueryDebugProcessMemory failed (rc=0x%x)", rc);
        return false;
    }

    out->base_address = meminfo.addr;
    out->size = meminfo.size;
    out->type = meminfo.type;
    out->perm = meminfo.perm;
    out->readable = (meminfo.perm & Perm_R) != 0;
    out->writable = (meminfo.perm & Perm_W) != 0;
    out->executable = (meminfo.perm & Perm_X) != 0;
    return true;
}

bool memory_read(uint64_t address, MemoryBase base, uint8_t *buf, size_t size, uint64_t *out_real_addr, char *err, size_t err_sz) {
    if (!buf || size == 0 || size > 4096) {
        if (err && err_sz > 0) snprintf(err, err_sz, "invalid size (1..4096)");
        return false;
    }

    uint64_t target_addr = address;
    if (base != MemoryBase_Absolute) {
        MemoryLayout layout;
        if (!memory_get_layout(&layout, err, err_sz))
            return false;
        if (base == MemoryBase_Main) {
            target_addr = layout.main_base + address;
        } else if (base == MemoryBase_Heap) {
            target_addr = layout.heap_base + address;
        } else if (base == MemoryBase_Alias) {
            target_addr = layout.alias_base + address;
        }
    }

    if (out_real_addr) *out_real_addr = target_addr;

    Handle h = 0;
    Result rc = attach_target(&h, NULL, NULL);
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "failed to attach to target process (rc=0x%x)", rc);
        return false;
    }

    rc = svcReadDebugProcessMemory(buf, h, target_addr, size);
    detach_target(h);

    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "svcReadDebugProcessMemory failed (rc=0x%x)", rc);
        return false;
    }
    return true;
}

bool memory_write(uint64_t address, MemoryBase base, const uint8_t *buf, size_t size, uint64_t *out_real_addr, char *err, size_t err_sz) {
    if (!buf || size == 0 || size > 4096) {
        if (err && err_sz > 0) snprintf(err, err_sz, "invalid size (1..4096)");
        return false;
    }

    uint64_t target_addr = address;
    if (base != MemoryBase_Absolute) {
        MemoryLayout layout;
        if (!memory_get_layout(&layout, err, err_sz))
            return false;
        if (base == MemoryBase_Main) {
            target_addr = layout.main_base + address;
        } else if (base == MemoryBase_Heap) {
            target_addr = layout.heap_base + address;
        } else if (base == MemoryBase_Alias) {
            target_addr = layout.alias_base + address;
        }
    }

    if (out_real_addr) *out_real_addr = target_addr;

    Handle h = 0;
    Result rc = attach_target(&h, NULL, NULL);
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "failed to attach to target process (rc=0x%x)", rc);
        return false;
    }

    rc = svcWriteDebugProcessMemory(h, buf, target_addr, size);
    detach_target(h);

    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "svcWriteDebugProcessMemory failed (rc=0x%x)", rc);
        return false;
    }
    return true;
}

#else

// --- Host mock implementation for deterministic unit tests ---

static uint8_t g_mock_ram[0x10000];
static bool g_mock_ram_init = false;

static void ensure_mock_ram(void) {
    if (!g_mock_ram_init) {
        for (size_t i = 0; i < sizeof(g_mock_ram); i++) {
            g_mock_ram[i] = (uint8_t)(i & 0xFF);
        }
        g_mock_ram_init = true;
    }
}

bool memory_get_layout(MemoryLayout *out, char *err, size_t err_sz) {
    (void)err; (void)err_sz;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->running = true;
    out->pid = 0x1234;
    out->title_id = 0x01006F8002326000ULL;
    out->main_base = 0x7100000000ULL;
    out->main_size = 0x1000000ULL;
    out->heap_base = 0x7200000000ULL;
    out->heap_size = 0x2000000ULL;
    out->alias_base = 0x7300000000ULL;
    out->alias_size = 0x3000000ULL;
    snprintf(out->build_id, sizeof(out->build_id), "DEADBEEF0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C");
    return true;
}

bool memory_query(uint64_t address, MemoryBase base, MemoryRegionInfo *out, char *err, size_t err_sz) {
    (void)err; (void)err_sz;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    uint64_t offset = address;
    if (base == MemoryBase_Main) offset += 0x7100000000ULL;
    else if (base == MemoryBase_Heap) offset += 0x7200000000ULL;
    else if (base == MemoryBase_Alias) offset += 0x7300000000ULL;
    out->base_address = offset & ~0xFFFULL;
    out->size = 0x10000;
    out->type = 3;
    out->perm = 3;
    out->readable = true;
    out->writable = true;
    out->executable = false;
    return true;
}

bool memory_read(uint64_t address, MemoryBase base, uint8_t *buf, size_t size, uint64_t *out_real_addr, char *err, size_t err_sz) {
    (void)err; (void)err_sz;
    if (!buf || size == 0 || size > 4096) return false;
    ensure_mock_ram();
    uint64_t offset = address;
    if (base == MemoryBase_Main) offset += 0x7100000000ULL;
    else if (base == MemoryBase_Heap) offset += 0x7200000000ULL;
    else if (base == MemoryBase_Alias) offset += 0x7300000000ULL;
    if (out_real_addr) *out_real_addr = offset;

    size_t local_idx = (size_t)(offset % sizeof(g_mock_ram));
    for (size_t i = 0; i < size; i++) {
        buf[i] = g_mock_ram[(local_idx + i) % sizeof(g_mock_ram)];
    }
    return true;
}

bool memory_write(uint64_t address, MemoryBase base, const uint8_t *buf, size_t size, uint64_t *out_real_addr, char *err, size_t err_sz) {
    (void)err; (void)err_sz;
    if (!buf || size == 0 || size > 4096) return false;
    ensure_mock_ram();
    uint64_t offset = address;
    if (base == MemoryBase_Main) offset += 0x7100000000ULL;
    else if (base == MemoryBase_Heap) offset += 0x7200000000ULL;
    else if (base == MemoryBase_Alias) offset += 0x7300000000ULL;
    if (out_real_addr) *out_real_addr = offset;

    size_t local_idx = (size_t)(offset % sizeof(g_mock_ram));
    for (size_t i = 0; i < size; i++) {
        g_mock_ram[(local_idx + i) % sizeof(g_mock_ram)] = buf[i];
    }
    return true;
}

#endif
