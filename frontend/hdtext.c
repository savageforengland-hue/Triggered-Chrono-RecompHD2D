/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port: HD text (hdtext.h). */
#include "hdtext.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"   /* the implementation is in hdtext_ttf.c */

#define W SCHED_WIDTH
#define H SCHED_HEIGHT
#define LINE_PX (CT_TEXT_COLS * 8)

static struct {
    uint8_t *data;
    stbtt_fontinfo info;
    int ok;
    float cap;                 /* cap height, font units */
    unsigned gen;              /* bumped by each load */
    char path[4096];
} F;
static int size_pct = 100, style = HDTEXT_SHADOW;

int hdtext_load_font(const char *path, char *err, size_t errlen)
{
    free(F.data);
    unsigned gen = F.gen + 1;
    memset(&F, 0, sizeof F);
    F.gen = gen;
    if (!path || !*path)
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "%s: cannot open", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64L << 20) {
        fclose(f);
        snprintf(err, errlen, "%s: not a font (size %ld)", path, n);
        return -1;
    }
    F.data = malloc((size_t)n);
    if (!F.data || fread(F.data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(F.data);
        F.data = NULL;
        snprintf(err, errlen, "%s: cannot read", path);
        return -1;
    }
    fclose(f);
    int off = stbtt_GetFontOffsetForIndex(F.data, 0);
    if (off < 0 || !stbtt_InitFont(&F.info, F.data, off)) {
        free(F.data);
        F.data = NULL;
        snprintf(err, errlen, "%s: not a TrueType or OpenType font", path);
        return -1;
    }
    int x0, y0, x1, y1, asc, desc, gap;
    stbtt_GetFontVMetrics(&F.info, &asc, &desc, &gap);
    if (stbtt_FindGlyphIndex(&F.info, 'H') && stbtt_GetCodepointBox(&F.info, 'H', &x0, &y0, &x1, &y1) &&
        y1 > y0)
        F.cap = (float)(y1 > 0 ? y1 : y1 - y0);
    else
        F.cap = 0.7f * (float)asc;
    if (F.cap <= 0) {
        free(F.data);
        F.data = NULL;
        snprintf(err, errlen, "%s: the font has no usable metrics", path);
        return -1;
    }
    snprintf(F.path, sizeof F.path, "%s", path);
    F.ok = 1;
    return 0;
}

int hdtext_ready(void) { return F.ok; }
const char *hdtext_font_path(void) { return F.ok ? F.path : NULL; }
void hdtext_set_size(int percent) { size_pct = percent < 50 ? 50 : percent > 200 ? 200 : percent; }
void hdtext_set_style(int s) { style = s >= 0 && s <= HDTEXT_PLAIN ? s : HDTEXT_SHADOW; }

/* ---- the frame ---- */

static uint16_t tile_of[32768];   /* VRAM word -> 1 + line * 64 + row * 32 + col */
static uint16_t tiles_set[CT_TEXT_LINES * CT_TEXT_ROWS * CT_TEXT_COLS];
static int n_tiles_set;
static uint8_t mask[W * H];       /* 1 + line: a console glyph pixel of a line drawn here */
static uint8_t glyph_px[W * H];   /* port: any console glyph pixel, kept ones too */
static uint8_t keep[CT_TEXT_LINES][LINE_PX + 16];   /* columns whose console glyph stays */
static uint8_t out_px[W * H * 4], out_layers[W * H];
static uint8_t *ov;
static size_t ov_cap;
static unsigned ov_gen;
static uint64_t ov_hash;
static int ov_w, ov_h, ov_x0, ov_y0, ov_x1, ov_y1;

typedef struct {
    int found;
    int x, y;                 /* the line's top-left on the screen */
    int pal;                  /* CGRAM index of its palette's color 0 */
} line_place;

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t k = 0; k < n; k++)
        h = (h ^ b[k]) * 0x100000001B3ull;
    return h;
}

static int drawable(uint32_t ch)
{
    return ch && ch != ' ' && stbtt_FindGlyphIndex(&F.info, (int)ch) != 0;
}

/* Coverage over the overlay, premultiplied "over": color c (r g b), alpha a. */
static void blend(int x, int y, float a, const float c[3])
{
    if (x < 0 || y < 0 || x >= ov_w || y >= ov_h || a <= 0.0f)
        return;
    uint8_t *d = ov + ((size_t)y * ov_w + x) * 4;
    float k = 1.0f - a;
    for (int i = 0; i < 3; i++)
        d[i] = (uint8_t)lrintf(c[i] * a * 255.0f + d[i] * k);
    d[3] = (uint8_t)lrintf(a * 255.0f + d[3] * k);
}

static uint8_t glyph_buf[512 * 512];

/* Glyph ch with its origin (pen, baseline) at overlay texel (px, by), x
   scale sx, y scale sy, offset by (dx, dy) texels, in color c. */
static void draw_glyph(uint32_t ch, float px, int by, float sx, float sy, int dx, int dy,
                       const float c[3])
{
    int ix0, iy0, ix1, iy1;
    float shift = px - floorf(px);
    stbtt_GetCodepointBitmapBoxSubpixel(&F.info, (int)ch, sx, sy, shift, 0.0f, &ix0, &iy0, &ix1, &iy1);
    int w = ix1 - ix0, h = iy1 - iy0;
    if (w <= 0 || h <= 0 || w > 512 || h > 512)
        return;
    stbtt_MakeCodepointBitmapSubpixel(&F.info, glyph_buf, w, h, w, sx, sy, shift, 0.0f, (int)ch);
    int ox = (int)floorf(px) + ix0 + dx, oy = by + iy0 + dy;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (glyph_buf[y * w + x])
                blend(ox + x, oy + y, glyph_buf[y * w + x] / 255.0f, c);
}

static void rgb_of(const sched_vmem *m, int index, float bright, float out[3])
{
    unsigned v = m->cgram[index & 0xFF];
    out[0] = (float)(v & 31) / 31.0f * bright;
    out[1] = (float)((v >> 5) & 31) / 31.0f * bright;
    out[2] = (float)((v >> 10) & 31) / 31.0f * bright;
}

typedef struct {
    uint32_t ch;
    float x;                  /* pen, overlay texels */
    float sx;                 /* x scale */
} placed;

/* Lay out and draw line l (on the screen at p) into the overlay. */
static void draw_line(const ct_text_line *ln, const line_place *p, const sched_vmem *m,
                      const ct_text *t, float bright, int S)
{
    static placed pl[CT_TEXT_GLYPHS];
    int np = 0;
    float sc = (8.0f * (float)S * (float)size_pct / 100.0f) / F.cap;
    for (int a = 0; a < ln->n;) {
        if (ln->g[a].ch == ' ' || !ln->g[a].ch) {
            a++;
            continue;
        }
        int b = a;   /* a word: a..b */
        while (b + 1 < ln->n && ln->g[b + 1].ch != ' ' && ln->g[b + 1].ch)
            b++;
        float game_w = (float)(ln->g[b].x + ln->g[b].w - ln->g[a].x) * (float)S, hd_w = 0.0f;
        for (int k = a; k <= b; k++) {
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&F.info, (int)ln->g[k].ch, &adv, &lsb);
            hd_w += (float)adv * sc;
            if (k < b)
                hd_w += (float)stbtt_GetCodepointKernAdvance(&F.info, (int)ln->g[k].ch,
                                                             (int)ln->g[k + 1].ch) * sc;
        }
        float f = hd_w > game_w && hd_w > 0.0f ? game_w / hd_w : 1.0f;
        float pen = (float)(p->x + ln->g[a].x - ov_x0) * (float)S;
        for (int k = a; k <= b; k++) {
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&F.info, (int)ln->g[k].ch, &adv, &lsb);
            if (drawable(ln->g[k].ch))
                pl[np++] = (placed){ln->g[k].ch, pen, sc * f};
            pen += (float)adv * sc * f;
            if (k < b)
                pen += (float)stbtt_GetCodepointKernAdvance(&F.info, (int)ln->g[k].ch,
                                                            (int)ln->g[k + 1].ch) * sc * f;
        }
        a = b + 1;
    }
    int by = (p->y + ln->top + 8 - ov_y0) * S;   /* the console font's baseline */
    float fg[3], sh[3];
    rgb_of(m, p->pal + t->fg, bright, fg);
    rgb_of(m, p->pal + t->shadow, bright, sh);
    if (style == HDTEXT_SHADOW) {
        int d = S / 2 > 1 ? S / 2 : 1;
        for (int k = 0; k < np; k++)
            draw_glyph(pl[k].ch, pl[k].x, by, pl[k].sx, sc, d, d, sh);
    } else if (style == HDTEXT_OUTLINE) {
        int r = (int)lrintf((float)S * 0.4f);
        r = r < 1 ? 1 : r;
        for (int k = 0; k < np; k++)
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++)
                    if ((dx || dy) && dx * dx + dy * dy <= r * r + r)
                        draw_glyph(pl[k].ch, pl[k].x, by, pl[k].sx, sc, dx, dy, sh);
    }
    for (int k = 0; k < np; k++)
        draw_glyph(pl[k].ch, pl[k].x, by, pl[k].sx, sc, 0, 0, fg);
}

int hdtext_frame(const ct_text *t, const sched_prov_px *prov, const sched_vmem *m,
                 int brightness, const uint8_t *bgrx, const uint8_t *layers, int S,
                 hdtext_result *r)
{
    memset(r, 0, sizeof *r);
    r->bgrx = bgrx;
    r->layers = layers;
    if (!F.ok || !t || !prov || !m || !t->n_lines)
        return 0;
    S = S < 1 ? 1 : S > 16 ? 16 : S;
    int nl = t->n_lines < CT_TEXT_LINES ? t->n_lines : CT_TEXT_LINES;

    /* Which VRAM characters are which line's tiles (lines with glyphs). */
    for (int k = 0; k < n_tiles_set; k++)
        tile_of[tiles_set[k]] = 0;
    n_tiles_set = 0;
    for (int l = 0; l < nl; l++) {
        const ct_text_line *ln = &t->line[l];
        if (!ln->n)
            continue;
        for (int row = 0; row < ln->rows && row < CT_TEXT_ROWS; row++)
            for (int col = 0; col < ln->cols && col < CT_TEXT_COLS; col++) {
                unsigned c = ln->chr[row][col] & 0x7FFF;
                if (!tile_of[c]) {
                    tile_of[c] = (uint16_t)(1 + l * 64 + row * 32 + col);
                    tiles_set[n_tiles_set++] = (uint16_t)c;
                }
            }
    }
    if (!n_tiles_set)
        return 0;

    /* Where the lines are on the screen, and their console glyphs' pixels. */
    line_place place[CT_TEXT_LINES];
    memset(place, 0, sizeof place);
    memset(mask, 0, sizeof mask);
    /* The screen's brightness, from the console's own text pixels: what
       it drew against the CGRAM color (fades included, however made). */
    double lit = 0.0, ref = 0.0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const sched_prov_px *p = &prov[y * W + x];
            if ((p->flags & SCHED_PROV_OBJ) || !(p->flags & SCHED_PROV_BPP) ||
                (p->flags & (SCHED_PROV_HFLIP | SCHED_PROV_VFLIP)) || p->index != t->layer)
                continue;
            unsigned e = tile_of[p->chr & 0x7FFF];
            if (!e)
                continue;
            e--;
            int l = (int)(e / 64), row = (int)(e % 64) / 32, col = (int)(e % 32);
            if (!place[l].found) {
                place[l].found = 1;
                place[l].x = x - p->u - 8 * col;
                place[l].y = y - p->v - 8 * row;
                place[l].pal = p->pal;
            }
            mask[y * W + x] = (uint8_t)(l + 1);
            if (p->cidx == ((p->pal + t->fg) & 0xFF)) {
                unsigned c = m->cgram[p->cidx];
                unsigned cm = c & 31;
                cm = ((c >> 5) & 31) > cm ? (c >> 5) & 31 : cm;
                cm = ((c >> 10) & 31) > cm ? (c >> 10) & 31 : cm;
                const uint8_t *f = bgrx + (size_t)(y * W + x) * 4;
                unsigned fm = f[0] > f[1] ? f[0] : f[1];
                fm = f[2] > fm ? f[2] : fm;
                lit += fm;
                ref += cm * 255.0 / 31.0;
            }
        }
    double seen = ref > 0.0 ? lit / ref : -1.0;
    if (seen >= 0.0)
        brightness = (int)lrint((seen > 1.0 ? 1.0 : seen) * 15.0);
    else if (brightness <= 0)
        brightness = 15;

    /* The overlay's extent, the characters the font draws, and the
       console glyphs that stay (characters it can't). */
    int any = 0;
    int x0 = W, y0 = H, x1 = 0, y1 = 0;
    uint64_t h = fnv(0xCBF29CE484222325ull, &F.gen, sizeof F.gen);
    int params[4] = {S, size_pct, style, brightness};
    h = fnv(h, params, sizeof params);
    memset(keep, 0, sizeof keep);
    for (int l = 0; l < nl; l++) {
        const ct_text_line *ln = &t->line[l];
        if (!place[l].found || !ln->n)
            continue;
        any = 1;
        int lx0 = place[l].x, ly0 = place[l].y;
        int lx1 = lx0 + ln->cols * 8, ly1 = ly0 + ln->rows * 8 + 2;
        x0 = lx0 < x0 ? lx0 : x0;
        y0 = ly0 < y0 ? ly0 : y0;
        x1 = lx1 > x1 ? lx1 : x1;
        y1 = ly1 > y1 ? ly1 : y1;
        for (int k = 0; k < ln->n; k++)
            if (ln->g[k].ch != ' ' && !drawable(ln->g[k].ch))
                for (int c = ln->g[k].x; c <= ln->g[k].x + ln->g[k].w + 1 && c < LINE_PX + 16; c++)
                    if (c >= 0)
                        keep[l][c] = 1;
        h = fnv(h, &place[l], sizeof place[l]);
        h = fnv(h, &l, sizeof l);
        h = fnv(h, ln->g, (size_t)ln->n * sizeof ln->g[0]);
        uint16_t colors[2] = {m->cgram[(place[l].pal + t->fg) & 0xFF],
                              m->cgram[(place[l].pal + t->shadow) & 0xFF]};
        h = fnv(h, colors, sizeof colors);
    }
    if (!any)
        return 0;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > W ? W : x1;
    y1 = y1 > H ? H : y1;
    if (x1 <= x0 || y1 <= y0)
        return 0;

    /* The console's glyphs out of the picture: each such pixel takes the
       box's own color from the nearest pixels around it that aren't text:
       the two nearest on its row or its column, whichever are closer (a
       word's shadowed top row can be a long unbroken run). */
    memcpy(out_px, bgrx, sizeof out_px);
    memcpy(glyph_px, mask, sizeof glyph_px);
    memcpy(out_layers, layers, sizeof out_layers);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int i = y * W + x, l = mask[i] - 1;
            if (l < 0)
                continue;
            int lc = x - place[l].x;
            if (lc >= 0 && lc < LINE_PX + 16 && keep[l][lc])
                mask[i] = 0;
        }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int i = y * W + x;
            if (!mask[i])
                continue;
            int a = -1, b = -1, da = 0, db = 0, c = -1, e = -1, dc = 0, de = 0;
            for (int d = 1; d < W && (a < 0 || b < 0); d++) {
                if (a < 0 && x - d >= 0 && !mask[i - d])
                    a = i - d, da = d;
                if (b < 0 && x + d < W && !mask[i + d])
                    b = i + d, db = d;
            }
            for (int d = 1; d < 24 && (c < 0 || e < 0); d++) {
                if (c < 0 && y - d >= 0 && !mask[i - d * W])
                    c = i - d * W, dc = d;
                if (e < 0 && y + d < H && !mask[i + d * W])
                    e = i + d * W, de = d;
            }
            int row = (a >= 0 ? da : 999) + (b >= 0 ? db : 999);
            int col = (c >= 0 ? dc : 999) + (e >= 0 ? de : 999);
            if (col < row)
                a = c, b = e;
            if (a < 0 && b < 0)
                continue;
            if (a < 0)
                a = b;
            if (b < 0)
                b = a;
            for (int c = 0; c < 3; c++)
                out_px[i * 4 + c] = (uint8_t)((bgrx[a * 4 + c] + bgrx[b * 4 + c] + 1) / 2);
            out_layers[i] = layers[a];
            /* port: better, the same spot in a tile beside it on the row (8,
               16.. pixels away) when it is box, not frame (near that average):
               the box's dithered pattern carries on instead of a flat fill,
               which showed as pale outlines of the old words. */
            for (int d = 8; d <= 64; d += 8) {
                int cand[2] = {x + d < x1 ? i + d : -1, x - d >= x0 ? i - d : -1}, found = 0;
                for (int q = 0; q < 2 && !found; q++) {
                    int j = cand[q];
                    if (j < 0 || glyph_px[j])
                        continue;
                    int diff = 0;
                    for (int c = 0; c < 3; c++)
                        diff += abs((int)bgrx[j * 4 + c] - (int)out_px[i * 4 + c]);
                    if (diff < 120) {
                        memcpy(&out_px[i * 4], &bgrx[j * 4], 3);
                        out_layers[i] = layers[j];
                        found = 1;
                    }
                }
                if (found)
                    break;
            }
        }
    r->bgrx = out_px;
    r->layers = out_layers;

    /* The overlay: drawn again only when something in it changed. */
    int lines = 0;
    for (int l = 0; l < nl; l++)
        lines += place[l].found && t->line[l].n;
    h = fnv(h, (int[4]){x0, y0, x1, y1}, 4 * sizeof(int));
    if (!ov || h != ov_hash) {
        ov_x0 = x0, ov_y0 = y0, ov_x1 = x1, ov_y1 = y1;
        ov_w = (x1 - x0) * S;
        ov_h = (y1 - y0) * S;
        size_t need = (size_t)ov_w * ov_h * 4;
        if (need > ov_cap) {
            uint8_t *n = realloc(ov, need);
            if (!n)
                return 0;
            ov = n;
            ov_cap = need;
        }
        memset(ov, 0, need);
        float bright = (float)(brightness < 0 ? 0 : brightness > 15 ? 15 : brightness) / 15.0f;
        for (int l = 0; l < nl; l++)
            if (place[l].found && t->line[l].n)
                draw_line(&t->line[l], &place[l], m, t, bright, S);
        ov_hash = h;
        ov_gen++;
    }
    r->rgba = ov;
    r->w = ov_w;
    r->h = ov_h;
    r->x = (float)ov_x0;
    r->y = (float)ov_y0;
    r->cw = (float)(ov_x1 - ov_x0);
    r->ch = (float)(ov_y1 - ov_y0);
    r->gen = ov_gen;
    r->lines = lines;
    return lines;
}
