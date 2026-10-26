#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <processthreadsapi.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NRAT_API_SET_THREAD_AFFINITY = 0,
    NRAT_API_SET_PROCESS_AFFINITY = 1,
    NRAT_API_SET_THREAD_IDEAL = 2,
    NRAT_API_SET_THREAD_DESCRIPTION = 3,
    NRAT_API_GET_PROCESS_AFFINITY = 4,
    NRAT_API_GET_SYSTEM_CPU_SETS = 5,
    NRAT_API_GET_PROC_ADDRESS = 6,
    NRAT_API_COUNT = 7,
    NRAT_API_INTERNAL = 7,
    NRAT_API_TARGETED_FIX = 8,
};

typedef DWORD_PTR(WINAPI *NratSetThreadAffinityMaskFn)(HANDLE, DWORD_PTR);
typedef BOOL(WINAPI *NratSetProcessAffinityMaskFn)(HANDLE, DWORD_PTR);
typedef DWORD(WINAPI *NratSetThreadIdealProcessorFn)(HANDLE, DWORD);
typedef HRESULT(WINAPI *NratSetThreadDescriptionFn)(HANDLE, PCWSTR);
typedef BOOL(WINAPI *NratGetProcessAffinityMaskFn)(HANDLE, PDWORD_PTR, PDWORD_PTR);
typedef BOOL(WINAPI *NratGetSystemCpuSetInformationFn)(
    PSYSTEM_CPU_SET_INFORMATION,
    ULONG,
    PULONG,
    HANDLE,
    ULONG
);
typedef FARPROC(WINAPI *NratGetProcAddressFn)(HMODULE, LPCSTR);

typedef struct NratRealApis {
    NratSetThreadAffinityMaskFn set_thread_affinity;
    NratSetProcessAffinityMaskFn set_process_affinity;
    NratSetThreadIdealProcessorFn set_thread_ideal;
    NratSetThreadDescriptionFn set_thread_description;
    NratGetProcessAffinityMaskFn get_process_affinity;
    NratGetSystemCpuSetInformationFn get_system_cpu_sets;
    NratGetProcAddressFn get_proc_address;
} NratRealApis;

typedef struct NratSnapshot {
    uint32_t size;
    uint32_t installed;
    uint32_t patch_count;
    uint32_t enqueued;
    uint32_t dropped;
    uint32_t corrections_attempted;
    uint32_t corrections_succeeded;
    uint32_t corrections_failed;
    uint32_t calls[NRAT_API_COUNT];
} NratSnapshot;

__declspec(dllexport) BOOL WINAPI NratInstallForModule(HMODULE module, PCWSTR log_path);
__declspec(dllexport) BOOL WINAPI NratUninstall(void);
__declspec(dllexport) BOOL WINAPI NratGetSnapshot(NratSnapshot *snapshot);

DWORD_PTR WINAPI NratHookSetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);
BOOL WINAPI NratHookSetProcessAffinityMask(HANDLE process, DWORD_PTR mask);
DWORD WINAPI NratHookSetThreadIdealProcessor(HANDLE thread, DWORD processor);
HRESULT WINAPI NratHookSetThreadDescription(HANDLE thread, PCWSTR description);
BOOL WINAPI NratHookGetProcessAffinityMask(
    HANDLE process,
    PDWORD_PTR process_mask,
    PDWORD_PTR system_mask
);
BOOL WINAPI NratHookGetSystemCpuSetInformation(
    PSYSTEM_CPU_SET_INFORMATION information,
    ULONG buffer_length,
    PULONG returned_length,
    HANDLE process,
    ULONG flags
);
FARPROC WINAPI NratHookGetProcAddress(HMODULE module, LPCSTR procedure_name);

#ifdef NRAT_TESTING
void NratTestSetRealApis(const NratRealApis *apis);
#endif

#ifdef __cplusplus
}
#endif
