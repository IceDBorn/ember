// DLEvent objects (ctor 0x1421afcc0, dtor 0x1421afd30) get their unnamed events from a pool, so per-frame
// SFX slots stop paying a wineserver round trip for every CreateEventW and CloseHandle.
#include <string.h>
#include "ember.h"

#define CTOR_RA 0x1421afcf2ull // return address of the CreateEventW call in the DLEvent ctor
#define DTOR_RA 0x1421afd4dull // return address of the CloseHandle call in the DLEvent dtor
#define POOL 8192

static HANDLE pool[2][POOL]; // [manual reset]
static int npool[2];
static SRWLOCK lock = SRWLOCK_INIT;
static volatile LONG64 reused, created, parked, closed;
static HANDLE(WINAPI *o_create)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
static BOOL(WINAPI *o_close)(HANDLE);
static BOOL(WINAPI *o_set)(HANDLE);
static BOOL(WINAPI *o_reset)(HANDLE);
// events the ctor made are tagged here, so the dtor parks only those
static HANDLE known[1 << 16];
static uint8_t known_manual[1 << 16];

static int slot(HANDLE h, int add)
{
    uint32_t i = (uint32_t)((uint64_t)h >> 2) & 0xffff;
    for (int n = 0; n < 0x10000; n++, i = (i + 1) & 0xffff) {
        if (known[i] == h)
            return i;
        if (!known[i])
            return add ? (known[i] = h, (int)i) : -1;
    }
    return -1;
}

static HANDLE WINAPI h_create(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name)
{
    if ((uint64_t)__builtin_return_address(0) != CTOR_RA || sa || name)
        return o_create(sa, manual, initial, name);
    int m = !!manual;
    HANDLE h = NULL;
    AcquireSRWLockExclusive(&lock);
    if (npool[m])
        h = pool[m][--npool[m]];
    ReleaseSRWLockExclusive(&lock);
    if (h) {
        if (initial)
            o_set(h);
        InterlockedIncrement64(&reused);
        return h;
    }
    h = o_create(sa, manual, initial, name);
    if (h) {
        AcquireSRWLockExclusive(&lock);
        int i = slot(h, 1);
        if (i >= 0)
            known_manual[i] = m;
        ReleaseSRWLockExclusive(&lock);
        InterlockedIncrement64(&created);
    }
    return h;
}

static BOOL WINAPI h_close(HANDLE h)
{
    if ((uint64_t)__builtin_return_address(0) != DTOR_RA || !h)
        return o_close(h);
    AcquireSRWLockExclusive(&lock);
    int i = slot(h, 0), m = i >= 0 ? known_manual[i] : 0;
    int ok = i >= 0 && npool[m] < POOL;
    ReleaseSRWLockExclusive(&lock);
    if (!ok) {
        InterlockedIncrement64(&closed);
        return o_close(h);
    }
    o_reset(h);
    AcquireSRWLockExclusive(&lock);
    pool[m][npool[m]++] = h;
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement64(&parked);
    return TRUE;
}

static int hook_iat(void)
{
    struct { const char *name; void *hook; void **orig; } fns[] = {
        {"CreateEventW", h_create, (void **)&o_create},
        {"CloseHandle", h_close, (void **)&o_close},
    };
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    o_set = (void *)GetProcAddress(k32, "SetEvent");
    o_reset = (void *)GetProcAddress(k32, "ResetEvent");
    uint8_t *b = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(b + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)b + d->Name, "KERNEL32.dll"))
            continue;
        uint64_t *names = (uint64_t *)(b + d->OriginalFirstThunk), *iat = (uint64_t *)(b + d->FirstThunk);
        for (int i = 0; names[i]; i++) {
            if (names[i] >> 63)
                continue;
            const char *nm = (char *)b + names[i] + 2;
            for (unsigned k = 0; k < sizeof fns / sizeof *fns; k++) {
                if (strcmp(nm, fns[k].name))
                    continue;
                DWORD old;
                VirtualProtect(&iat[i], 8, PAGE_READWRITE, &old);
                *fns[k].orig = (void *)iat[i];
                iat[i] = (uint64_t)fns[k].hook;
                VirtualProtect(&iat[i], 8, old, &old);
                patched++;
            }
        }
    }
    return patched;
}

__attribute__((noreturn)) static DWORD WINAPI stats(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Sleep(60000);
        log_line(LOG_DEBUG, "eventpool reused=%lld created=%lld parked=%lld closed=%lld pool=%d/%d",
                 InterlockedExchange64(&reused, 0), InterlockedExchange64(&created, 0),
                 InterlockedExchange64(&parked, 0), InterlockedExchange64(&closed, 0), npool[0], npool[1]);
    }
}

void eventpool_start(void)
{
    int patched = hook_iat();
    log_line(patched == 2 ? LOG_INFO : LOG_ERROR, "eventpool import hooks %d of 2", patched);
    if (patched)
        CloseHandle(CreateThread(NULL, 0, stats, NULL, 0, NULL));
}
