#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

// Log levels from [log] level in ember.ini.
enum { LOG_OFF, LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG };

// util.c: paths next to ember.dll, settings and the log.
void mod_dir_init(HINSTANCE self);
void mod_path(char *out, const char *name);
int cfg(const char *section, const char *key, int def);
void log_init(void);
void log_line(int level, const char *fmt, ...);

// Code patches in the exe's executable sections.
uint8_t *aob_find(const uint8_t *pattern, size_t n);
bool patch_code(void *at, const void *bytes, size_t n);
bool iat_hook(const char *dll, const char *name, void *hook, void **orig);

// One start function per feature, each called once from DllMain.
void eventpool_start(void);
void sfx_start(void);
void fps_start(int limit);
void ultrawide_start(void);
void animation_start(void);
void loadbars_start(void);
void postfx_start(bool chromatic, bool vignette, bool sharpen);
void affinity_attach(HINSTANCE instance);
