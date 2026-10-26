// Replaces the 60 fps frame time the game writes into its frame pacer with 1/limit.
// Pattern from ReignTweak by Minksh.
#include "ember.h"

// mov dword [rbx+0x20], 0x3c888889 (1/60 as a float)
static const uint8_t pattern[] = {0xC7, 0x43, 0x20, 0x89, 0x88, 0x88, 0x3C};

void fps_start(int limit)
{
    uint8_t *at = aob_find(pattern, sizeof pattern);
    float frame = 1.0f / limit;
    if (!at || !patch_code(at + 3, &frame, sizeof frame)) {
        log_line(LOG_ERROR, "fps frame time write not found");
        return;
    }
    log_line(LOG_INFO, "fps limit %d at %p", limit, at);
}
