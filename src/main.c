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
    cfg_write_default();
    log_init();
    if (cfg("eventpool", "enabled", 1))
        eventpool_start();
    if (cfg("sfx", "enabled", 1))
        sfx_start();
    if (cfg("fps", "limit", 144) > 0)
        fps_start(cfg("fps", "limit", 144));
    if (cfg("ultrawide", "enabled", 1))
        ultrawide_start();
    if (cfg("animation", "enabled", 1))
        animation_start();
    if (cfg("loadbars", "enabled", 1))
        loadbars_start();
    postfx_start(cfg("postfx", "remove_chromatic_aberration", 1), cfg("postfx", "remove_vignette", 1),
                 cfg("postfx", "remove_sharpen", 1));
    affinity_attach(self);
    log_line(LOG_INFO, "ember loaded");
    return TRUE;
}
