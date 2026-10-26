// ember: performance fixes for Elden Ring Nightreign, loaded early by me3.
#include "ember.h"

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(self);
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    while (n && exe[n - 1] != '\\' && exe[n - 1] != '/')
        n--;
    if (_stricmp(exe + n, "nightreign.exe"))
        return TRUE;
    mod_dir_init(self);
    log_init();
    if (cfg("eventpool", "enabled", 1))
        eventpool_start();
    if (cfg("sfx", "enabled", 1))
        sfx_start();
    affinity_attach(self);
    log_line(LOG_INFO, "ember loaded");
    return TRUE;
}
