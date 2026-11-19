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

static bool get_main_module_info(u64 pid, uint64_t *out_rtld_base, uint64_t *out_rtld_size,
                                 uint64_t *out_main_base, uint64_t *out_main_size,
                                 uint32_t *out_count, char build_id[65]) {
    if (R_FAILED(init_ldrdmnt()))
        return false;
    LoaderModuleInfo mods[16] = {0};
    s32 num_mods = 0;
    Result rc = ldrDmntGetProcessModuleInfo(pid, mods, 16, &num_mods);
    ldrDmntExit();
    if (R_FAILED(rc) || num_mods <= 0)
        return false;

    if (out_count) *out_count = (uint32_t)num_mods;
    // Standard Switch apps load rtld at mods[0] and main executable at mods[1].
    // Single-module homebrew loads directly at mods[0].
    const LoaderModuleInfo *main_mod = (num_mods >= 2) ? &mods[1] : &mods[0];
    if (num_mods >= 2) {
        if (out_rtld_base) *out_rtld_base = mods[0].base_address;
        if (out_rtld_size) *out_rtld_size = mods[0].size;
    }
    if (out_main_base) *out_main_base = main_mod->base_address;
    if (out_main_size) *out_main_size = main_mod->size;
    if (build_id) {
        for (int i = 0; i < 32; i++) {
            snprintf(build_id + i * 2, 3, "%02X", main_mod->build_id[i]);
        }
    }
    return true;
}

static bool resolve_base_attached(Handle h, u64 pid, MemoryBase base,
                                  uint64_t *out_base, uint64_t *out_size,
                                  char *err, size_t err_sz) {
    if (base == MemoryBase_Absolute) {
        if (out_base) *out_base = 0;
        if (out_size) *out_size = 0;
        return true;
    }
    uint64_t b = 0, sz = 0;
    if (base == MemoryBase_Main) {
        get_main_module_info(pid, NULL, NULL, &b, &sz, NULL, NULL);
    } else if (base == MemoryBase_Heap) {
        svcGetInfo(&b, InfoType_HeapRegionAddress, h, 0);
        svcGetInfo(&sz, InfoType_HeapRegionSize, h, 0);
    } else if (base == MemoryBase_Alias) {
        svcGetInfo(&b, InfoType_AliasRegionAddress, h, 0);
        svcGetInfo(&sz, InfoType_AliasRegionSize, h, 0);
    }
    if (b == 0) {
        if (err && err_sz > 0) snprintf(err, err_sz, "requested memory base region unavailable");
        return false;
    }
    if (out_base) *out_base = b;
    if (out_size) *out_size = sz;
    return true;
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

    svcGetInfo(&out->heap_base, InfoType_HeapRegionAddress, h, 0);
    svcGetInfo(&out->heap_size, InfoType_HeapRegionSize, h, 0);
    svcGetInfo(&out->alias_base, InfoType_AliasRegionAddress, h, 0);
    svcGetInfo(&out->alias_size, InfoType_AliasRegionSize, h, 0);

    detach_target(h);

    get_main_module_info(pid, &out->rtld_base, &out->rtld_size,
                         &out->main_base, &out->main_size,
                         &out->module_count, out->build_id);
    return true;
}

bool memory_query(uint64_t address, MemoryBase base, MemoryRegionInfo *out, char *err, size_t err_sz) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    Handle h = 0;
    u64 pid = 0;
    Result rc = attach_target(&h, &pid, NULL);
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "no running application (rc=0x%x)", rc);
        return false;
    }

    uint64_t base_addr = 0;
    if (!resolve_base_attached(h, pid, base, &base_addr, NULL, err, err_sz)) {
        detach_target(h);
        return false;
    }

    MemoryInfo meminfo = {0};
    u32 pageinfo = 0;
    rc = svcQueryDebugProcessMemory(&meminfo, &pageinfo, h, base_addr + address);
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

    Handle h = 0;
    u64 pid = 0;
    Result rc = attach_target(&h, &pid, NULL);
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "no running application (rc=0x%x)", rc);
        return false;
    }

    uint64_t base_addr = 0;
    if (!resolve_base_attached(h, pid, base, &base_addr, NULL, err, err_sz)) {
        detach_target(h);
        return false;
    }

    uint64_t target_addr = base_addr + address;
    if (out_real_addr) *out_real_addr = target_addr;

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

    Handle h = 0;
    u64 pid = 0;
    Result rc = attach_target(&h, &pid, NULL);
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "no running application (rc=0x%x)", rc);
        return false;
    }

    uint64_t base_addr = 0;
    if (!resolve_base_attached(h, pid, base, &base_addr, NULL, err, err_sz)) {
        detach_target(h);
        return false;
    }

    uint64_t target_addr = base_addr + address;
    if (out_real_addr) *out_real_addr = target_addr;

    rc = svcWriteDebugProcessMemory(h, buf, target_addr, size);
    detach_target(h);

    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "svcWriteDebugProcessMemory failed (rc=0x%x)", rc);
        return false;
    }
    return true;
}

bool memory_resolve_pointer(MemoryBase base, const int64_t *offsets, size_t count, uint64_t *out_addr, char *err, size_t err_sz) {
    if (!offsets || count == 0 || count > 16 || !out_addr) {
        if (err && err_sz > 0) snprintf(err, err_sz, "invalid pointer chain (1..16 offsets)");
        return false;
    }

    Handle h = 0;
    u64 pid = 0;
    Result rc = attach_target(&h, &pid, NULL);
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "no running application (rc=0x%x)", rc);
        return false;
    }

    uint64_t base_addr = 0;
    if (!resolve_base_attached(h, pid, base, &base_addr, NULL, err, err_sz)) {
        detach_target(h);
        return false;
    }

    uint64_t addr = (uint64_t)((int64_t)base_addr + offsets[0]);
    for (size_t i = 1; i < count; i++) {
        uint64_t ptr = 0;
        rc = svcReadDebugProcessMemory(&ptr, h, addr, sizeof(ptr));
        if (R_FAILED(rc)) {
            detach_target(h);
            if (err && err_sz > 0)
                snprintf(err, err_sz, "pointer read failed at hop %zu (0x%llx, rc=0x%x)",
                         i - 1, (unsigned long long)addr, rc);
            return false;
        }
        if (ptr == 0) {
            detach_target(h);
            if (err && err_sz > 0)
                snprintf(err, err_sz, "null pointer at hop %zu (0x%llx)",
                         i - 1, (unsigned long long)addr);
            return false;
        }
        addr = (uint64_t)((int64_t)ptr + offsets[i]);
    }

    detach_target(h);
    *out_addr = addr;
    return true;
}

bool memory_search(MemoryBase base, const uint8_t *needle, size_t needle_len,
                   uint64_t *out_addrs, size_t max_results, size_t *out_count,
                   char *err, size_t err_sz) {
    if (!needle || needle_len == 0 || needle_len > 64 || !out_addrs || max_results == 0) {
        if (err && err_sz > 0) snprintf(err, err_sz, "invalid search parameters (pattern 1..64 bytes)");
        return false;
    }
    if (base == MemoryBase_Absolute)
        base = MemoryBase_Heap;
    if (out_count) *out_count = 0;

    Handle h = 0;
    u64 pid = 0;
    Result rc = attach_target(&h, &pid, NULL);
    if (R_FAILED(rc)) {
        if (err && err_sz > 0) snprintf(err, err_sz, "no running application (rc=0x%x)", rc);
        return false;
    }

    uint64_t region_start = 0, region_size = 0;
    if (!resolve_base_attached(h, pid, base, &region_start, &region_size, err, err_sz)) {
        detach_target(h);
        return false;
    }

    static uint8_t scan_buf[0x4000 + 64];
    const uint64_t region_end = region_start + region_size;
    const uint64_t max_scan_bytes = 64ULL * 1024 * 1024; // 64 MB cap per call
    uint64_t total_scanned = 0;
    uint64_t since_yield = 0;
    size_t found = 0;

    uint64_t cur = region_start;
    while (cur < region_end && found < max_results && total_scanned < max_scan_bytes) {
        MemoryInfo mi = {0};
        u32 pi = 0;
        if (R_FAILED(svcQueryDebugProcessMemory(&mi, &pi, h, cur)))
            break;
        uint64_t seg_end = mi.addr + mi.size;
        if (seg_end <= cur)
            break;
        if (seg_end > region_end)
            seg_end = region_end;

        if ((mi.perm & Perm_R) != 0 && mi.size >= needle_len) {
            uint64_t pos = cur;
            size_t carry = 0;
            while (pos < seg_end && found < max_results && total_scanned < max_scan_bytes) {
                uint64_t rem = seg_end - pos;
                size_t chunk = rem > 0x4000 ? 0x4000 : (size_t)rem;
                if (R_FAILED(svcReadDebugProcessMemory(scan_buf + carry, h, pos, chunk)))
                    break;
                size_t avail = carry + chunk;
                if (avail >= needle_len) {
                    size_t limit = avail - needle_len + 1;
                    for (size_t i = 0; i < limit && found < max_results; i++) {
                        if (scan_buf[i] == needle[0] &&
                            (needle_len == 1 || memcmp(scan_buf + i, needle, needle_len) == 0)) {
                            out_addrs[found++] = (pos - carry) + i;
                        }
                    }
                }
                if (needle_len > 1 && avail >= needle_len - 1) {
                    carry = needle_len - 1;
                    memmove(scan_buf, scan_buf + avail - carry, carry);
                } else {
                    carry = 0;
                }
                pos += chunk;
                total_scanned += chunk;
                since_yield += chunk;
                if (since_yield >= 0x40000) { // yield every 256 KB to protect Core 3
                    since_yield = 0;
                    svcSleepThread(1000000LL);
                }
            }
        }
        cur = seg_end;
    }

    detach_target(h);
    if (out_count) *out_count = found;
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
    out->rtld_base = 0x70FFFFC000ULL;
    out->rtld_size = 0x4000ULL;
    out->main_base = 0x7100000000ULL;
    out->main_size = 0x1000000ULL;
    out->heap_base = 0x7200000000ULL;
    out->heap_size = 0x2000000ULL;
    out->alias_base = 0x7300000000ULL;
    out->alias_size = 0x3000000ULL;
    out->module_count = 2;
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

bool memory_resolve_pointer(MemoryBase base, const int64_t *offsets, size_t count, uint64_t *out_addr, char *err, size_t err_sz) {
    if (!offsets || count == 0 || count > 16 || !out_addr) {
        if (err && err_sz > 0) snprintf(err, err_sz, "invalid pointer chain");
        return false;
    }
    ensure_mock_ram();
    uint64_t base_addr = 0;
    if (base == MemoryBase_Main) base_addr = 0x7100000000ULL;
    else if (base == MemoryBase_Heap) base_addr = 0x7200000000ULL;
    else if (base == MemoryBase_Alias) base_addr = 0x7300000000ULL;

    uint64_t addr = (uint64_t)((int64_t)base_addr + offsets[0]);
    for (size_t i = 1; i < count; i++) {
        uint64_t ptr = 0;
        size_t idx = (size_t)(addr % sizeof(g_mock_ram));
        for (size_t b = 0; b < 8; b++)
            ((uint8_t *)&ptr)[b] = g_mock_ram[(idx + b) % sizeof(g_mock_ram)];
        if (ptr == 0) {
            if (err && err_sz > 0) snprintf(err, err_sz, "null pointer at hop %zu", i - 1);
            return false;
        }
        addr = (uint64_t)((int64_t)ptr + offsets[i]);
    }
    *out_addr = addr;
    return true;
}

bool memory_search(MemoryBase base, const uint8_t *needle, size_t needle_len,
                   uint64_t *out_addrs, size_t max_results, size_t *out_count,
                   char *err, size_t err_sz) {
    if (!needle || needle_len == 0 || needle_len > 64 || !out_addrs || max_results == 0) {
        if (err && err_sz > 0) snprintf(err, err_sz, "invalid search parameters");
        return false;
    }
    ensure_mock_ram();
    uint64_t base_addr = (base == MemoryBase_Main) ? 0x7100000000ULL : 0x7200000000ULL;
    size_t found = 0;
    for (size_t i = 0; i + needle_len <= sizeof(g_mock_ram) && found < max_results; i++) {
        if (memcmp(g_mock_ram + i, needle, needle_len) == 0) {
            out_addrs[found++] = base_addr + i;
        }
    }
    if (out_count) *out_count = found;
    return true;
}

#endif
