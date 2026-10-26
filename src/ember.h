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

// One start function per feature, each called once from DllMain.
void eventpool_start(void);
void sfx_start(void);
void affinity_attach(HINSTANCE instance);
