// SPDX-FileCopyrightText: 2026 jerbanpls
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pass 5, at the output size: depth of field (the blurred foreground plane
// over the blurred background plane), bloom, color grade, vignette and
// grain, with the UI put back sharp and ungraded on top, and the screen's
// sides (outside the picture's 4:3 rectangle) filled with a soft, dimmed
// extension of the picture.
//
// Compiled after upscale.frag: planes() gives the picture's two planes
// (composite rgb, a = depth; background plane rgb, a = foreground
// coverage) at each pixel; uMat (r = UI, b = class / 8) is nearest.
uniform sampler2D uDof;      // half size: background plane blurred, a = coc
uniform sampler2D uFgDof;    // half size: foreground plane blurred, premultiplied
uniform vec4 uFocus;         // as coc.frag: for the foreground's blur amount
uniform vec3 uDof3;
uniform int uFillTB;      // mirror-fill the bars above and below (0: black)
uniform sampler2D uBloomA;   // quarter size
uniform sampler2D uBloomB;   // eighth size
uniform sampler2D uSoft;     // eighth size, unthresholded: side fill
uniform sampler2D uOrig;     // source size, nearest: the console's frame (.bgr)
uniform vec4 uRect;          // picture rectangle in output pixels: x, y, w, h (top-left origin)
uniform vec2 uOut;           // output size
uniform int uFlip;           // 1: drawing to the window (bottom-left origin)
uniform vec4 uGrade;         // bloom, exposure, saturation, vignette
uniform vec4 uGrade2;        // grain, split-tone amount, contrast, effects on (0/1)
uniform vec3 uShadowTint;
uniform vec3 uHighTint;
uniform float uSideDim;
uniform float uTime;
uniform sampler2D uOsd;      // message mask, nearest
uniform vec4 uOsdRect;       // its rectangle in output pixels (top-left origin)
uniform float uOsdAlpha;     // 0: no message
uniform sampler2D uText;     // port: HD text, premultiplied (hdtext.h)
uniform vec4 uTextRect;      // its rectangle in output pixels (top-left origin)
uniform int uTextOn;
uniform int uDebug;          // 0 off, 1 classes, 2 depth, 3 blur amount, 4 split compare,
                             // 5 background plane
out vec4 oColor;

float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

float fgCoc(float y)
{
    float tilt = smoothstep(uFocus.y, uFocus.y + uFocus.z, abs(y - uFocus.x));
    return max(tilt * uDof3.x, smoothstep(0.08, 0.45, abs(uDof3.z - uFocus.w)) * uDof3.y);
}

vec3 classColor(float c)
{
    int k = int(c * 8.0 + 0.5);
    if (k == 0) return vec3(0.1, 0.1, 0.3);   // backdrop
    if (k == 1) return vec3(0.2, 0.4, 0.8);   // far
    if (k == 2) return vec3(0.2, 0.7, 0.3);   // ground
    if (k == 3) return vec3(0.9, 0.6, 0.1);   // foreground
    if (k == 4) return vec3(1.0, 0.2, 0.6);   // actor
    return vec3(1.0, 1.0, 1.0);               // UI
}

vec3 grade(vec3 c, vec2 uv)
{
    // Exposure, then a soft shoulder: identity below 0.7 (blacks stay
    // black), highlights roll off smoothly instead of clipping.
    c *= uGrade.y;
    const float k = 0.7;
    vec3 over = max(c - k, 0.0);
    c = min(c, vec3(k)) + (1.0 - k) * (1.0 - exp(-over / (1.0 - k)));
    // Contrast around mid grey.
    c = mix(vec3(0.5), c, uGrade2.z);
    // Saturation.
    float l = luma(c);
    c = mix(vec3(l), c, uGrade.z);
    // Split toning: tint the shadows and the highlights.
    float t = smoothstep(0.0, 1.0, l);
    c *= mix(uShadowTint, uHighTint, t) * uGrade2.y + vec3(1.0 - uGrade2.y);
    // Vignette.
    vec2 d = (uv - 0.5) * vec2(1.15, 1.0);
    c *= 1.0 - uGrade.w * smoothstep(0.35, 0.95, length(d));
    return c;
}

vec3 shade(vec2 fc)
{
    vec2 uv = (fc - uRect.xy) / uRect.zw;   // picture coordinates, 0..1 inside
    bool inside = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)));

    if (!inside) {
        if (uFillTB == 0 && (uv.y < 0.0 || uv.y >= 1.0))
            return vec3(0.0);   // black bars above and below: no blurred copy there
        // Side fill: the soft copy mirrored at the picture's edge (a blurred
        // reflection, not stretched edge columns), dimmed with distance.
        // Bars above and below (a tall window) mirror and fade the same
        // way, in picture heights, not the edge row smeared down the bar.
        vec2 q = uv;
        float out_ = max(max(-uv.x, uv.x - 1.0), max(-uv.y, uv.y - 1.0));
        q.x = uv.x < 0.0 ? -uv.x * 0.7 : uv.x > 1.0 ? 1.0 - (uv.x - 1.0) * 0.7 : uv.x;
        q.y = uv.y < 0.0 ? -uv.y * 0.7 : uv.y > 1.0 ? 1.0 - (uv.y - 1.0) * 0.7 : uv.y;
        q = clamp(q, vec2(0.002), vec2(0.998));
        // A 9-tap disc on top of the soft copy's blur: no visible blobs.
        vec2 r = 2.5 / vec2(textureSize(uSoft, 0));
        vec3 c = texture(uSoft, q).rgb * 0.2;
        for (int k = 0; k < 8; k++) {
            float a = float(k) * 0.785398;
            c += texture(uSoft, clamp(q + vec2(cos(a), sin(a)) * r, vec2(0.002), vec2(0.998))).rgb * 0.1;
        }
        c *= uSideDim;
        float fade = 1.0 - smoothstep(0.0, 0.35, out_);
        return c * (0.35 + 0.65 * fade);
    }

    vec4 scene, back, mat;
    planes(uv, scene, back, mat);

    if (uDebug == 1) { return mix(classColor(mat.b), scene.rgb, 0.35); }
    if (uDebug == 2) { return vec3(scene.a); }
    if (uDebug == 3) { return vec3(texture(uDof, uv).a); }
    if (uDebug == 5) { return back.rgb; }
    if (uDebug == 4 && uv.x < 0.5) {
        // Split compare: the console's own pixels left of center.
        float line_ = 1.0 - step(1.5 / uRect.z, abs(uv.x - 0.5));
        return mix(texture(uOrig, uv).bgr, vec3(1.0), line_);
    }

    if (uGrade2.w < 0.5 || mat.r > 0.5) {
        // Effects off, or UI: the plain upscaled picture.
        return scene.rgb;
    }

    // The background plane, blurred by its own amount, then the
    // foreground plane over it: sharp where it is in focus, otherwise its
    // blurred copy, whose soft edge lets the background through.
    vec4 dof = texture(uDof, uv);
    vec3 bg = mix(back.rgb, dof.rgb, smoothstep(0.0, 0.6, dof.a));
    vec4 fg = vec4(scene.rgb * back.a, back.a);
    float fgBlur = smoothstep(0.0, 0.6, fgCoc(uv.y));
    if (fgBlur > 0.0)   // rows in focus skip the read
        fg = mix(fg, texture(uFgDof, uv), fgBlur);
    vec3 c = bg * (1.0 - fg.a) + fg.rgb;
    c += uGrade.x * (texture(uBloomA, uv).rgb * 0.6 + texture(uBloomB, uv).rgb * 0.8);
    c = grade(c, uv);
    c += (hash(fc + fract(uTime) * 61.0) - 0.5) * uGrade2.x;
    return clamp(c, 0.0, 1.0);
}

void main()
{
    vec2 fc = vec2(gl_FragCoord.x, uFlip == 1 ? uOut.y - gl_FragCoord.y : gl_FragCoord.y);
    vec3 c = shade(fc);
    if (uTextOn == 1) {
        vec2 o = (fc - uTextRect.xy) / uTextRect.zw;
        if (all(greaterThanEqual(o, vec2(0.0))) && all(lessThan(o, vec2(1.0)))) {
            vec4 t = texture(uText, o);
            c = c * (1.0 - t.a) + t.rgb;
        }
    }
    // On-screen message: white text on a dimmed box, fading out.
    if (uOsdAlpha > 0.0) {
        vec2 o = (fc - uOsdRect.xy) / uOsdRect.zw;
        if (all(greaterThanEqual(o, vec2(0.0))) && all(lessThan(o, vec2(1.0)))) {
            float m = texture(uOsd, o).r;
            c = mix(c, mix(c * 0.3, vec3(1.0), m), uOsdAlpha);
        }
    }
    oColor = vec4(c, 1.0);
}
