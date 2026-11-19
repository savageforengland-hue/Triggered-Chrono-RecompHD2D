/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port: HD text: the game's text boxes drawn with a TrueType / OpenType
 * font of the player's (settings.ini hd_font=PATH), at the screen's
 * resolution, over the console's own glyphs, which are taken out of the
 * picture. The game module says which characters it has drawn into each
 * line of a box and where those lines' 8x8 tiles are in VRAM
 * (module.h ct_text); the provenance G-buffer (sched_prov) says where on
 * the screen those tiles are this frame. So a box anywhere, scrolled or
 * half off the screen, carries its text, and a line that isn't on the
 * screen isn't drawn.
 *
 * Each word starts where the console starts it; within a word the font's
 * own advances are used, narrowed if the word would come out wider than
 * the console's. A character the font lacks (or the game's encoding
 * doesn't name) keeps the console's glyph. The text takes the colors the
 * console gives it (CGRAM) and the screen's brightness.
 *
 * No SDL, no GL: the renderer draws the overlay (hd2d_set_text_overlay).
 * The project ships no font; stb_truetype (third_party/stb) reads the
 * player's. */
#ifndef CT_HDTEXT_H
#define CT_HDTEXT_H

#include <stddef.h>
#include <stdint.h>

#include "module.h"
#include "sched.h"

enum { HDTEXT_SHADOW = 0, HDTEXT_OUTLINE = 1, HDTEXT_PLAIN = 2 };

/* Load the font at path (a .ttf, .otf or .ttc's first face). Returns 0,
   or -1 with the reason in err. NULL or "": no font (HD text off). */
int hdtext_load_font(const char *path, char *err, size_t errlen);
int hdtext_ready(void);            /* a font is loaded */
const char *hdtext_font_path(void);
/* Size: percent of the console font's (50-200, default 100); style:
   HDTEXT_*. */
void hdtext_set_size(int percent);
void hdtext_set_style(int style);

typedef struct {
    /* The picture with the console's glyphs taken out where the overlay
       draws them (this module's buffers), or the inputs when nothing is. */
    const uint8_t *bgrx;           /* SCHED_WIDTH x SCHED_HEIGHT, B G R x */
    const uint8_t *layers;         /* as sched_layers */
    /* The overlay: rgba w x h texels, premultiplied, covering console
       pixels x, y, cw, ch; NULL: none this frame. gen changes when the
       pixels do. */
    const uint8_t *rgba;
    int w, h;
    float x, y, cw, ch;
    unsigned gen;
    int lines;                     /* lines drawn */
} hdtext_result;

/* One frame: text t (ct_text_capture), the frame's provenance and video
   memory (NULL: no HD text this frame), picture and layers; brightness
   (0-15): used only when the console's glyph pixels can't tell (they are
   compared with their CGRAM colors: fades come out right however the
   game makes them); scale: overlay texels per console pixel (the
   screen's scale, 1-16). Returns the number of lines drawn. */
int hdtext_frame(const ct_text *t, const sched_prov_px *prov, const sched_vmem *m,
                 int brightness, const uint8_t *bgrx, const uint8_t *layers, int scale,
                 hdtext_result *r);

/* port: the pixels the last hdtext_frame took the console's glyphs out
   of (SCHED_WIDTH x SCHED_HEIGHT, nonzero), or NULL if it drew none. */
const uint8_t *hdtext_removed(void);

#endif
