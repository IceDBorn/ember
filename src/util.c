#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ember.h"

static char mod_dir[MAX_PATH - 32]; // room for the file names appended by mod_path
static char log_file[MAX_PATH];
static int log_level = LOG_ERROR;
static CRITICAL_SECTION log_lock;
static FILE *out;
static bool log_failed;
static const char *const level_names[] = {"OFF", "ERROR", "WARN", "INFO", "DEBUG"};

void mod_dir_init(HINSTANCE self)
{
    DWORD n = GetModuleFileNameA(self, mod_dir, sizeof mod_dir);
    while (n && mod_dir[n - 1] != '\\' && mod_dir[n - 1] != '/')
        n--;
    mod_dir[n] = 0;
}

void mod_path(char *out_path, const char *name)
{
    snprintf(out_path, MAX_PATH, "%s%s", mod_dir, name);
}

int cfg(const char *section, const char *key, int def)
{
    char ini[MAX_PATH];
    mod_path(ini, "ember.ini");
    return (int)GetPrivateProfileIntA(section, key, def, ini);
}

// The log only holds this launch: an old one is removed even when nothing gets written.
void log_init(void)
{
    char ini[MAX_PATH], v[16];
    InitializeCriticalSection(&log_lock);
    mod_path(ini, "ember.ini");
    GetPrivateProfileStringA("log", "level", "error", v, sizeof v, ini);
    for (int i = 0; i < 5; i++)
        if (!_stricmp(v, level_names[i]))
            log_level = i;
    mod_path(log_file, "ember.log");
    DeleteFileA(log_file);
}

void log_line(int level, const char *fmt, ...)
{
    if (level > log_level)
        return;
    EnterCriticalSection(&log_lock);
    if (!out && !log_failed)
        log_failed = !(out = fopen(log_file, "w"));
    if (out) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(out, "%02d:%02d:%02d.%03d %-5s ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                level_names[level]);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(out, fmt, ap);
        va_end(ap);
        fputc('\n', out);
        fflush(out);
    }
    LeaveCriticalSection(&log_lock);
}

uint8_t *aob_find(const uint8_t *pat, size_t n)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE) || s->Misc.VirtualSize < n)
            continue;
        uint8_t *p = base + s->VirtualAddress, *end = p + s->Misc.VirtualSize - n;
        for (; p <= end; p++)
            if (*p == pat[0] && !memcmp(p, pat, n))
                return p;
    }
    return NULL;
}

bool patch_code(void *at, const void *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}
