// Port of techiew's IncreaseAnimationDistance. The frustum builder used for animation culling passes the
// camera aspect ratio to the projection helper; reading the far clip distance instead widens it to ~180 degrees.
#include "ember.h"

// movss xmm3/xmm2/xmm1, [rbx+0x5c]/[rbx+0x58]/[rbx+0x54], aspect load last
static const uint8_t pattern[] = {0xC7, 0x44, 0x24, 0x30, 0x01, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x5B,
                                  0x5C, 0xF3, 0x0F, 0x10, 0x53, 0x58, 0xF3, 0x0F, 0x10, 0x4B, 0x54};

void animation_start(void)
{
    uint8_t *at = aob_find(pattern, sizeof pattern);
    const uint8_t far_clip = 0x5C;
    if (!at || !patch_code(at + sizeof pattern - 1, &far_clip, 1)) {
        log_line(LOG_ERROR, "animation frustum not found");
        return;
    }
    log_line(LOG_INFO, "animation frustum widened at %p", at);
}
