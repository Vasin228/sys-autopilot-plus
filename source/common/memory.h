#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MemoryBase_Absolute = 0,
    MemoryBase_Main     = 1,
    MemoryBase_Heap     = 2,
    MemoryBase_Alias    = 3,
} MemoryBase;

typedef struct {
    bool running;
    uint64_t pid;
    uint64_t title_id;
    uint64_t rtld_base;
    uint64_t rtld_size;
    uint64_t main_base;
    uint64_t main_size;
    uint64_t heap_base;
    uint64_t heap_size;
    uint64_t alias_base;
    uint64_t alias_size;
    uint32_t module_count;
    char build_id[65]; // 32 bytes hex string + NUL
} MemoryLayout;

typedef struct {
    uint64_t base_address;
    uint64_t size;
    uint32_t type;
    uint32_t perm;
    bool readable;
    bool writable;
    bool executable;
} MemoryRegionInfo;

bool memory_get_layout(MemoryLayout *out, char *err, size_t err_sz);
bool memory_query(uint64_t address, MemoryBase base, MemoryRegionInfo *out, char *err, size_t err_sz);
bool memory_read(uint64_t address, MemoryBase base, uint8_t *buf, size_t size, uint64_t *out_real_addr, char *err, size_t err_sz);
bool memory_write(uint64_t address, MemoryBase base, const uint8_t *buf, size_t size, uint64_t *out_real_addr, char *err, size_t err_sz);
bool memory_resolve_pointer(MemoryBase base, const int64_t *offsets, size_t count, uint64_t *out_addr, char *err, size_t err_sz);
bool memory_search(MemoryBase base, const uint8_t *needle, size_t needle_len, uint64_t *out_addrs, size_t max_results, size_t *out_count, char *err, size_t err_sz);
MemoryBase memory_parse_base(const char *str);
