/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Game module loader (module.h). */
#include "module.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "ppu.h"
#include "snes_adapter.h"

static ct_module_scene_fn scene_fn;
static ct_module_pose_fn pose_fn;

void ct_pose_set_source(ct_module_pose_fn fn) { pose_fn = fn; }
int ct_pose_available(void) { return pose_fn != NULL; }

int ct_scene_pose_of(int obj, ct_scene_pose *out, uint8_t *rgba, int max)
{
    memset(out, 0, sizeof *out);
    if (!pose_fn || pose_fn(obj, out, rgba, max) || !out->key || !out->w || !out->h)
        return -1;
    out->name[sizeof out->name - 1] = 0;
    return 0;
}

void ct_scene_set_source(ct_module_scene_fn fn) { scene_fn = fn; }
int ct_scene_available(void) { return scene_fn != NULL; }

int ct_scene_capture(ct_scene *out)
{
    out->mode = CT_SCENE_OTHER;
    out->flags = 0;
    if (!scene_fn)
        return -1;
    /* The console's side first, as the PPU holds it now (OBSEL, INIDISP),
       so the module can match its objects to OAM entries. */
    static const uint8_t sizes[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32},
                                        {16, 64}, {32, 64}, {16, 32}, {16, 32}};
    const Ppu *p = snes_hw_ppu();
    out->vram = p->vram;
    out->cgram = p->cgram;
    out->oam = p->oam;
    out->obj_chr[0] = p->objTileAdr1;
    out->obj_chr[1] = p->objTileAdr2;
    out->obj_size[0] = sizes[p->objSize & 7][0];
    out->obj_size[1] = sizes[p->objSize & 7][1];
    out->brightness = p->forcedBlank ? 0 : p->brightness;
    if (scene_fn(out)) {
        out->mode = CT_SCENE_OTHER;
        out->flags = 0;
        return -1;
    }
    return 0;
}

/* ---- music and text (ABI 9) ---- */

static const ct_music_hooks *music;
static ct_module_text_fn text_fn;

void ct_music_set_source(const ct_music_hooks *hooks)
{
    music = hooks;
    if (!hooks)
        snes_apu_port_hook = NULL;
}
const ct_music_hooks *ct_music_source(void) { return music; }
void ct_music_watch(int on)
{
    snes_apu_port_hook = on && music ? music->port_write : NULL;
}
int ct_music_events(ct_music_event *out, int max)
{
    return music && music->events ? music->events(out, max) : -1;
}

void ct_text_set_source(ct_module_text_fn fn) { text_fn = fn; }
int ct_text_available(void) { return text_fn != NULL; }
int ct_text_capture(ct_text *out)
{
    out->n_lines = 0;
    if (!text_fn || text_fn(out)) {
        out->n_lines = 0;
        return -1;
    }
    return 0;
}

int ct_module_load(const char *path, const char **err)
{
    static char msg[512];
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(msg, sizeof msg, "%s", dlerror());
        *err = msg;
        return -1;
    }
    void *sym = dlsym(h, "ct_module_get");
    ct_module_get_fn get;
    memcpy(&get, &sym, sizeof get);   /* object -> function pointer */
    if (!get) {
        snprintf(msg, sizeof msg, "%s: no ct_module_get (not a game module)", path);
        *err = msg;
        dlclose(h);
        return -1;
    }
    const ct_module_info *m = get();
    if (!m || m->abi != CT_MODULE_ABI || m->cpu_size != sizeof(CPU) ||
        m->func_size != sizeof(ct_func) || m->overlay_size != sizeof(ct_overlay_func) ||
        m->scene_size != sizeof(ct_scene) || m->music_size != sizeof(ct_music_hooks) ||
        m->text_size != sizeof(ct_text)) {
        snprintf(msg, sizeof msg, "%s: built for a different engine (module ABI %u, engine %u)",
                 path, m ? m->abi : 0, CT_MODULE_ABI);
        *err = msg;
        dlclose(h);
        return -1;
    }
    ct_tables_install(&m->tables);   /* the module stays loaded for good */
    ct_scene_set_source(m->scene);
    ct_rumble_set_source(m->rumble);
    ct_music_set_source(m->music);
    ct_text_set_source(m->text);
    /* Optional, outside ct_module_info: poses (a module without them is fine). */
    sym = dlsym(h, "ct_module_pose");
    ct_module_pose_fn pose = NULL;
    memcpy(&pose, &sym, sizeof pose);
    ct_pose_set_source(pose);
    return 0;
}
