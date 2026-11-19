// SPDX-FileCopyrightText: 2026 jerbanpls
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 3D diorama (scene3d.h): one 8x8 tile per quad, in world space (map
// pixels, y up). uMVP is the camera's, or the key light's for the shadow
// map. The tile it shows is decoded from VRAM per pixel (scene3d.frag).
// With WORLD_MAP, uOffset moves the whole mesh (a wrapping world map's
// neighbouring copies).
//
// With MODEL, an asset pack's model (hd2d.h hd2d_models): its own normal
// and vertex color instead of a tile, always raised (it casts shadows).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
#ifdef MODEL
layout(location = 3) in vec3 aNrm;
layout(location = 4) in vec4 aCol;
out vec3 vNrm;
out vec4 vCol;
#else
layout(location = 2) in uint aTile;
#endif
uniform mat4 uMVP;
uniform mat4 uLightMVP;
#if defined(WORLD_MAP) || defined(MODEL)
uniform vec3 uOffset;
#define POS (aPos + uOffset)
#else
#define POS aPos
#endif
out vec2 vUV;
out vec3 vWorld;
out vec4 vShadow;
flat out uint vTile;

#if !defined(WORLD_MAP) && !defined(MODEL) && !defined(SHADOW_PASS)
// port: layered cards: a lifted card's corners slid toward where the ray
// from the eye through the spot the console drew them meets their height,
// by uCard.x (1: exactly there, no parallax and no seams between a lifted
// part and the rest of the same table or tent; 0: where the mesh put them,
// lift * uCard.y south of that spot).
uniform vec2 uCard;
uniform vec3 uEyeV;
vec3 cardPos(vec3 p, uint tile)
{
    if (uCard.x <= 0.0 || p.y < 0.01 || ((tile >> 20u) & 7u) != 0u || (tile & (1u << 19)) != 0u)
        return p;
    vec3 g = vec3(p.x, 0.0, p.z - p.y * uCard.y);   // the spot on the ground
    vec3 r = g + (uEyeV - g) * (p.y / max(uEyeV.y, 1.0));
    return mix(p, r, uCard.x);
}
#endif

void main()
{
    vUV = aUV;
#ifdef MODEL
    vTile = 1u << 24u;   // S3D_RAISED
    vNrm = aNrm;
    vCol = aCol;
#else
    vTile = aTile;
#endif
#if !defined(WORLD_MAP) && !defined(MODEL) && !defined(SHADOW_PASS)
    vec3 pos = cardPos(POS, aTile);
#else
    vec3 pos = POS;
#endif
    vWorld = pos;
    vShadow = uLightMVP * vec4(pos, 1.0);
    gl_Position = uMVP * vec4(pos, 1.0);
}
