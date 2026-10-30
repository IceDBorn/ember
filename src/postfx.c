// Turns off chromatic aberration, vignette and the sharpen filter.
// Same edits as huutaiii's RemoveChromaticAberration, RemoveVignette and DisableSharpening.
#include "ember.h"

#include <string.h>

// Finds a NUL-terminated UTF-16 string in the exe's read-only data.
static uint8_t *find_wstr(const wchar_t *str)
{
    size_t n = (wcslen(str) + 1) * sizeof(wchar_t);
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE || s->Misc.VirtualSize < n)
            continue;
        uint8_t *p = base + s->VirtualAddress, *end = p + s->Misc.VirtualSize - n;
        for (; p <= end; p += 2)
            if (*p == (uint8_t)str[0] && !memcmp(p, str, n))
                return p;
    }
    return NULL;
}

// The game registers each effect under a "GS Presents - ..." label passed in r9, and an unknown label leaves
// it off. The callee reads the string, so `lea r9` moves 2 characters into the label instead of becoming NOPs.
static bool rename_label(const wchar_t *label)
{
    uint8_t *str = find_wstr(label);
    if (!str)
        return false;
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE) || s->Misc.VirtualSize < 7)
            continue;
        uint8_t *p = base + s->VirtualAddress, *end = p + s->Misc.VirtualSize - 7;
        for (; p <= end; p++) {
            if (p[0] != 0x4C || p[1] != 0x8D || p[2] != 0x0D)
                continue;
            int32_t rel;
            memcpy(&rel, p + 3, 4);
            if (p + 7 + rel == str) {
                rel += 2 * sizeof(wchar_t);
                bool ok = patch_code(p + 3, &rel, sizeof rel);
                log_line(ok ? LOG_INFO : LOG_ERROR, "postfx %ls %s at %p", label,
                         ok ? "removed" : "patch failed", p);
                return ok;
            }
        }
    }
    return false;
}

static void drop(const wchar_t *label)
{
    if (!rename_label(label))
        log_line(LOG_ERROR, "postfx %ls not found", label);
}

void postfx_start(bool chromatic, bool vignette, bool sharpen)
{
    if (chromatic) {
        drop(L"GS Presents - Chromatic Aberration");
        drop(L"GS Presents - Chromatic Aberration Enable");
    }
    if (vignette) {
        drop(L"GS Presents - Vignetting");
        drop(L"GS Presents - Vignetting Power");
    }
    if (sharpen) {
        // Without its file name the game never loads the sharpen pass.
        static const wchar_t name[] = L"SharpenFilter.ppo";
        uint8_t *at = find_wstr(name);
        static const wchar_t blank[sizeof name / sizeof *name] = {0};
        if (!at || !patch_code(at, blank, sizeof blank))
            log_line(LOG_ERROR, "postfx SharpenFilter.ppo not found");
        else
            log_line(LOG_INFO, "postfx sharpen filter removed at %p", at);
    }
}
