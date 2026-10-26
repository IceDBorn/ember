// Sets LoadBalancerParam's SFX reduction levels in the loaded regulation. A level is the load balancer step
// from which the reduction applies, so 0 keeps it on all the time. The param is found by scanning the heap
// for its type name once the regulation is loaded, so no game addresses are needed.
#include <string.h>
#include "ember.h"

#define PARAM_TYPE "LOAD_BALANCER_PARAM_ST"
#define ROW_SIZE 80
#define CHUNK (1 << 20)
#define MAX_ROWS 4096

// LOAD_BALANCER_PARAM_ST fields and their ember.ini keys under [sfx]
static const struct { const char *key; int offset; } fields[] = {
    {"particle_half", 34},
    {"reflection", 35},
    {"water_interaction", 36},
    {"distortion", 38},
    {"scale_render_distance", 41},
};
#define NFIELDS (int)(sizeof fields / sizeof *fields)
static int levels[NFIELDS];
static uint8_t chunk[CHUNK + 64];

static bool peek(uintptr_t at, void *to, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (void *)at, to, n, &got) && got == n;
}

// Param file header: row count at +0x0a, type name inline at +0x0c or at the offset stored at +0x10,
// row table at +0x40 with 24-byte entries {u32 id, u32 pad, u64 data offset, u64 name offset}.
static int patch_param(uintptr_t hdr, uintptr_t type_at)
{
    uint16_t rows;
    if (!peek(hdr + 0x0a, &rows, 2) || !rows || rows > MAX_ROWS)
        return 0;
    uint64_t prev = 0;
    for (int i = 0; i < rows; i++) {
        uint64_t data;
        if (!peek(hdr + 0x40 + 24 * i + 8, &data, 8) || hdr + data + ROW_SIZE > type_at)
            return 0;
        if (i && data - prev != ROW_SIZE)
            return 0;
        prev = data;
    }
    int changed = 0;
    for (int i = 0; i < rows; i++) {
        uint64_t data;
        uint32_t id;
        peek(hdr + 0x40 + 24 * i, &id, 4);
        peek(hdr + 0x40 + 24 * i + 8, &data, 8);
        for (int f = 0; f < NFIELDS; f++) {
            uint8_t v = (uint8_t)levels[f], old;
            uintptr_t at = hdr + data + fields[f].offset;
            if (!peek(at, &old, 1) || old == v)
                continue;
            SIZE_T put;
            if (WriteProcessMemory(GetCurrentProcess(), (void *)at, &v, 1, &put))
                changed++;
            log_line(LOG_DEBUG, "sfx row %u %s %u -> %u", id, fields[f].key, old, v);
        }
    }
    log_line(LOG_INFO, "sfx LoadBalancerParam at %p, %d rows, %d values changed", (void *)hdr, rows, changed);
    return 1;
}

// A hit is the type name; its header sits 0x0c before it, or up to 64 KB before it with the offset at +0x10.
static int try_hit(uintptr_t at)
{
    if (patch_param(at - 0x0c, at))
        return 1;
    for (uintptr_t hdr = (at & ~(uintptr_t)7) - 0x40; hdr + 0x10000 > at; hdr -= 8) {
        uint64_t off;
        if (peek(hdr + 0x10, &off, 8) && off == at - hdr)
            return patch_param(hdr, at);
    }
    return 0;
}

static int scan(void)
{
    int found = 0;
    const size_t tlen = sizeof PARAM_TYPE;
    MEMORY_BASIC_INFORMATION mi;
    for (uintptr_t p = 0x10000; VirtualQuery((void *)p, &mi, sizeof mi); p = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE || mi.Protect != PAGE_READWRITE)
            continue;
        uintptr_t base = (uintptr_t)mi.BaseAddress, end = base + mi.RegionSize;
        for (uintptr_t at = base; at < end; at += CHUNK) {
            size_t n = end - at < CHUNK + tlen ? end - at : CHUNK + tlen;
            if (!peek(at, chunk, n))
                continue;
            for (uint8_t *q = chunk; (q = memchr(q, 'L', n - (q - chunk))) && q + tlen <= chunk + n; q++)
                if (!memcmp(q, PARAM_TYPE, tlen) && q - chunk < CHUNK)
                    found += try_hit(at + (q - chunk));
        }
    }
    return found;
}

static DWORD WINAPI run(LPVOID arg)
{
    (void)arg;
    int wait = cfg("sfx", "scan_seconds", 120);
    for (int t = 0; t < wait; t += 2) {
        Sleep(2000);
        int found = scan();
        if (found) {
            log_line(LOG_INFO, "sfx patched %d copies after %d s", found, t + 2);
            return 0;
        }
    }
    log_line(LOG_ERROR, "sfx LoadBalancerParam not found within %d s", wait);
    return 0;
}

void sfx_start(void)
{
    for (int f = 0; f < NFIELDS; f++)
        levels[f] = cfg("sfx", fields[f].key, 0);
    CloseHandle(CreateThread(NULL, 0, run, NULL, 0, NULL));
}
