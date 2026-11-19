// SPDX-FileCopyrightText: 2026 jerbanpls
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 3D diorama's surfaces. Each quad names an 8x8 tile in VRAM (address,
// palette, depth: scene3d.h's packing), decoded here from the console's
// own VRAM and CGRAM, so animated tiles and palette changes show as they
// happen. Color 0 is transparent (cut-outs, sprites) except on the back
// layer's floor, where it is the backdrop color, as on the console.
//
// Lighting keeps the original's colors where the sun falls on the floor:
// everything is lit relative to that (a south wall a little darker, the
// shaded side of a box more), with the key light's shadow map, the 2D
// path's point-light map looked up where the console drew the pixel, and
// haze with distance. Outputs the lit color with the depth of field's
// depth in alpha (0.5 at the focus), and the material (as light.frag's,
// with the surface's normal in alpha for the ambient occlusion).
//
// With WORLD_MAP defined (a program of its own, so the field's stays as
// it was), the world map's (scene3d_world.c): ground whose normal comes
// from its height map (S3D_N_TERRAIN), water with a shimmer
// (S3D_N_WATER), entrance marks (S3D_N_WATER on a sprite), sprites flat
// over the picture (S3D_OBJ | S3D_OVERHEAD: captions), and the overlay
// layer (clouds), drawn as a plane (uPassKind 1) and as the soft shadows
// it casts.
//
// With MODEL defined: an asset pack's model (hd2d.h hd2d_models), lit
// like the rest, with its own normals, its texture and colors.
//
// With SHADOW_PASS defined: only the cut-out test, for the shadow map.
#ifdef GL_ES
precision highp sampler2DShadow;
#endif
uniform usampler2D uVram;        // 256 x 128 words
uniform sampler2D uCgram;        // 256 x 1, RGB
in vec2 vUV;
in vec3 vWorld;
in vec4 vShadow;
flat in uint vTile;

uint vramAt(uint a) { return texelFetch(uVram, ivec2(int(a & 255u), int(a >> 8u)), 0).r; }

// The tile's color index (0-15) at pixel p of it.
int colorIndex(uint tile, ivec2 p)
{
    uint addr = tile & 0x7FFFu;
    uint w0 = vramAt((addr + uint(p.y)) & 0x7FFFu);
    int sh = 7 - p.x;
    int c = int((w0 >> uint(sh)) & 1u) | int((w0 >> uint(sh + 8)) & 1u) << 1;
    if ((tile & (1u << 18)) == 0u) {   // 4 bpp: planes 2 and 3 eight words on
        uint w1 = vramAt((addr + 8u + uint(p.y)) & 0x7FFFu);
        c |= int((w1 >> uint(sh)) & 1u) << 2 | int((w1 >> uint(sh + 8)) & 1u) << 3;
    }
    return c;
}

// THE TILE LOOKUP: what a quad's tile looks like at uv (0-8 px, flips
// applied): rgb, and a = 0 where it is transparent. Everything 3D samples
// its texture through here, keyed by the tile (VRAM address, palette,
// depth, BG or sprite: scene3d.h's packing). An asset pack replaces this
// function (or branches in it on its own flag) to return its own texture
// for a (tile, palette); the console's pixels are the default.
uniform usampler2D uPackRects;   // per entry: (page, x, y, w), (h, src w, src h, 0)
uniform highp sampler2DArray uPackAtlas;
#ifndef SHADOW_PASS
// Asset packs (docs/ASSET-PACKS.md): BG tiles with a replacement. The
// lookup is by (VRAM address / 8, palette, + 8 for sprites), filled each
// frame for the tiles the meshes show; 0: none, else the atlas entry + 1.
uniform int uPack;
uniform usampler2D uPackLut;
#endif

vec4 tileTexel(uint tile, vec2 uv)
{
#ifndef SHADOW_PASS
    // Derivatives here, where the control flow is still uniform.
    vec2 duvx = dFdx(uv), duvy = dFdy(uv);
#endif
    if ((tile & (1u << 26)) != 0u) {   // S3D_PACKED: a whole picture (a pose), uv 0-8 across it
        int i = int(tile & 0x3FFFFu);
        ivec2 rc = ivec2((i % 1024) * 2, i / 1024);
        uvec4 r0 = texelFetch(uPackRects, rc, 0);
        uvec4 r1 = texelFetch(uPackRects, rc + ivec2(1, 0), 0);
        vec2 rect = vec2(float(r0.w), float(r1.x));
        float asize = float(textureSize(uPackAtlas, 0).x);
        vec2 t = clamp(uv / 8.0 * rect, vec2(0.5), rect - 0.5);
        vec3 at = vec3((vec2(float(r0.y), float(r0.z)) + t) / asize, float(r0.x));
#ifdef SHADOW_PASS
        return textureLod(uPackAtlas, at, 0.0);
#else
        vec2 s = rect / 8.0 / asize;
        return textureGrad(uPackAtlas, at, duvx * s, duvy * s);
#endif
    }
#ifndef SHADOW_PASS
    if (uPack != 0) {
        int row = int((tile >> 15u) & 7u) + ((tile & (1u << 19)) != 0u ? 8 : 0);
        uint e = texelFetch(uPackLut, ivec2(int((tile & 0x7FFFu) >> 3u), row), 0).r;
        if (e != 0u) {
            int i = int(e) - 1;
            ivec2 rc = ivec2((i % 1024) * 2, i / 1024);
            uvec4 r0 = texelFetch(uPackRects, rc, 0);
            uvec4 r1 = texelFetch(uPackRects, rc + ivec2(1, 0), 0);
            vec2 rect = vec2(float(r0.w), float(r1.x));
            float asize = float(textureSize(uPackAtlas, 0).x);
            vec2 t = clamp(uv / 8.0 * rect, vec2(0.5), rect - 0.5);
            vec2 s = rect / 8.0 / asize;
            vec4 rep = textureGrad(uPackAtlas, vec3((vec2(float(r0.y), float(r0.z)) + t) / asize,
                                                    float(r0.x)), duvx * s, duvy * s);
            if (rep.a < 0.5 && (tile & (1u << 23)) != 0u)   // the back floor: the backdrop
                return vec4(texelFetch(uCgram, ivec2(0, 0), 0).rgb, 1.0);
            return rep;
        }
    }
#endif
    ivec2 p = clamp(ivec2(floor(uv)), ivec2(0), ivec2(7));
    int c = colorIndex(tile, p);
    bool opaque = (tile & (1u << 23)) != 0u;   // the back floor: color 0 is the backdrop
    if (c == 0 && !opaque)
        return vec4(0.0);
#ifdef SHADOW_PASS
    return vec4(1.0);
#else
    bool obj = (tile & (1u << 19)) != 0u;
    int pal = int((tile >> 15u) & 7u);
    int idx = c == 0 ? 0 : (obj ? 128 : 0) + pal * ((tile & (1u << 18)) != 0u ? 4 : 16) + c;
    return vec4(texelFetch(uCgram, ivec2(idx, 0), 0).rgb, 1.0);
#endif
}

#ifdef MODEL
uniform sampler2D uModelTex;
uniform vec4 uModelColor;        // the part's color
uniform ivec2 uModelMode;        // texture on, cut-out
in vec3 vNrm;
in vec4 vCol;

vec4 modelTexel()
{
    vec4 c = uModelColor * vCol;
    if (uModelMode.x != 0)
        c *= texture(uModelTex, vUV);
    return vec4(c.rgb, uModelMode.y != 0 ? c.a : 1.0);
}
#endif

#ifdef SHADOW_PASS
void main()
{
#ifdef MODEL
    if (modelTexel().a < 0.5)
        discard;
    return;
#endif
#ifdef WORLD_MAP
    uint ni = (vTile >> 20u) & 7u;
    bool flat_ = (vTile & (1u << 19)) != 0u && (vTile & (1u << 25)) != 0u;
    if (ni == 7u || flat_)
        discard;
#endif
    if (tileTexel(vTile, vUV).a < 0.5)
        discard;
}
#else
#ifdef WORLD_MAP
// The world map's: normals per tile corner (rgb), wrapping.
uniform sampler2D uWorldNrm;
// The overlay layer (BG tilemap words, its tiles at uOverlayChr, 2 bpp):
// scroll (px), size (px); uCloud: height, opacity, shadow amount (0: none).
uniform usampler2D uOverlayMap;
uniform int uOverlayChr;
uniform int uCloudPal;           // port: the clouds' palette (-1: any)
uniform vec4 uOverlay;
uniform vec4 uCloud;
uniform vec2 uCloudClear;        // where it thins out (over the player)
uniform float uTime;             // seconds
uniform int uPassKind;           // 0: surfaces, 1: the overlay plane

vec4 overlayTexel(vec2 p)
{
    vec2 q = mod(p + uOverlay.xy, uOverlay.zw);
    ivec2 t = ivec2(floor(q / 8.0));
    uint e = texelFetch(uOverlayMap, t, 0).r;
    if (uCloudPal >= 0 && int((e >> 10u) & 7u) != uCloudPal)
        return vec4(0.0);   // not a cloud: a caption's box (port)
    uint tile = ((uint(uOverlayChr) + (e & 0x3FFu) * 8u) & 0x7FFFu) | ((e >> 10u) & 7u) << 15u |
                (1u << 18u);
    vec2 uv = q - vec2(t) * 8.0;
    if ((e & 0x4000u) != 0u)
        uv.x = 8.0 - uv.x;
    if ((e & 0x8000u) != 0u)
        uv.y = 8.0 - uv.y;
    return tileTexel(tile, uv);
}
#endif

#ifndef SHADOW_PASS
uniform int uSmooth;             // pixel-art smoothing in the 3D view
// Four taps across the pixel's footprint: oblique and distant tiles average out instead of
// flickering between texels, and a magnified texel edge gets a one-pixel soft step.
vec4 tileTexelAA(uint tile, vec2 uv)
{
    vec2 dx = dFdx(uv) * 0.3, dy = dFdy(uv) * 0.3;
    vec2 fw = max((abs(dx) + abs(dy)) / 0.3, vec2(1e-4));   // texels per pixel
    // Only where a screen pixel spans over half a texel (far, or seen at a
    // slant): close up one texel covers several pixels and a single read is
    // already even, and the four reads cost most of the frame.
    float span = max(dot(dx, dx), dot(dy, dy));   // (0.3 * texels per pixel)^2
    if (uSmooth == 0 || (tile & (1u << 26)) != 0u)   // packed pictures: filtered already
        return tileTexel(tile, uv);
    if (span < 0.0225) {
        // port: magnified (close up, most of the screen): "sharp bilinear",
        // each texel a flat square with a one-pixel soft edge to the next,
        // so every texel comes out the same size on the screen instead of
        // some rows and columns a pixel wider (uneven, blocky sprites).
        vec2 p = uv - 0.5, i = floor(p), f = p - i;
        vec2 w = clamp((f - 0.5) / fw + 0.5, 0.0, 1.0);
        if ((w.x == 0.0 || w.x == 1.0) && (w.y == 0.0 || w.y == 1.0))
            return tileTexel(tile, i + 0.5 + w);
        vec4 a = tileTexel(tile, i + vec2(0.5, 0.5)), b = tileTexel(tile, i + vec2(1.5, 0.5));
        vec4 c = tileTexel(tile, i + vec2(0.5, 1.5)), d = tileTexel(tile, i + vec2(1.5, 1.5));
        float wa = (1.0 - w.x) * (1.0 - w.y) * a.a, wb = w.x * (1.0 - w.y) * b.a;
        float wc = (1.0 - w.x) * w.y * c.a, wd = w.x * w.y * d.a;
        float al = (1.0 - w.x) * (1.0 - w.y) * a.a + w.x * (1.0 - w.y) * b.a +
                   (1.0 - w.x) * w.y * c.a + w.x * w.y * d.a;
        vec3 rgb = (a.rgb * wa + b.rgb * wb + c.rgb * wc + d.rgb * wd) / max(wa + wb + wc + wd, 1e-4);
        return vec4(rgb, al);
    }
    vec4 a = tileTexel(tile, uv - dx - dy), b = tileTexel(tile, uv + dx - dy);
    vec4 c = tileTexel(tile, uv - dx + dy), d = tileTexel(tile, uv + dx + dy);
    float al = a.a + b.a + c.a + d.a;
    vec3 rgb = (a.rgb * a.a + b.rgb * b.a + c.rgb * c.a + d.rgb * d.a) / max(al, 1e-4);
    return vec4(rgb, al * 0.25);
}
#endif

uniform sampler2DShadow uShadowMap;
uniform sampler2D uLightMap;     // point lights, in the console's screen space
uniform vec3 uLightDir;          // towards the key light
uniform vec3 uKey;
uniform vec3 uAmbient;
uniform vec3 uEye;
uniform vec2 uCam;               // the game's camera: map px at the screen's top-left
uniform vec4 uFog;               // rgb, amount
uniform vec4 uDepth;             // focus distance, depth range, brightness, shadow amount
uniform vec4 uMisc;              // point lights, haze start, haze end, shadow texel
layout(location = 0) out vec4 oLit;
layout(location = 1) out vec4 oMat;

const vec3 NORMALS[6] = vec3[6](vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0),
                                vec3(-1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0),
                                vec3(0.0, 0.5, 0.866));

float shadowAt(vec3 n, vec3 L)
{
    if (uDepth.w <= 0.0)   // shadows off (port): the map isn't drawn
        return 1.0;
    vec3 s = vShadow.xyz / vShadow.w * 0.5 + 0.5;
    if (any(lessThan(s.xy, vec2(0.0))) || any(greaterThan(s.xy, vec2(1.0))))
        return 1.0;
    float bias = 0.0015 + 0.003 * (1.0 - max(dot(n, L), 0.0));
#ifdef CT_FAST
    // One lookup: the map's linear filter already compares four texels.
    return texture(uShadowMap, vec3(s.xy, s.z - bias));
#endif
    float t = uMisc.w, v = 0.0;
    for (int k = 0; k < 4; k++) {
        vec2 o = vec2(float(k & 1) - 0.5, float(k >> 1) - 0.5) * 1.5 * t;
        v += texture(uShadowMap, vec3(s.xy + o, s.z - bias));
    }
    return v * 0.25;
}

void main()
{
#ifdef WORLD_MAP
    if (uPassKind == 1) {   // the overlay plane: blended over (alpha: its opacity)
        vec4 c = overlayTexel(vWorld.xz);
        if (c.a < 0.5)
            discard;
        float d = length(vWorld - uEye);
        vec3 l = mix(c.rgb, uFog.rgb, uFog.a * 0.5 * smoothstep(uMisc.y, uMisc.z, d));
        float a = uCloud.y * mix(0.25, 1.0, smoothstep(24.0, 72.0, distance(vWorld.xz, uCloudClear)));
        oLit = vec4(l * uDepth.z, a);
        oMat = vec4(0.0, 0.0, 2.0 / 8.0, a);
        return;
    }
    if (((vTile >> 20u) & 7u) == 7u && (vTile & (1u << 19)) != 0u) {   // an entrance mark
        vec2 d = abs(vUV - 4.0);
        float r = d.x + d.y, pulse = 0.5 + 0.5 * sin(uTime * 2.5);
        if (r > 2.5 + 0.5 * pulse || r < 1.0 + 0.5 * pulse)
            discard;
        oLit = vec4(vec3(1.0, 0.86, 0.5) * (0.85 + 0.3 * pulse) * uDepth.z, 0.5);
        oMat = vec4(0.0, 0.0, 2.0 / 8.0, 0.0);
        return;
    }
#endif
#ifdef MODEL
    vec4 tex = modelTexel();
    if (tex.a < 0.5)
        discard;
    vec3 col = tex.rgb;
    bool obj = false;
    vec3 n = normalize(vNrm);
    if (dot(n, uEye - vWorld) < 0.0)   // a face seen from behind: lit as its other side
        n = -n;
    // the nearest of the fixed normals, for the ambient occlusion
    int ni = n.y > 0.7 ? 0 : abs(n.x) > abs(n.z) ? (n.x > 0.0 ? 2 : 3) : (n.z > 0.0 ? 1 : 4);
#else
#ifndef WORLD_MAP
    if (((vTile >> 20u) & 7u) == 7u && (vTile & (1u << 19)) != 0u) {   // a contact shadow (port)
        float r = length((vUV - 4.0) / 4.0);
        if (r > 1.0)
            discard;
        float k = 1.0 - 0.45 * smoothstep(1.0, 0.25, r) * uDepth.z;
        oLit = vec4(k, k, k, 1.0);   // multiplied in: the depth (a) stays
        oMat = vec4(1.0);
        return;
    }
#endif
    vec4 tex = tileTexelAA(vTile, vUV);
    if (tex.a < 0.5)
        discard;
    vec3 col = tex.rgb;
    bool obj = (vTile & (1u << 19)) != 0u;

    int ni = int((vTile >> 20u) & 7u);
    vec3 n = NORMALS[min(ni, 5)];
#endif
#ifdef WORLD_MAP
    if (obj && (vTile & (1u << 25)) != 0u) {   // flat over the picture, as the UI
        oLit = vec4(col * uDepth.z, 0.5);
        oMat = vec4(1.0, 0.0, 5.0 / 8.0, 0.0);
        return;
    }
    if (ni == 6)
        n = normalize(texture(uWorldNrm, (vWorld.xz / 8.0 + 0.5) / vec2(textureSize(uWorldNrm, 0)))
                          .rgb * 2.0 - 1.0);
    else if (ni == 7)
        n = vec3(0.0, 1.0, 0.0);
#endif
    vec3 L = normalize(uLightDir);
    float vis = shadowAt(n, L);
    float shade = 1.0 - uDepth.w * (1.0 - vis);
#ifdef WORLD_MAP
    if (uCloud.z > 0.0) {   // the clouds' shadows, soft: four taps where the sun ray meets them
        vec2 q = vWorld.xz + L.xz / max(L.y, 0.2) * (uCloud.x - vWorld.y);
        float c = overlayTexel(q + vec2(-4.0, -3.0)).a + overlayTexel(q + vec2(4.0, -3.0)).a +
                  overlayTexel(q + vec2(-3.0, 4.0)).a + overlayTexel(q + vec2(3.0, 4.0)).a;
        shade *= 1.0 - uCloud.z * 0.25 * c;
    }
    float amb = ni == 0 || ni >= 6 ? 1.0 : 0.85;   // the ground keeps its colors where lit
#elif defined(MODEL)
    float amb = mix(0.85, 1.0, clamp(n.y, 0.0, 1.0));
#else
    float amb = ni == 0 ? 1.0 : 0.85;
#endif
    // Relative to the sunlit floor, which keeps its colors.
    vec3 flat_ = uAmbient + uKey * max(L.y, 0.05);
    vec3 light = uAmbient * amb + uKey * max(dot(n, L), 0.0) * shade;
    vec3 rel = light / flat_;
    if (obj)
        rel = mix(vec3(shade), rel, 0.4);   // sprites: mostly their own shading
    vec3 lit = col * rel;
#ifdef WORLD_MAP
    if (ni == 7) {   // water: a slow swell, and glints that twinkle, on the console's pixels
        vec2 p = floor(vWorld.xz);
        float w = sin(p.x * 0.05 + uTime * 0.9 + 1.5 * sin(p.y * 0.04 + uTime * 0.3)) *
                  sin(p.y * 0.06 - uTime * 0.7);
        lit *= 1.0 + 0.035 * w;
        vec2 cell = floor(p / 3.0);
        float h = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
        float tw = pow(max(sin(uTime * 1.7 + h * 40.0), 0.0), 24.0) * step(0.93, fract(h * 7.31));
        lit += vec3(0.22, 0.26, 0.3) * tw * shade * (0.6 + 0.4 * w);
    }
#endif

    // Point lights where the console drew this pixel (screen row = z - height).
    vec2 sp = vec2(vWorld.x - uCam.x, vWorld.z - vWorld.y - uCam.y) / vec2(256.0, 224.0);
    vec3 pl = texture(uLightMap, sp).rgb;
    lit += col * pl * uMisc.x;

    float dist = length(vWorld - uEye);
    lit = mix(lit, uFog.rgb, uFog.a * smoothstep(uMisc.y, uMisc.z, dist));
    lit *= uDepth.z;   // the console's brightness (fades)

    float depth = clamp(0.5 + (dist - uDepth.x) / uDepth.y, 0.0, 1.0);
    bool raised = (vTile & (1u << 24)) != 0u;
    float cls = obj ? 4.0 : raised ? 3.0 : 2.0;
    oLit = vec4(lit, depth);
    // a: the normal for the ambient occlusion (ssao.frag), index + 1 over 8;
    // 0 for sprites, which get none.
#ifdef WORLD_MAP
    // The ground's normal as the nearest of ssao.frag's (up unless steep);
    // water gets none (it would darken under every shore).
    int ao = ni;
    if (ni == 6)
        ao = n.y > 0.7 ? 0 : abs(n.x) > abs(n.z) ? (n.x > 0.0 ? 2 : 3) : (n.z > 0.0 ? 1 : 4);
    oMat = vec4(0.0, obj ? 1.0 : 0.0, cls / 8.0, obj || ni == 7 ? 0.0 : float(min(ao, 5) + 1) / 8.0);
#else
    oMat = vec4(0.0, obj ? 1.0 : 0.0, cls / 8.0, obj ? 0.0 : float(min(ni, 5) + 1) / 8.0);
#endif
}
#endif
