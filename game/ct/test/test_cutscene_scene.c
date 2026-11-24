/* SPDX-License-Identifier: GPL-3.0-or-later
 * Cutscene classification from synthetic WRAM: no ROM or generated code. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "module.h"
#include "sprites.h"

static uint8_t wram[0x20000];
uint8_t *bus_wram(void) { return wram; }

int ct_world_scene(ct_scene *out)
{
    (void)out;
    return -1;
}

int ct_sprite_describe(int obj, ct_sprite_info *info)
{
    (void)obj;
    (void)info;
    return -1;
}

int ct_sprite_frame(int obj, uint8_t *rgba, int max, ct_sprite_info *info)
{
    (void)obj;
    (void)rgba;
    (void)max;
    (void)info;
    return -1;
}

int ct_game_scene(ct_scene *out);

static void nmi(const uint8_t vector[4])
{
    memset(wram, 0, sizeof wram);
    memcpy(wram + 0x500, vector, 4);
}

int main(void)
{
    static const uint8_t mode7_cutscene[][4] = {
        {0x5C, 0x48, 0x05, 0xC3}, {0x5C, 0x29, 0x05, 0xC3}, {0x5C, 0xC9, 0x08, 0xC3},
    };
    static const uint8_t field[] = {0x5C, 0x63, 0xEA, 0xC0};
    static const uint8_t ui[] = {0x5C, 0x0E, 0x84, 0xC2};
    ct_scene s = {0};

    for (size_t k = 0; k < sizeof mode7_cutscene / sizeof *mode7_cutscene; k++) {
        nmi(mode7_cutscene[k]);
        assert(ct_game_scene(&s) == 0);
        assert(s.mode == CT_SCENE_OTHER);
        assert((s.flags & (CT_SCENE_SCRIPTED | CT_SCENE_EXACT_2D)) ==
               (CT_SCENE_SCRIPTED | CT_SCENE_EXACT_2D));
    }

    nmi(ui);
    s.flags = 0xFF;
    assert(ct_game_scene(&s) == 0);
    assert(s.mode == CT_SCENE_UI && s.flags == 0);

    nmi(field);
    s.wrap = 1;   /* a reused world scene must not leak wrapping into a field */
    wram[0x0BCB] = wram[0x0BCD] = 1;
    wram[0x7080] = 0x80;
    assert(ct_game_scene(&s) == 0);
    assert(s.mode == CT_SCENE_FIELD && !(s.flags & CT_SCENE_SCRIPTED) && !s.wrap);

    wram[0x0138] = 1;
    assert(ct_game_scene(&s) == 0);
    assert(s.mode == CT_SCENE_FIELD && (s.flags & CT_SCENE_SCRIPTED));
    assert(!(s.flags & CT_SCENE_EXACT_2D));

    puts("cutscene scene: ok");
    return 0;
}
