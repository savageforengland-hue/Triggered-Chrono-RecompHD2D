/* SPDX-License-Identifier: GPL-3.0-or-later
 * Synthetic world-map WRAM/OAM: no game ROM or generated code. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bus.h"
#include "module.h"

static uint8_t wram[0x20000];
static uint16_t oam[272];
uint8_t *bus_wram(void) { return wram; }
int ct_world_scene(ct_scene *out);

static void word(unsigned address, unsigned value)
{
    wram[address] = (uint8_t)value;
    wram[address + 1] = (uint8_t)(value >> 8);
}

int main(void)
{
    const uint8_t nmi[] = {0x5C, 0x5E, 0x01, 0xC2};
    memcpy(wram + 0x500, nmi, sizeof nmi);
    word(0x100, 0x1F0);
    word(0x283, 600);
    word(0x285, 400);
    word(0x20, 300);
    word(0x26, 200);
    for (int k = 0; k < 128; k++)
        oam[2 * k] = 0xF000;
    /* A sprite at the usual screen center is not the offscreen leader. */
    oam[0] = 95 << 8 | 120;
    ct_scene s = {.oam = oam};
    assert(ct_world_scene(&s) == 0);
    assert(s.camera_x == 300 && s.camera_y == 200);
    assert(s.flags & CT_SCENE_SCRIPTED);
    assert(s.object[0].x == 600 && s.object[0].y == 400);
    assert(s.object[0].n_pieces == 0 && s.n_objects == 2);

    word(0x20, 472);
    word(0x26, 288);
    oam[0] = 0xF000;
    oam[2] = 95 << 8 | 120;
    assert(ct_world_scene(&s) == 0);
    assert(s.camera_x == 472 && s.camera_y == 288);
    assert(!(s.flags & CT_SCENE_SCRIPTED));
    assert(s.object[0].n_pieces == 1 && s.object[0].piece[0] == 1);

    /* The script pans while the leader's world position stays fixed. */
    word(0x20, 500);
    word(0x26, 302);
    oam[2] = 81 << 8 | 92;
    assert(ct_world_scene(&s) == 0);
    assert(s.camera_x == 500 && s.camera_y == 302);
    assert(s.flags & CT_SCENE_SCRIPTED);
    assert(s.object[0].x == 600 && s.object[0].y == 400);
    assert(s.object[0].n_pieces == 1 && s.object[0].piece[0] == 1);

    /* An event-controlled shot stays scripted even at the usual center. */
    word(0x20, 472);
    word(0x26, 288);
    wram[0x138] = 1;
    assert(ct_world_scene(&s) == 0);
    assert(s.flags & CT_SCENE_SCRIPTED);
    wram[0x138] = 0;

    word(0x283, 20);
    word(0x285, 20);
    word(0x20, 1500);
    word(0x26, 1000);
    oam[2] = 27 << 8 | 48;
    assert(ct_world_scene(&s) == 0);
    assert(s.camera_x == 1500 && s.camera_y == 1000);
    assert(s.object[0].n_pieces == 1 && s.object[0].piece[0] == 1);
    word(0x20, 1636);
    word(0x26, 1224);
    assert(ct_world_scene(&s) == 0);
    assert(s.camera_x == 100 && s.camera_y == 200);
    puts("world camera: ok");
    return 0;
}
