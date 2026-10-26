// Skips the aspect ratio check that letterboxes resolutions wider than 16:9.
// Pattern from techiew's EldenRingMods, as used by ReignTweak.
#include "ember.h"

static const uint8_t pattern[] = {0x85, 0xC0, 0x74, 0x73, 0x46, 0x8B, 0x84, 0x21,
                                  0xB4, 0x04, 0x00, 0x00, 0x45, 0x85, 0xC0, 0x74};

void ultrawide_start(void)
{
    uint8_t *at = aob_find(pattern, sizeof pattern);
    const uint8_t jmp = 0xEB;
    if (!at || !patch_code(at + 15, &jmp, 1)) {
        log_line(LOG_ERROR, "ultrawide aspect check not found");
        return;
    }
    log_line(LOG_INFO, "ultrawide aspect check skipped at %p", at);
}
