/* From github.com/oskarwirga/nightreign-intel-affinity-patch (Apache-2.0), commit bff5ca1.
   Modified for ember: a configurable thread list, settings from ember.ini, an optional trace log and affinity_attach() in place of DllMain. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "affinity_fix.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define NRAT_VERSION 2
#define NRAT_RING_CAPACITY 4096u
#define NRAT_DESCRIPTION_CHARS 96u
#define NRAT_CPU_SET_CAPACITY 32u
#define NRAT_PATCH_CAPACITY 64u
#define NRAT_WRITER_BATCH 32u

typedef struct NratCpuSetSnapshot {
    ULONG id;
    WORD group;
    BYTE logical_processor_index;
    BYTE core_index;
    BYTE last_level_cache_index;
    BYTE numa_node_index;
    BYTE efficiency_class;
    BYTE flags;
} NratCpuSetSnapshot;

typedef struct NratRecord {
    uint32_t api;
    LARGE_INTEGER qpc;
    DWORD caller_tid;
    DWORD target_tid;
    DWORD target_pid;
    ULONG_PTR arg0;
    ULONG_PTR arg1;
    ULONG_PTR arg2;
    ULONG_PTR result0;
    ULONG_PTR result1;
    DWORD last_error;
    const void *caller;
    uint32_t cpu_set_count;
    WCHAR description[NRAT_DESCRIPTION_CHARS];
    NratCpuSetSnapshot cpu_sets[NRAT_CPU_SET_CAPACITY];
} NratRecord;

typedef struct NratLogger {
    CRITICAL_SECTION lock;
    HANDLE event;
    HANDLE stop_event;
    HANDLE writer_thread;
    HANDLE file;
    volatile LONG ready;
    volatile LONG enqueued;
    volatile LONG dropped;
    uint32_t head;
    uint32_t tail;
    NratRecord ring[NRAT_RING_CAPACITY];
} NratLogger;

typedef struct NratPatch {
    void **slot;
    void *original;
    void *hook;
    uint32_t api;
} NratPatch;

#ifndef NRAT_NO_DLLMAIN
static HMODULE g_self = NULL;
#endif
static volatile LONG g_install_state = 0;
static volatile LONG g_calls[NRAT_API_COUNT];
static volatile LONG g_corrections_attempted = 0;
static volatile LONG g_corrections_succeeded = 0;
static volatile LONG g_corrections_failed = 0;
// comma-separated thread names to widen, from [affinity] threads in ember.ini
static WCHAR g_targets[1024] = L"EWP_LOW_Single_Clus1";
static NratRealApis g_real;
static NratLogger g_logger;
static NratPatch g_patches[NRAT_PATCH_CAPACITY];
static uint32_t g_patch_count = 0;

static const char *const g_api_names[] = {
    "SetThreadAffinityMask",
    "SetProcessAffinityMask",
    "SetThreadIdealProcessor",
    "SetThreadDescription",
    "GetProcessAffinityMask",
    "GetSystemCpuSetInformation",
    "GetProcAddress",
    "internal",
    "targeted_fix",
};

#define NRAT_RETURN_ADDRESS() __builtin_return_address(0)

static DWORD nrat_thread_id(HANDLE thread) {
    if (thread == NULL) {
        return 0;
    }
    if (thread == GetCurrentThread()) {
        return GetCurrentThreadId();
    }
    return GetThreadId(thread);
}

static DWORD nrat_process_id(HANDLE process) {
    if (process == NULL) {
        return 0;
    }
    if (process == GetCurrentProcess()) {
        return GetCurrentProcessId();
    }
    return GetProcessId(process);
}

static void nrat_copy_description(WCHAR *destination, size_t capacity, PCWSTR source) {
    size_t index = 0;
    if (capacity == 0) {
        return;
    }
    if (source != NULL) {
        while (index + 1 < capacity && source[index] != L'\0') {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = L'\0';
}

static void nrat_submit(const NratRecord *record) {
    LONG ready = InterlockedCompareExchange(&g_logger.ready, 0, 0);
    if (ready == 0) {
        return;
    }
    if (!TryEnterCriticalSection(&g_logger.lock)) {
        InterlockedIncrement(&g_logger.dropped);
        return;
    }

    uint32_t next = (g_logger.head + 1u) % NRAT_RING_CAPACITY;
    if (next == g_logger.tail) {
        LeaveCriticalSection(&g_logger.lock);
        InterlockedIncrement(&g_logger.dropped);
        return;
    }

    g_logger.ring[g_logger.head] = *record;
    g_logger.head = next;
    InterlockedIncrement(&g_logger.enqueued);
    LeaveCriticalSection(&g_logger.lock);
    SetEvent(g_logger.event);
}

static void nrat_record_base(NratRecord *record, uint32_t api, const void *caller) {
    ZeroMemory(record, sizeof(*record));
    record->api = api;
    QueryPerformanceCounter(&record->qpc);
    record->caller_tid = GetCurrentThreadId();
    record->caller = caller;
}

static BOOL nrat_write_all(HANDLE file, const char *buffer, DWORD length) {
    while (length != 0) {
        DWORD written = 0;
        if (!WriteFile(file, buffer, length, &written, NULL) || written == 0) {
            return FALSE;
        }
        buffer += written;
        length -= written;
    }
    return TRUE;
}

static void nrat_append(char **cursor, size_t *remaining, const char *format, ...) {
    if (*remaining <= 1) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(*cursor, *remaining, format, arguments);
    va_end(arguments);
    if (result < 0) {
        **cursor = '\0';
        *remaining = 1;
        return;
    }
    size_t used = (size_t)result;
    if (used >= *remaining) {
        *cursor += *remaining - 1;
        *remaining = 1;
        **cursor = '\0';
        return;
    }
    *cursor += used;
    *remaining -= used;
}

static void nrat_wide_to_safe_utf8(PCWSTR source, char *destination, size_t capacity) {
    if (capacity == 0) {
        return;
    }
    destination[0] = '\0';
    if (source == NULL || source[0] == L'\0') {
        return;
    }
    int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        source,
        -1,
        destination,
        (int)capacity,
        NULL,
        NULL
    );
    if (converted == 0) {
        destination[0] = '\0';
        return;
    }
    for (size_t index = 0; destination[index] != '\0'; ++index) {
        unsigned char value = (unsigned char)destination[index];
        if (destination[index] == '"' || destination[index] == '\\' || value < 0x20) {
            destination[index] = '_';
        }
    }
}

static void nrat_resolve_caller(
    const void *caller,
    char *module_name,
    size_t module_name_capacity,
    ULONG_PTR *rva
) {
    module_name[0] = '\0';
    *rva = 0;
    if (caller == NULL) {
        return;
    }
    HMODULE module = NULL;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (PCWSTR)caller,
            &module)) {
        return;
    }
    WCHAR path[MAX_PATH];
    DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return;
    }
    PCWSTR base_name = path;
    for (DWORD index = 0; index < length; ++index) {
        if (path[index] == L'\\' || path[index] == L'/') {
            base_name = &path[index + 1];
        }
    }
    nrat_wide_to_safe_utf8(base_name, module_name, module_name_capacity);
    *rva = (ULONG_PTR)((const BYTE *)caller - (const BYTE *)module);
}

static size_t nrat_format_record(const NratRecord *record, char *line, size_t capacity) {
    char module_name[MAX_PATH];
    char description[NRAT_DESCRIPTION_CHARS * 4u];
    ULONG_PTR caller_rva = 0;
    nrat_resolve_caller(record->caller, module_name, sizeof(module_name), &caller_rva);
    nrat_wide_to_safe_utf8(record->description, description, sizeof(description));

    char *cursor = line;
    size_t remaining = capacity;
    const char *api_name = record->api <= NRAT_API_TARGETED_FIX
        ? g_api_names[record->api]
        : "unknown";

    nrat_append(
        &cursor,
        &remaining,
        "{\"type\":\"api\",\"api\":\"%s\",\"qpc\":%lld,"
        "\"caller_tid\":%lu,\"target_tid\":%lu,\"target_pid\":%lu,"
        "\"arg0\":\"0x%llx\",\"arg1\":\"0x%llx\",\"arg2\":\"0x%llx\","
        "\"result0\":\"0x%llx\",\"result1\":\"0x%llx\","
        "\"last_error\":%lu,\"caller_module\":\"%s\","
        "\"caller_rva\":\"0x%llx\",\"description\":\"%s\"",
        api_name,
        (long long)record->qpc.QuadPart,
        (unsigned long)record->caller_tid,
        (unsigned long)record->target_tid,
        (unsigned long)record->target_pid,
        (unsigned long long)record->arg0,
        (unsigned long long)record->arg1,
        (unsigned long long)record->arg2,
        (unsigned long long)record->result0,
        (unsigned long long)record->result1,
        (unsigned long)record->last_error,
        module_name,
        (unsigned long long)caller_rva,
        description
    );

    if (record->cpu_set_count != 0) {
        nrat_append(&cursor, &remaining, ",\"cpu_sets\":[");
        for (uint32_t index = 0; index < record->cpu_set_count; ++index) {
            const NratCpuSetSnapshot *cpu = &record->cpu_sets[index];
            nrat_append(
                &cursor,
                &remaining,
                "%s{\"id\":%lu,\"group\":%u,\"logical\":%u,\"core\":%u,"
                "\"llc\":%u,\"numa\":%u,\"efficiency\":%u,\"flags\":%u}",
                index == 0 ? "" : ",",
                (unsigned long)cpu->id,
                (unsigned)cpu->group,
                (unsigned)cpu->logical_processor_index,
                (unsigned)cpu->core_index,
                (unsigned)cpu->last_level_cache_index,
                (unsigned)cpu->numa_node_index,
                (unsigned)cpu->efficiency_class,
                (unsigned)cpu->flags
            );
        }
        nrat_append(&cursor, &remaining, "]");
    }
    nrat_append(&cursor, &remaining, "}\r\n");
    return (size_t)(cursor - line);
}

static BOOL nrat_pop_batch(NratRecord *batch, uint32_t *count) {
    *count = 0;
    EnterCriticalSection(&g_logger.lock);
    while (g_logger.tail != g_logger.head && *count < NRAT_WRITER_BATCH) {
        batch[*count] = g_logger.ring[g_logger.tail];
        g_logger.tail = (g_logger.tail + 1u) % NRAT_RING_CAPACITY;
        ++*count;
    }
    BOOL has_more = g_logger.tail != g_logger.head;
    LeaveCriticalSection(&g_logger.lock);
    return has_more;
}

static void nrat_write_stats(void) {
    char line[384];
    int length = snprintf(
        line,
        sizeof(line),
        "{\"type\":\"stats\",\"enqueued\":%ld,\"dropped\":%ld,"
        "\"corrections_attempted\":%ld,\"corrections_succeeded\":%ld,"
        "\"corrections_failed\":%ld}\r\n",
        (long)InterlockedCompareExchange(&g_logger.enqueued, 0, 0),
        (long)InterlockedCompareExchange(&g_logger.dropped, 0, 0),
        (long)InterlockedCompareExchange(&g_corrections_attempted, 0, 0),
        (long)InterlockedCompareExchange(&g_corrections_succeeded, 0, 0),
        (long)InterlockedCompareExchange(&g_corrections_failed, 0, 0)
    );
    if (length > 0) {
        nrat_write_all(g_logger.file, line, (DWORD)length);
    }
}

static DWORD WINAPI nrat_writer_thread(void *unused) {
    (void)unused;
    NratRecord batch[NRAT_WRITER_BATCH];
    char line[16384];
    ULONGLONG last_stats = 0;
    for (;;) {
        HANDLE handles[2] = {g_logger.stop_event, g_logger.event};
        DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 250);
        BOOL stopping = wait == WAIT_OBJECT_0;
        BOOL has_more;
        do {
            uint32_t count = 0;
            has_more = nrat_pop_batch(batch, &count);
            for (uint32_t index = 0; index < count; ++index) {
                size_t length = nrat_format_record(&batch[index], line, sizeof(line));
                nrat_write_all(g_logger.file, line, (DWORD)length);
            }
        } while (has_more);
        ULONGLONG now = GetTickCount64();
        if (stopping || last_stats == 0 || now - last_stats >= 1000) {
            nrat_write_stats();
            last_stats = now;
        }
        FlushFileBuffers(g_logger.file);
        if (stopping) {
            break;
        }
    }
    return 0;
}

static BOOL nrat_logger_initialize(PCWSTR log_path) {
    ZeroMemory(&g_logger, sizeof(g_logger));
    InitializeCriticalSection(&g_logger.lock);
    g_logger.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_logger.stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_logger.event == NULL || g_logger.stop_event == NULL) {
        goto fail;
    }
    g_logger.file = CreateFileW(
        log_path,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (g_logger.file == INVALID_HANDLE_VALUE) {
        g_logger.file = NULL;
        goto fail;
    }

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    char header[512];
    int header_length = snprintf(
        header,
        sizeof(header),
        "{\"type\":\"session\",\"schema\":%u,\"pid\":%lu,"
        "\"qpc_frequency\":%lld,\"mode\":\"targeted-fix\"}\r\n",
        NRAT_VERSION,
        (unsigned long)GetCurrentProcessId(),
        (long long)frequency.QuadPart
    );
    if (header_length <= 0 || !nrat_write_all(g_logger.file, header, (DWORD)header_length)) {
        goto fail;
    }

    g_logger.writer_thread = CreateThread(NULL, 0, nrat_writer_thread, NULL, 0, NULL);
    if (g_logger.writer_thread == NULL) {
        goto fail;
    }
    InterlockedExchange(&g_logger.ready, 1);
    return TRUE;

fail:
    if (g_logger.writer_thread != NULL) {
        CloseHandle(g_logger.writer_thread);
    }
    if (g_logger.file != NULL) {
        CloseHandle(g_logger.file);
    }
    if (g_logger.event != NULL) {
        CloseHandle(g_logger.event);
    }
    if (g_logger.stop_event != NULL) {
        CloseHandle(g_logger.stop_event);
    }
    DeleteCriticalSection(&g_logger.lock);
    ZeroMemory(&g_logger, sizeof(g_logger));
    return FALSE;
}

static void nrat_logger_shutdown(void) {
    if (InterlockedExchange(&g_logger.ready, 0) == 0) {
        return;
    }
    SetEvent(g_logger.stop_event);
    WaitForSingleObject(g_logger.writer_thread, 5000);
    CloseHandle(g_logger.writer_thread);
    CloseHandle(g_logger.file);
    CloseHandle(g_logger.event);
    CloseHandle(g_logger.stop_event);
    DeleteCriticalSection(&g_logger.lock);
    ZeroMemory(&g_logger, sizeof(g_logger));
}

static void nrat_set_real(uint32_t api, void *function) {
    switch (api) {
        case NRAT_API_SET_THREAD_AFFINITY:
            g_real.set_thread_affinity = (NratSetThreadAffinityMaskFn)function;
            break;
        case NRAT_API_SET_PROCESS_AFFINITY:
            g_real.set_process_affinity = (NratSetProcessAffinityMaskFn)function;
            break;
        case NRAT_API_SET_THREAD_IDEAL:
            g_real.set_thread_ideal = (NratSetThreadIdealProcessorFn)function;
            break;
        case NRAT_API_SET_THREAD_DESCRIPTION:
            g_real.set_thread_description = (NratSetThreadDescriptionFn)function;
            break;
        case NRAT_API_GET_PROCESS_AFFINITY:
            g_real.get_process_affinity = (NratGetProcessAffinityMaskFn)function;
            break;
        case NRAT_API_GET_SYSTEM_CPU_SETS:
            g_real.get_system_cpu_sets = (NratGetSystemCpuSetInformationFn)function;
            break;
        case NRAT_API_GET_PROC_ADDRESS:
            g_real.get_proc_address = (NratGetProcAddressFn)function;
            break;
        default:
            break;
    }
}

static void *nrat_hook_for_api(uint32_t api) {
    switch (api) {
        case NRAT_API_SET_THREAD_AFFINITY:
            return (void *)NratHookSetThreadAffinityMask;
        case NRAT_API_SET_PROCESS_AFFINITY:
            return (void *)NratHookSetProcessAffinityMask;
        case NRAT_API_SET_THREAD_IDEAL:
            return (void *)NratHookSetThreadIdealProcessor;
        case NRAT_API_SET_THREAD_DESCRIPTION:
            return (void *)NratHookSetThreadDescription;
        case NRAT_API_GET_PROCESS_AFFINITY:
            return (void *)NratHookGetProcessAffinityMask;
        case NRAT_API_GET_SYSTEM_CPU_SETS:
            return (void *)NratHookGetSystemCpuSetInformation;
        case NRAT_API_GET_PROC_ADDRESS:
            return (void *)NratHookGetProcAddress;
        default:
            return NULL;
    }
}

static int nrat_api_for_name(const char *name) {
    for (uint32_t index = 0; index < NRAT_API_COUNT; ++index) {
        if (strcmp(name, g_api_names[index]) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static BOOL nrat_in_image(BYTE *base, SIZE_T image_size, const void *pointer, SIZE_T size) {
    const BYTE *value = (const BYTE *)pointer;
    if (value < base || size > image_size) {
        return FALSE;
    }
    SIZE_T offset = (SIZE_T)(value - base);
    return offset <= image_size - size;
}

static BOOL nrat_patch_slot(
    void **slot,
    void *expected,
    void *hook,
    void **original
) {
    DWORD old_protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protection)) {
        return FALSE;
    }
    *original = InterlockedExchangePointer((void *volatile *)slot, hook);
    if (*original != expected) {
        DWORD exchange_error = ERROR_RETRY;
        InterlockedExchangePointer((void *volatile *)slot, *original);
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(*slot), old_protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
        SetLastError(exchange_error);
        return FALSE;
    }
    DWORD ignored = 0;
    if (!VirtualProtect(slot, sizeof(*slot), old_protection, &ignored)) {
        DWORD restore_error = GetLastError();
        InterlockedExchangePointer((void *volatile *)slot, *original);
        VirtualProtect(slot, sizeof(*slot), old_protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
        SetLastError(restore_error);
        return FALSE;
    }
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    return TRUE;
}

static BOOL nrat_restore_patches(void) {
    BOOL success = TRUE;
    DWORD first_error = ERROR_SUCCESS;
    while (g_patch_count != 0) {
        NratPatch *patch = &g_patches[g_patch_count - 1u];
        DWORD old_protection = 0;
        if (!VirtualProtect(patch->slot, sizeof(*patch->slot), PAGE_READWRITE, &old_protection)) {
            success = FALSE;
            first_error = GetLastError();
            break;
        } else {
            InterlockedExchangePointer((void *volatile *)patch->slot, patch->original);
            --g_patch_count;
            DWORD ignored = 0;
            if (!VirtualProtect(patch->slot, sizeof(*patch->slot), old_protection, &ignored)) {
                success = FALSE;
                if (first_error == ERROR_SUCCESS) {
                    first_error = GetLastError();
                }
            }
        }
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    if (!success) {
        SetLastError(first_error);
    }
    return success;
}

static BOOL nrat_resolve_real_apis(void) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == NULL) {
        return FALSE;
    }
    ZeroMemory(&g_real, sizeof(g_real));
    for (uint32_t api = 0; api < NRAT_API_COUNT; ++api) {
        FARPROC function = GetProcAddress(kernel32, g_api_names[api]);
        if (function == NULL) {
            ZeroMemory(&g_real, sizeof(g_real));
            return FALSE;
        }
        nrat_set_real(api, (void *)function);
    }
    return TRUE;
}

static BOOL nrat_patch_thunks(
    BYTE *base,
    SIZE_T image_size,
    IMAGE_THUNK_DATA64 *lookup,
    IMAGE_THUNK_DATA64 *address,
    uint32_t found[NRAT_API_COUNT]
) {
    for (;;) {
        if (!nrat_in_image(base, image_size, lookup, sizeof(*lookup)) ||
            !nrat_in_image(base, image_size, address, sizeof(*address))) {
            return FALSE;
        }
        if (lookup->u1.AddressOfData == 0) {
            break;
        }
        if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) {
            ++lookup;
            ++address;
            continue;
        }
        IMAGE_IMPORT_BY_NAME *import_name =
            (IMAGE_IMPORT_BY_NAME *)(base + lookup->u1.AddressOfData);
        if (!nrat_in_image(base, image_size, import_name, sizeof(*import_name))) {
            return FALSE;
        }
        int api = nrat_api_for_name((const char *)import_name->Name);
        if (api < 0) {
            ++lookup;
            ++address;
            continue;
        }
        if (g_patch_count >= NRAT_PATCH_CAPACITY) {
            return FALSE;
        }
        void **slot = (void **)&address->u1.Function;
        void *current = *slot;
        void *original = NULL;
        void *hook = nrat_hook_for_api((uint32_t)api);
        if (!nrat_patch_slot(slot, current, hook, &original)) {
            return FALSE;
        }
        g_patches[g_patch_count].slot = slot;
        g_patches[g_patch_count].original = original;
        g_patches[g_patch_count].hook = hook;
        g_patches[g_patch_count].api = (uint32_t)api;
        ++g_patch_count;
        ++found[api];
        ++lookup;
        ++address;
    }
    return TRUE;
}

static BOOL nrat_install_iat_hooks(HMODULE module) {
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return FALSE;
    }
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return FALSE;
    }
    SIZE_T image_size = nt->OptionalHeader.SizeOfImage;
    uint32_t found[NRAT_API_COUNT] = {0};
    g_patch_count = 0;
    if (!nrat_resolve_real_apis()) {
        return FALSE;
    }

    IMAGE_DATA_DIRECTORY imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress != 0 && imports.Size >= sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR *descriptor =
            (IMAGE_IMPORT_DESCRIPTOR *)(base + imports.VirtualAddress);
        if (!nrat_in_image(base, image_size, descriptor, sizeof(*descriptor))) {
            goto fail;
        }
        for (;;) {
            if (!nrat_in_image(base, image_size, descriptor, sizeof(*descriptor))) {
                goto fail;
            }
            if (descriptor->Name == 0) {
                break;
            }
            if (descriptor->OriginalFirstThunk == 0 || descriptor->FirstThunk == 0) {
                ++descriptor;
                continue;
            }
            IMAGE_THUNK_DATA64 *lookup =
                (IMAGE_THUNK_DATA64 *)(base + descriptor->OriginalFirstThunk);
            IMAGE_THUNK_DATA64 *address =
                (IMAGE_THUNK_DATA64 *)(base + descriptor->FirstThunk);
            if (!nrat_patch_thunks(base, image_size, lookup, address, found)) {
                goto fail;
            }
            ++descriptor;
        }
    }

    IMAGE_DATA_DIRECTORY delays =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delays.VirtualAddress != 0 &&
        delays.Size >= sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
        PIMAGE_DELAYLOAD_DESCRIPTOR descriptor =
            (PIMAGE_DELAYLOAD_DESCRIPTOR)(base + delays.VirtualAddress);
        BYTE *delay_end = base + delays.VirtualAddress + delays.Size;
        while ((BYTE *)(descriptor + 1) <= delay_end && descriptor->DllNameRVA != 0) {
            if (!nrat_in_image(base, image_size, descriptor, sizeof(*descriptor)) ||
                !descriptor->Attributes.RvaBased ||
                descriptor->ImportNameTableRVA == 0 ||
                descriptor->ImportAddressTableRVA == 0) {
                goto fail;
            }
            IMAGE_THUNK_DATA64 *lookup =
                (IMAGE_THUNK_DATA64 *)(base + descriptor->ImportNameTableRVA);
            IMAGE_THUNK_DATA64 *address =
                (IMAGE_THUNK_DATA64 *)(base + descriptor->ImportAddressTableRVA);
            if (!nrat_patch_thunks(base, image_size, lookup, address, found)) {
                goto fail;
            }
            ++descriptor;
        }
    }

    if (found[NRAT_API_GET_PROC_ADDRESS] == 0) {
        goto fail;
    }
    return TRUE;

fail:
    nrat_restore_patches();
    if (g_patch_count == 0) {
        ZeroMemory(&g_real, sizeof(g_real));
    }
    return FALSE;
}

static void nrat_internal_record(ULONG_PTR result0, DWORD last_error) {
    NratRecord record;
    nrat_record_base(&record, NRAT_API_INTERNAL, NRAT_RETURN_ADDRESS());
    record.result0 = result0;
    record.last_error = last_error;
    nrat_submit(&record);
}

#ifndef NRAT_NO_DLLMAIN
static BOOL nrat_default_log_path(WCHAR *path, size_t capacity) {
    DWORD length = GetModuleFileNameW(g_self, path, (DWORD)capacity);
    if (length == 0 || length >= capacity) {
        return FALSE;
    }
    size_t separator = 0;
    for (size_t index = 0; index < length; ++index) {
        if (path[index] == L'\\' || path[index] == L'/') {
            separator = index + 1;
        }
    }
    int result = swprintf(
        &path[separator],
        capacity - separator,
        L"ember-affinity-%lu.jsonl",
        (unsigned long)GetCurrentProcessId()
    );
    return result > 0;
}
#endif

BOOL WINAPI NratInstallForModule(HMODULE module, PCWSTR log_path) {
    if (module == NULL || log_path == NULL || log_path[0] == L'\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (InterlockedCompareExchange(&g_install_state, 1, 0) != 0) {
        SetLastError(ERROR_ALREADY_INITIALIZED);
        return FALSE;
    }
    if (!nrat_logger_initialize(log_path)) {
        InterlockedExchange(&g_install_state, -1);
        return FALSE;
    }
    if (!nrat_install_iat_hooks(module)) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_PROC_NOT_FOUND;
        }
        nrat_internal_record(g_patch_count, error);
        if (g_patch_count != 0) {
            /* Keep the DLL and logger alive if an exceptional rollback failure
               leaves any IAT slot pointing at a wrapper. */
            InterlockedExchange(&g_install_state, 2);
            SetLastError(error);
            return FALSE;
        }
        Sleep(50);
        nrat_logger_shutdown();
        InterlockedExchange(&g_install_state, -1);
        SetLastError(error);
        return FALSE;
    }
    InterlockedExchange(&g_install_state, 2);
    nrat_internal_record(g_patch_count, ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI NratUninstall(void) {
    if (InterlockedCompareExchange(&g_install_state, 3, 2) != 2) {
        SetLastError(ERROR_INVALID_STATE);
        return FALSE;
    }
    BOOL restored = nrat_restore_patches();
    DWORD restore_error = restored ? ERROR_SUCCESS : GetLastError();
    nrat_internal_record(restored ? 1u : 0u, restore_error);
    if (!restored && g_patch_count != 0) {
        InterlockedExchange(&g_install_state, 2);
        SetLastError(restore_error);
        return FALSE;
    }
    Sleep(50);
    nrat_logger_shutdown();
    InterlockedExchange(&g_install_state, 0);
    if (!restored) {
        SetLastError(restore_error);
    }
    return restored;
}

BOOL WINAPI NratGetSnapshot(NratSnapshot *snapshot) {
    if (snapshot == NULL || snapshot->size < sizeof(*snapshot)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    snapshot->installed = InterlockedCompareExchange(&g_install_state, 0, 0) == 2;
    snapshot->patch_count = g_patch_count;
    snapshot->enqueued = (uint32_t)InterlockedCompareExchange(&g_logger.enqueued, 0, 0);
    snapshot->dropped = (uint32_t)InterlockedCompareExchange(&g_logger.dropped, 0, 0);
    snapshot->corrections_attempted =
        (uint32_t)InterlockedCompareExchange(&g_corrections_attempted, 0, 0);
    snapshot->corrections_succeeded =
        (uint32_t)InterlockedCompareExchange(&g_corrections_succeeded, 0, 0);
    snapshot->corrections_failed =
        (uint32_t)InterlockedCompareExchange(&g_corrections_failed, 0, 0);
    for (uint32_t api = 0; api < NRAT_API_COUNT; ++api) {
        snapshot->calls[api] = (uint32_t)InterlockedCompareExchange(&g_calls[api], 0, 0);
    }
    return TRUE;
}

DWORD_PTR WINAPI NratHookSetThreadAffinityMask(HANDLE thread, DWORD_PTR mask) {
    const void *caller = NRAT_RETURN_ADDRESS();
    DWORD_PTR forwarded_mask = mask;
#ifdef NRAT_NEGATIVE_MUTATE_MASK
    forwarded_mask ^= 1u;
#endif
    DWORD_PTR result = g_real.set_thread_affinity(thread, forwarded_mask);
    DWORD error = GetLastError();
    InterlockedIncrement(&g_calls[NRAT_API_SET_THREAD_AFFINITY]);
    NratRecord record;
    nrat_record_base(&record, NRAT_API_SET_THREAD_AFFINITY, caller);
    record.target_tid = nrat_thread_id(thread);
    record.arg0 = mask;
    record.arg1 = forwarded_mask;
    record.result0 = result;
    record.last_error = error;
    nrat_submit(&record);
    SetLastError(error);
    return result;
}

BOOL WINAPI NratHookSetProcessAffinityMask(HANDLE process, DWORD_PTR mask) {
    const void *caller = NRAT_RETURN_ADDRESS();
    BOOL result = g_real.set_process_affinity(process, mask);
    DWORD error = GetLastError();
    InterlockedIncrement(&g_calls[NRAT_API_SET_PROCESS_AFFINITY]);
    NratRecord record;
    nrat_record_base(&record, NRAT_API_SET_PROCESS_AFFINITY, caller);
    record.target_pid = nrat_process_id(process);
    record.arg0 = mask;
    record.result0 = (ULONG_PTR)result;
    record.last_error = error;
    nrat_submit(&record);
    SetLastError(error);
    return result;
}

DWORD WINAPI NratHookSetThreadIdealProcessor(HANDLE thread, DWORD processor) {
    const void *caller = NRAT_RETURN_ADDRESS();
    DWORD result = g_real.set_thread_ideal(thread, processor);
    DWORD error = GetLastError();
    InterlockedIncrement(&g_calls[NRAT_API_SET_THREAD_IDEAL]);
    NratRecord record;
    nrat_record_base(&record, NRAT_API_SET_THREAD_IDEAL, caller);
    record.target_tid = nrat_thread_id(thread);
    record.arg0 = processor;
    record.result0 = result;
    record.last_error = error;
    nrat_submit(&record);
    SetLastError(error);
    return result;
}

static BOOL nrat_is_target(PCWSTR description) {
    size_t length = wcslen(description);
    for (PCWSTR item = g_targets; *item != L'\0';) {
        while (*item == L' ' || *item == L',') ++item;
        PCWSTR end = item;
        while (*end != L'\0' && *end != L',') ++end;
        PCWSTR last = end;
        while (last > item && last[-1] == L' ') --last;
        if ((size_t)(last - item) == length && length != 0 && wcsncmp(item, description, length) == 0) return TRUE;
        item = end;
    }
    return FALSE;
}

HRESULT WINAPI NratHookSetThreadDescription(HANDLE thread, PCWSTR description) {
    const void *caller = NRAT_RETURN_ADDRESS();
    HRESULT result = g_real.set_thread_description(thread, description);
    DWORD error = GetLastError();
    InterlockedIncrement(&g_calls[NRAT_API_SET_THREAD_DESCRIPTION]);
    NratRecord record;
    nrat_record_base(&record, NRAT_API_SET_THREAD_DESCRIPTION, caller);
    record.target_tid = nrat_thread_id(thread);
    record.result0 = (ULONG_PTR)(LONG_PTR)result;
    record.last_error = error;
    nrat_copy_description(record.description, NRAT_DESCRIPTION_CHARS, description);
    nrat_submit(&record);

    if (SUCCEEDED(result) && description != NULL &&
        nrat_is_target(description)) {
        InterlockedIncrement(&g_corrections_attempted);
        DWORD_PTR process_mask = 0;
        DWORD_PTR system_mask = 0;
        BOOL queried = g_real.get_process_affinity(
            GetCurrentProcess(),
            &process_mask,
            &system_mask
        );
        DWORD correction_error = GetLastError();
        DWORD_PTR correction_mask = process_mask;
#ifdef NRAT_NEGATIVE_CORRECTION_MASK
        correction_mask = (DWORD_PTR)0xEu;
#endif
        DWORD_PTR previous_mask = 0;
        BOOL corrected = FALSE;
        if (queried && process_mask != 0 && correction_mask != 0) {
            previous_mask = g_real.set_thread_affinity(thread, correction_mask);
            correction_error = GetLastError();
            corrected = previous_mask != 0;
        }
        if (corrected) {
            InterlockedIncrement(&g_corrections_succeeded);
        } else {
            InterlockedIncrement(&g_corrections_failed);
        }

        NratRecord correction;
        nrat_record_base(&correction, NRAT_API_TARGETED_FIX, caller);
        correction.target_tid = nrat_thread_id(thread);
        correction.target_pid = GetCurrentProcessId();
        correction.arg0 = process_mask;
        correction.arg1 = correction_mask;
        correction.arg2 = system_mask;
        correction.result0 = previous_mask;
        correction.result1 = (ULONG_PTR)corrected;
        correction.last_error = correction_error;
        nrat_copy_description(
            correction.description,
            NRAT_DESCRIPTION_CHARS,
            description
        );
        nrat_submit(&correction);
    }
    SetLastError(error);
    return result;
}

BOOL WINAPI NratHookGetProcessAffinityMask(
    HANDLE process,
    PDWORD_PTR process_mask,
    PDWORD_PTR system_mask
) {
    const void *caller = NRAT_RETURN_ADDRESS();
    BOOL result = g_real.get_process_affinity(process, process_mask, system_mask);
    DWORD error = GetLastError();
    InterlockedIncrement(&g_calls[NRAT_API_GET_PROCESS_AFFINITY]);
    NratRecord record;
    nrat_record_base(&record, NRAT_API_GET_PROCESS_AFFINITY, caller);
    record.target_pid = nrat_process_id(process);
    record.result0 = result && process_mask != NULL ? *process_mask : 0;
    record.result1 = result && system_mask != NULL ? *system_mask : 0;
    record.arg0 = (ULONG_PTR)result;
    record.last_error = error;
    nrat_submit(&record);
    SetLastError(error);
    return result;
}

BOOL WINAPI NratHookGetSystemCpuSetInformation(
    PSYSTEM_CPU_SET_INFORMATION information,
    ULONG buffer_length,
    PULONG returned_length,
    HANDLE process,
    ULONG flags
) {
    const void *caller = NRAT_RETURN_ADDRESS();
    BOOL result = g_real.get_system_cpu_sets(
        information,
        buffer_length,
        returned_length,
        process,
        flags
    );
    DWORD error = GetLastError();
    InterlockedIncrement(&g_calls[NRAT_API_GET_SYSTEM_CPU_SETS]);
    NratRecord record;
    nrat_record_base(&record, NRAT_API_GET_SYSTEM_CPU_SETS, caller);
    record.target_pid = nrat_process_id(process);
    record.arg0 = buffer_length;
    record.arg1 = flags;
    record.result0 = (ULONG_PTR)result;
    record.result1 = returned_length != NULL ? *returned_length : 0;
    record.last_error = error;

    if (result && information != NULL && returned_length != NULL) {
        ULONG offset = 0;
        ULONG total = *returned_length < buffer_length ? *returned_length : buffer_length;
        while (offset + sizeof(SYSTEM_CPU_SET_INFORMATION) <= total &&
               record.cpu_set_count < NRAT_CPU_SET_CAPACITY) {
            PSYSTEM_CPU_SET_INFORMATION item =
                (PSYSTEM_CPU_SET_INFORMATION)((BYTE *)information + offset);
            if (item->Size < sizeof(SYSTEM_CPU_SET_INFORMATION) ||
                item->Size > total - offset) {
                break;
            }
            if (item->Type == CpuSetInformation) {
                NratCpuSetSnapshot *snapshot =
                    &record.cpu_sets[record.cpu_set_count++];
                snapshot->id = item->CpuSet.Id;
                snapshot->group = item->CpuSet.Group;
                snapshot->logical_processor_index = item->CpuSet.LogicalProcessorIndex;
                snapshot->core_index = item->CpuSet.CoreIndex;
                snapshot->last_level_cache_index = item->CpuSet.LastLevelCacheIndex;
                snapshot->numa_node_index = item->CpuSet.NumaNodeIndex;
                snapshot->efficiency_class = item->CpuSet.EfficiencyClass;
                snapshot->flags = item->CpuSet.AllFlags;
            }
            offset += item->Size;
        }
    }
    nrat_submit(&record);
    SetLastError(error);
    return result;
}

FARPROC WINAPI NratHookGetProcAddress(HMODULE module, LPCSTR procedure_name) {
    const void *caller = NRAT_RETURN_ADDRESS();
    FARPROC original = g_real.get_proc_address(module, procedure_name);
    DWORD error = GetLastError();
    FARPROC forwarded = original;
    BOOL by_name = ((ULONG_PTR)procedure_name >> 16u) != 0;
    int resolved_api = -1;
    if (original != NULL && by_name) {
        resolved_api = nrat_api_for_name(procedure_name);
        if (resolved_api >= 0 && resolved_api < NRAT_API_GET_PROC_ADDRESS) {
            forwarded = (FARPROC)nrat_hook_for_api((uint32_t)resolved_api);
        }
    }

    InterlockedIncrement(&g_calls[NRAT_API_GET_PROC_ADDRESS]);
    NratRecord record;
    nrat_record_base(&record, NRAT_API_GET_PROC_ADDRESS, caller);
    record.arg0 = (ULONG_PTR)module;
    record.arg1 = by_name ? (ULONG_PTR)(resolved_api + 1) : 0;
    record.result0 = (ULONG_PTR)original;
    record.result1 = (ULONG_PTR)forwarded;
    record.last_error = error;
    if (by_name) {
        size_t index = 0;
        while (index + 1 < NRAT_DESCRIPTION_CHARS && procedure_name[index] != '\0') {
            record.description[index] = (WCHAR)(unsigned char)procedure_name[index];
            ++index;
        }
        record.description[index] = L'\0';
    }
    nrat_submit(&record);
    SetLastError(error);
    return forwarded;
}

#ifdef NRAT_TESTING
void NratTestSetRealApis(const NratRealApis *apis) {
    g_real = *apis;
    ZeroMemory((void *)g_calls, sizeof(g_calls));
    InterlockedExchange(&g_corrections_attempted, 0);
    InterlockedExchange(&g_corrections_succeeded, 0);
    InterlockedExchange(&g_corrections_failed, 0);
}
#endif

#ifndef NRAT_NO_DLLMAIN
static DWORD WINAPI nrat_auto_start(void *unused) {
    (void)unused;
    WCHAR executable[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return 0;
    }
    PCWSTR base_name = executable;
    for (DWORD index = 0; index < length; ++index) {
        if (executable[index] == L'\\' || executable[index] == L'/') {
            base_name = &executable[index + 1];
        }
    }
    if (_wcsicmp(base_name, L"nightreign.exe") != 0) {
        return 0;
    }
    WCHAR log_path[MAX_PATH];
    if (!nrat_default_log_path(log_path, MAX_PATH)) {
        return 0;
    }
    /* ember: settings come from ember.ini next to ember.dll, and the trace log is off unless [affinity] log=1. */
    WCHAR ini_path[MAX_PATH];
    wcscpy(ini_path, log_path);
    WCHAR *name = wcsrchr(ini_path, L'\\');
    if (name == NULL) name = wcsrchr(ini_path, L'/');
    name = name != NULL ? name + 1 : ini_path;
    wcscpy(name, L"ember.ini");
    if (!GetPrivateProfileIntW(L"affinity", L"enabled", 1, ini_path)) {
        return 0;
    }
    GetPrivateProfileStringW(L"affinity", L"threads", L"EWP_LOW_Single_Clus1", g_targets, 1024, ini_path);
    if (!GetPrivateProfileIntW(L"affinity", L"log", 0, ini_path)) {
        wcscpy(log_path, L"NUL");
    }
    NratInstallForModule(GetModuleHandleW(NULL), log_path);
    return 0;
}

/* ember: called from ember.dll's DllMain instead of the upstream DllMain. */
void affinity_attach(HINSTANCE instance) {
    g_self = instance;
    HANDLE thread = CreateThread(NULL, 0, nrat_auto_start, NULL, 0, NULL);
    if (thread != NULL) {
        CloseHandle(thread);
    }
}
#endif
