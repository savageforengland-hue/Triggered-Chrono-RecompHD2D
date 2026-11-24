/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HD-2D renderer (hd2d.h). Passes, all full-screen triangles:
 *
 *   1 light     source size   classify pixels from the layer G-buffer,
 *                             relief lighting, actor shadows, contact
 *                             occlusion, rim light, haze  -> lit, mat
 *   2 upscale   picture size  sharp bilinear, split into a foreground
 *                             and a background plane (the behind
 *                             G-buffer fills in under the foreground),
 *                             diorama tilt. Not a pass: upscale.frag is
 *                             compiled into the passes that read the
 *                             picture (3a, 4a, 5), which work it out from
 *                             the source-size targets as they go
 *   3 coc/dof   half size     blur amount, then H and V variable blur,
 *                             both planes at once
 *   4 bloom     1/4 and 1/8   threshold, Gaussian H and V; plus an
 *                             unthresholded 1/8 copy for the side fill,
 *                             made only when the picture leaves sides
 *   5 final     output size   DOF mix, bloom, grade, vignette, grain, UI
 *
 * The 3D diorama (hd2d3d, deck3d) replaces 1 and 2 with its own passes
 * (shadow map, surfaces at the render scale, ambient occlusion, FSR 1's
 * EASU up to the picture size, the UI over it) and runs 3-5 on the result
 * (below, "the 3D diorama").
 *
 * Every intermediate keeps row 0 at the top of the picture (the order the
 * frame is uploaded in); only drawing to the window flips. GL entry points
 * come from SDL_GL_GetProcAddress, so nothing links libGL directly. */
#include "hd2d.h"

#include <math.h>
#include <stddef.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "hd2d_shaders.h"   /* generated: tools/embed_text.py */
#include "osd_text.h"   /* port: the message mask, with glyphs */
#include "scene3d.h"

/* ---- the GL subset used here (values from the Khronos registry) ---- */
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float GLfloat;
typedef char GLchar;

#define GL_NO_ERROR                 0
#define GL_TRIANGLES                0x0004
#define GL_DEPTH_TEST               0x0B71
#define GL_BLEND                    0x0BE2
#define GL_SCISSOR_TEST             0x0C11
#define GL_UNPACK_ALIGNMENT         0x0CF5
#define GL_PACK_ALIGNMENT           0x0D05
#define GL_TEXTURE_2D               0x0DE1
#define GL_UNSIGNED_BYTE            0x1401
#define GL_FLOAT                    0x1406
#define GL_HALF_FLOAT               0x140B
#define GL_RED                      0x1903
#define GL_RG                       0x8227
#define GL_RG16F                    0x822F
#define GL_RGBA                     0x1908
#define GL_VENDOR                   0x1F00
#define GL_RENDERER                 0x1F01
#define GL_VERSION                  0x1F02
#define GL_NEAREST                  0x2600
#define GL_LINEAR                   0x2601
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803
#define GL_COLOR_BUFFER_BIT         0x00004000
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_TEXTURE0                 0x84C0
#define GL_RGBA8                    0x8058
#define GL_RGB10_A2                 0x8059
#define GL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#define GL_R8                       0x8229
#define GL_RGBA16F                  0x881A
#define GL_R8UI                     0x8232
#define GL_RED_INTEGER              0x8D94
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_LINK_STATUS              0x8B82
#define GL_INFO_LOG_LENGTH          0x8B84
#define GL_FRAMEBUFFER              0x8D40
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_COLOR_ATTACHMENT1        0x8CE1
#define GL_COLOR_ATTACHMENT2        0x8CE2
#define GL_READ_FRAMEBUFFER         0x8CA8
#define GL_DRAW_FRAMEBUFFER         0x8CA9
/* The 3D diorama's (scene3d.h). */
#define GL_NONE                     0
#define GL_FALSE                    0
#define GL_LESS                     0x0201
#define GL_LEQUAL                   0x0203
#define GL_ALWAYS                   0x0207
#define GL_DEPTH_BUFFER_BIT         0x00000100
#define GL_UNSIGNED_SHORT           0x1403
#define GL_UNSIGNED_INT             0x1405
#define GL_DEPTH_COMPONENT          0x1902
#define GL_CONSTANT_ALPHA           0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define GL_POLYGON_OFFSET_FILL      0x8037
#define GL_DEPTH_COMPONENT24        0x81A6
#define GL_R16UI                    0x8234
#define GL_TEXTURE_COMPARE_MODE     0x884C
#define GL_TEXTURE_COMPARE_FUNC     0x884D
#define GL_COMPARE_REF_TO_TEXTURE   0x884E
#define GL_ARRAY_BUFFER             0x8892
#define GL_ELEMENT_ARRAY_BUFFER     0x8893
#define GL_STREAM_DRAW              0x88E0
#define GL_STATIC_DRAW              0x88E4
#define GL_DEPTH_ATTACHMENT         0x8D00
typedef ptrdiff_t GLsizeiptr;
/* port: asset packs */
#define GL_TEXTURE_2D_ARRAY         0x8C1A
#define GL_RGBA16UI                 0x8D76
#define GL_RGBA_INTEGER             0x8D99
#define GL_LINEAR_MIPMAP_LINEAR     0x2703
#define GL_TEXTURE_MAX_LEVEL        0x813D
#define GL_MAX_TEXTURE_SIZE         0x0D33
/* the 3D world map */
#define GL_REPEAT                   0x2901
#define GL_ZERO                     0
#define GL_ONE                      1
#define GL_SRC_ALPHA                0x0302
#define GL_DST_COLOR                0x0306
#define GL_ONE_MINUS_SRC_ALPHA      0x0303

#define GL_FUNCS(X)                                                                           \
    X(const GLubyte *, glGetString, (GLenum name))                                            \
    X(GLenum, glGetError, (void))                                                             \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                             \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                       \
    X(void, glClear, (GLbitfield mask))                                                       \
    X(void, glDisable, (GLenum cap))                                                          \
    X(void, glFinish, (void))                                                                 \
    X(void, glPixelStorei, (GLenum pname, GLint param))                                       \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p)) \
    X(void, glGenTextures, (GLsizei n, GLuint *t))                                            \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *t))                                   \
    X(void, glBindTexture, (GLenum target, GLuint t))                                         \
    X(void, glActiveTexture, (GLenum unit))                                                   \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))                      \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,      \
                           GLint border, GLenum fmt, GLenum type, const void *data))          \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w,        \
                              GLsizei h, GLenum fmt, GLenum type, const void *data))          \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *f))                                        \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *f))                               \
    X(void, glBindFramebuffer, (GLenum target, GLuint f))                                     \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum att, GLenum tt, GLuint t, GLint l)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                      \
    X(void, glDrawBuffers, (GLsizei n, const GLenum *bufs))                                   \
    X(GLuint, glCreateShader, (GLenum type))                                                  \
    X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar *const *src, const GLint *len)) \
    X(void, glCompileShader, (GLuint s))                                                      \
    X(void, glGetShaderiv, (GLuint s, GLenum pname, GLint *v))                                \
    X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei *len, GLchar *log))           \
    X(void, glDeleteShader, (GLuint s))                                                       \
    X(GLuint, glCreateProgram, (void))                                                        \
    X(void, glAttachShader, (GLuint p, GLuint s))                                             \
    X(void, glLinkProgram, (GLuint p))                                                        \
    X(void, glGetProgramiv, (GLuint p, GLenum pname, GLint *v))                               \
    X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei *len, GLchar *log))          \
    X(void, glDeleteProgram, (GLuint p))                                                      \
    X(void, glUseProgram, (GLuint p))                                                         \
    X(GLint, glGetUniformLocation, (GLuint p, const GLchar *name))                            \
    X(void, glUniform1i, (GLint loc, GLint v))                                                \
    X(void, glUniform1f, (GLint loc, GLfloat v))                                              \
    X(void, glUniform2f, (GLint loc, GLfloat a, GLfloat b))                                   \
    X(void, glUniform2i, (GLint loc, GLint a, GLint b))                                       \
    X(void, glUniform3f, (GLint loc, GLfloat a, GLfloat b, GLfloat c))                        \
    X(void, glUniform4f, (GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d))             \
    X(void, glUniform3fv, (GLint loc, GLsizei n, const GLfloat *v))                           \
    X(void, glUniform4fv, (GLint loc, GLsizei n, const GLfloat *v))                           \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *v))                                        \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint *v))                               \
    X(void, glBindVertexArray, (GLuint v))                                                    \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                          \
    X(void, glEnable, (GLenum cap))                                                           \
    X(void, glDepthFunc, (GLenum f))                                                          \
    X(void, glDepthMask, (GLboolean on))                                                      \
    X(void, glBlendFunc, (GLenum s, GLenum d))                                                \
    X(void, glBlendColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                       \
    X(void, glPolygonOffset, (GLfloat factor, GLfloat units))                                 \
    X(void, glReadBuffer, (GLenum b))                                                         \
    X(void, glGenBuffers, (GLsizei n, GLuint *b))                                             \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint *b))                                    \
    X(void, glBindBuffer, (GLenum target, GLuint b))                                          \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage))   \
    X(void, glEnableVertexAttribArray, (GLuint i))                                            \
    X(void, glVertexAttribPointer, (GLuint i, GLint n, GLenum t, GLboolean norm, GLsizei stride, \
                                    const void *off))                                         \
    X(void, glVertexAttribIPointer, (GLuint i, GLint n, GLenum t, GLsizei stride, const void *off)) \
    X(void, glDrawElements, (GLenum mode, GLsizei n, GLenum t, const void *off))              \
    X(void, glUniformMatrix4fv, (GLint loc, GLsizei n, GLboolean transpose, const GLfloat *m)) \
    X(void, glTexImage3D, (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,      \
                           GLsizei d, GLint border, GLenum fmt, GLenum type, const void *data)) \
    X(void, glTexSubImage3D, (GLenum target, GLint level, GLint x, GLint y, GLint z,          \
                              GLsizei w, GLsizei h, GLsizei d, GLenum fmt, GLenum type,       \
                              const void *data))                                              \
    X(void, glGenerateMipmap, (GLenum target))                                                \
    X(void, glGetIntegerv, (GLenum pname, GLint *v))                                          \
    X(void, glBlendFuncSeparate, (GLenum sc, GLenum dc, GLenum sa, GLenum da))

#define X(ret, name, args) typedef ret (*PFN_##name) args; static PFN_##name p_##name;
GL_FUNCS(X)
#undef X

static int load_gl(void)
{
    int missing = 0;
#define X(ret, name, args)                                                     \
    {                                                                          \
        void *sym = SDL_GL_GetProcAddress(#name);                              \
        memcpy(&p_##name, &sym, sizeof sym); /* object -> function pointer */  \
    }                                                                          \
    if (!p_##name) {                                                           \
        fprintf(stderr, "hd2d: missing GL function %s\n", #name);             \
        missing = 1;                                                           \
    }
    GL_FUNCS(X)
#undef X
    return missing ? -1 : 0;
}

static int perf_osd;   /* port: hd2d_set_perf_osd */
#ifdef CT_PS5
static float dyn_scale = 1.0f;   /* port: the 3D view's dynamic resolution (PS5) */
#endif
void hd2d_set_perf_osd(int on) { perf_osd = on != 0; }
int hd2d_perf_osd(void) { return perf_osd; }
void hd2d_osd(const char *text);

/* ---- optional per-pass GPU timing ----
   CT_HD2D_PROFILE=FILE writes one line per frame: GPU time per pass from
   timestamp queries (GL 3.3 / ARB_timer_query, or ES with
   EXT_disjoint_timer_query), read back a few frames later so nothing
   stalls, plus the CPU time of the scene analysis and of the uploads. */
#define GL_TIMESTAMP                0x8E28
#define GL_QUERY_RESULT             0x8866
#define GL_EXTENSIONS               0x1F03
typedef unsigned long long GLuint64;
typedef void (*PFN_glGenQueries)(GLsizei n, GLuint *ids);
typedef void (*PFN_glDeleteQueries)(GLsizei n, const GLuint *ids);
typedef void (*PFN_glQueryCounter)(GLuint id, GLenum target);
typedef void (*PFN_glGetQueryObjectui64v)(GLuint id, GLenum pname, GLuint64 *v);

enum { PROF_MARKS = 24, PROF_RING = 4 };

static struct {
    FILE *f;
    PFN_glGenQueries gen;
    PFN_glDeleteQueries del;
    PFN_glQueryCounter counter;
    PFN_glGetQueryObjectui64v get;
    GLuint q[PROF_RING][PROF_MARKS];
    const char *name[PROF_RING][PROF_MARKS];
    int n[PROF_RING];
    unsigned frame[PROF_RING];
    double analyze_ms[PROF_RING], upload_ms[PROF_RING], submit_ms[PROF_RING],
        total_ms[PROF_RING];
    int slot;
} prof;

static void *gl_sym(const char *name)
{
    return SDL_GL_GetProcAddress(name);
}

static void prof_init(int gles)
{
    if (prof.f) {   /* a second init, after the first context failed */
        fclose(prof.f);
        memset(&prof, 0, sizeof prof);
    }
    const char *path = getenv("CT_HD2D_PROFILE");
    if (!path || !*path)
        return;
    void *gen = gl_sym("glGenQueries"), *del = gl_sym("glDeleteQueries");
    void *counter = gl_sym(gles ? "glQueryCounterEXT" : "glQueryCounter");
    void *get = gl_sym(gles ? "glGetQueryObjectui64vEXT" : "glGetQueryObjectui64v");
    /* (glGetString(GL_EXTENSIONS) is an error in a core profile.) */
    const char *ext = gles ? (const char *)p_glGetString(GL_EXTENSIONS) : NULL;
    if (gles && !(ext && strstr(ext, "GL_EXT_disjoint_timer_query")))
        counter = NULL;
    if (!gen || !del || !counter || !get) {
        fprintf(stderr, "hd2d: CT_HD2D_PROFILE: no timer queries here\n");
        return;
    }
    memcpy(&prof.gen, &gen, sizeof gen);
    memcpy(&prof.del, &del, sizeof del);
    memcpy(&prof.counter, &counter, sizeof counter);
    memcpy(&prof.get, &get, sizeof get);
    if (!(prof.f = fopen(path, "w"))) {
        fprintf(stderr, "hd2d: cannot write %s\n", path);
        return;
    }
    prof.gen(PROF_RING * PROF_MARKS, &prof.q[0][0]);
}

/* A timestamp at the end of the pass called name (the first one, "start",
   only opens the frame). */
static void prof_mark(const char *name)
{
    int s = prof.slot;
    if (prof.f && prof.n[s] < PROF_MARKS) {
        prof.counter(prof.q[s][prof.n[s]], GL_TIMESTAMP);
        prof.name[s][prof.n[s]++] = name;
    }
}

/* Write out the frame recorded in the ring slot about to be reused. */
static void prof_flush(int s)
{
    if (!prof.f || prof.n[s] < 2)
        return;
#ifdef CT_PS5   /* port: every 30th frame on the console: a small file, few writes */
    if (prof.frame[s] % 30) {
        prof.n[s] = 0;
        return;
    }
#endif
    GLuint64 t[PROF_MARKS];
    for (int k = 0; k < prof.n[s]; k++)
        prof.get(prof.q[s][k], GL_QUERY_RESULT, &t[k]);
    fprintf(prof.f, "frame=%u total=%.4f analyze=%.4f upload=%.4f submit=%.4f gpu=%.4f",
            prof.frame[s], prof.total_ms[s], prof.analyze_ms[s], prof.upload_ms[s],
            prof.submit_ms[s], (double)(t[prof.n[s] - 1] - t[0]) / 1e6);
    for (int k = 1; k < prof.n[s]; k++)
        fprintf(prof.f, " %s=%.4f", prof.name[s][k], (double)(t[k] - t[k - 1]) / 1e6);
    fputc('\n', prof.f);
    fflush(prof.f);
#ifdef CT_PS5
    /* port: dynamic resolution for the 3D view. Every 30th frame: over
       15 ms of GPU work (60 fps needs 16.7, with some to spare) draws the
       3D scene 5% smaller, down to 75%; under 11 ms, 5% bigger again. */
    {
        int threeD = 0;
        for (int k = 1; k < prof.n[s]; k++)
            threeD |= !strncmp(prof.name[s][k], "s3d_", 4);
        double gpu = (double)(t[prof.n[s] - 1] - t[0]) / 1e6;
        if (threeD && gpu > 15.0 && dyn_scale > 0.751f)
            dyn_scale -= 0.05f;
        else if (threeD && gpu < 11.0 && dyn_scale < 0.999f)
            dyn_scale += 0.05f;
    }
#endif
#ifdef CT_PS5
    /* port: the readout (hd2d_set_perf_osd): every 4 s, the frame's GPU
       time and its three slowest passes, so a photo of the screen tells
       what the file would. */
    if (perf_osd && prof.frame[s] % 240 == 0) {
        int top[3] = {0, 0, 0};
        for (int r = 0; r < 3; r++)
            for (int k = 1; k < prof.n[s]; k++) {
                if (k == top[0] || k == top[1])
                    continue;
                if (!top[r] || t[k] - t[k - 1] > t[top[r]] - t[top[r] - 1])
                    top[r] = k;
            }
        char msg[160];
        int len = snprintf(msg, sizeof msg, "3D %d%% GPU %.1f CPU %.1f:",
                           (int)lroundf(dyn_scale * 100.0f),
                           (double)(t[prof.n[s] - 1] - t[0]) / 1e6,
                           prof.submit_ms[s]);   /* from the start: it holds the other two */
        for (int r = 0; r < 3 && top[r] && len < (int)sizeof msg; r++) {
            char nm[24];
            int j = 0;
            const char *pn = prof.name[s][top[r]];
            if (!strncmp(pn, "s3d_", 4))
                pn += 4;   /* short: the line must fit the screen */
            for (const char *c = pn; *c && j < 23; c++)
                nm[j++] = *c == '_' ? ' ' : (char)toupper((unsigned char)*c);
            nm[j] = 0;
            len += snprintf(msg + len, sizeof msg - (size_t)len, " %s %.1f", nm,
                            (double)(t[top[r]] - t[top[r] - 1]) / 1e6);
        }
        hd2d_osd(msg);
    }
#endif
    prof.n[s] = 0;
}

static double ms_since(uint64_t t0)
{
    return (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
           (double)SDL_GetPerformanceFrequency();
}

/* ---- settings ---- */

typedef struct {
    float relief, shadow, ao, rim, haze, point_light;
    float dof_tilt, dof_depth, focus_band, focus_ramp, dof_radius;
    float bloom, bloom_threshold;
    float exposure, saturation, contrast, vignette, grain, split;
    float side_dim;
    float tilt;   /* diorama tilt: foreground enlargement about the focus */
    int effects;
    int bloom_quarter_only;   /* one bloom level, no 1/8 one */
    int side_fill_wide_only;  /* side fill only where the bars are over 20% of the width */
    int scene3d;              /* map views as the 3D diorama, where there is a scene */
    /* The 3D diorama's quality (hd2d.h: hd2d_set_render_scale and the rest):
       the preset's render scale and ambient occlusion strength, and the
       occlusion's size (the render size / ao_div). */
    float render_scale, ao3d;
    int ao_div;
} settings;

static const settings presets[HD2D_PRESETS] = {
    [HD2D_OFF] = {.exposure = 1.0f, .saturation = 1.0f, .contrast = 1.0f, .side_dim = 0.45f},
    [HD2D_SUBTLE] = {.relief = 0.3f, .shadow = 0.5f, .ao = 0.4f, .rim = 0.3f, .haze = 0.25f, .point_light = 0.6f,
                     .dof_tilt = 0.45f, .dof_depth = 0.25f, .focus_band = 0.2f, .focus_ramp = 0.4f,
                     .dof_radius = 4.0f, .bloom = 0.25f, .bloom_threshold = 0.78f,
                     .exposure = 1.03f, .saturation = 1.06f, .contrast = 1.02f, .vignette = 0.2f,
                     .grain = 0.01f, .split = 0.2f, .side_dim = 0.42f, .tilt = 0.0f, .effects = 1},
    [HD2D_FULL] = {.relief = 0.55f, .shadow = 0.85f, .ao = 0.7f, .rim = 0.6f, .haze = 0.5f, .point_light = 1.2f,
                   .dof_tilt = 0.9f, .dof_depth = 0.5f, .focus_band = 0.18f, .focus_ramp = 0.34f,
                   .dof_radius = 6.0f, .bloom = 0.45f, .bloom_threshold = 0.7f,
                   .exposure = 1.06f, .saturation = 1.12f, .contrast = 1.04f, .vignette = 0.35f,
                   .grain = 0.018f, .split = 0.35f, .side_dim = 0.42f, .tilt = 0.022f,
                   .effects = 1},
    /* The Steam Deck: HD2D_FULL with a shorter blur, one bloom level (the
       1/4 one, read for both of the final pass's bloom taps) and, at
       16:10, black bars without the side fill. */
    [HD2D_DECK] = {.relief = 0.55f, .shadow = 0.85f, .ao = 0.7f, .rim = 0.6f, .haze = 0.5f, .point_light = 1.2f,
                   .dof_tilt = 0.9f, .dof_depth = 0.5f, .focus_band = 0.18f, .focus_ramp = 0.34f,
                   .dof_radius = 3.5f, .bloom = 0.4f, .bloom_threshold = 0.7f,
                   .exposure = 1.06f, .saturation = 1.12f, .contrast = 1.04f, .vignette = 0.35f,
                   .grain = 0.018f, .split = 0.35f, .side_dim = 0.42f, .tilt = 0.022f,
                   .effects = 1, .bloom_quarter_only = 1, .side_fill_wide_only = 1},
    /* HD2D_FULL, with the 3D diorama in map views (the rest as HD2D_FULL). */
    [HD2D_3D] = {.relief = 0.55f, .shadow = 0.85f, .ao = 0.7f, .rim = 0.6f, .haze = 0.5f, .point_light = 1.2f,
                 .dof_tilt = 0.9f, .dof_depth = 0.5f, .focus_band = 0.18f, .focus_ramp = 0.34f,
                 .dof_radius = 6.0f, .bloom = 0.45f, .bloom_threshold = 0.7f,
                 .exposure = 1.06f, .saturation = 1.12f, .contrast = 1.04f, .vignette = 0.35f,
                 .grain = 0.018f, .split = 0.35f, .side_dim = 0.42f, .tilt = 0.022f,
                 .effects = 1, .scene3d = 1, .render_scale = 1.0f, .ao3d = 0.75f, .ao_div = 2},
    /* HD2D_DECK with the 3D diorama: the surfaces at 67% of the picture's
       size and FSR 1 up to it, the ambient occlusion at a quarter. */
    [HD2D_DECK3D] = {.relief = 0.55f, .shadow = 0.85f, .ao = 0.7f, .rim = 0.6f, .haze = 0.5f, .point_light = 1.2f,
                     .dof_tilt = 0.9f, .dof_depth = 0.5f, .focus_band = 0.18f, .focus_ramp = 0.34f,
                     .dof_radius = 3.5f, .bloom = 0.4f, .bloom_threshold = 0.7f,
                     .exposure = 1.06f, .saturation = 1.12f, .contrast = 1.04f, .vignette = 0.35f,
                     .grain = 0.018f, .split = 0.35f, .side_dim = 0.42f, .tilt = 0.022f,
                     .effects = 1, .bloom_quarter_only = 1, .side_fill_wide_only = 1,
                     .scene3d = 1, .render_scale = 0.67f, .ao3d = 0.75f, .ao_div = 4},
};

static const char *const preset_names[HD2D_PRESETS] = {"off",  "subtle", "hd2d",
                                                        "deck", "hd2d3d", "deck3d"};

static int cur_preset = HD2D_FULL, debug_view, aspect_mode, timing;
/* Eased across frames by hd2d_render (scene brightness, ground share). */
static float dark_amt = 0.5f, field_amt = 1.0f;

static double last_ms;

void hd2d_set_preset(int p) { cur_preset = p < 0 || p >= HD2D_PRESETS ? HD2D_FULL : p; }
int hd2d_preset(void) { return cur_preset; }
const char *hd2d_preset_name(int p) { return p >= 0 && p < HD2D_PRESETS ? preset_names[p] : "?"; }
void hd2d_set_debug(int v) { debug_view = v < 0 || v >= HD2D_DEBUGS ? 0 : v; }
int hd2d_debug(void) { return debug_view; }
void hd2d_set_aspect(int a) { aspect_mode = a < 0 || a >= HD2D_ASPECTS ? 0 : a; }
/* port: the depth of field's strength, a setting: off, low, full. The PS5
   starts with it off (the blur at the top and bottom read as a fault on a
   TV, menus included). */
#ifdef CT_PS5
static int dof_level;
#else
static int dof_level = 2;
#endif
void hd2d_set_dof(int level) { dof_level = level < 0 ? 0 : level > 2 ? 2 : level; }
int hd2d_dof(void) { return dof_level; }
static int smooth_3d = 1;   /* port: pixel-art smoothing in the 3D view */
void hd2d_set_smooth(int on) { smooth_3d = on != 0; }
int hd2d_smooth(void) { return smooth_3d; }
int hd2d_aspect(void) { return aspect_mode; }
void hd2d_set_timing(int on) { timing = on; }
int hd2d_wants_scene(void) { return presets[cur_preset].scene3d; }
double hd2d_last_ms(void) { return last_ms; }

/* The 3D diorama's quality, as set (0 / -1: the preset's). */
static float user_render_scale, user_sharpen = -1.0f, user_ao = -1.0f;
static int user_ao_div;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
void hd2d_set_render_scale(float v) { user_render_scale = v > 0.0f ? clampf(v, 0.5f, 1.0f) : 0.0f; }
float hd2d_render_scale(void) { return user_render_scale; }
void hd2d_set_sharpen(float v) { user_sharpen = v < 0.0f ? -1.0f : clampf(v, 0.0f, 1.0f); }
float hd2d_sharpen(void) { return user_sharpen; }
void hd2d_set_ao(float v) { user_ao = v < 0.0f ? -1.0f : clampf(v, 0.0f, 1.0f); }
float hd2d_ao(void) { return user_ao; }
void hd2d_set_ao_size(int div) { user_ao_div = div == 2 || div == 4 ? div : 0; }
int hd2d_ao_size(void) { return user_ao_div; }

/* ---- GL objects ---- */

enum { P_EMIT, P_LIGHT, P_COC, P_DOFBLUR, P_BRIGHT, P_GAUSS, P_FINAL, P_COUNT,
       /* the 3D diorama's: its surfaces, their shadow map, the UI over them,
          and the picture's passes again with scene3d_pic.frag for upscale.frag */
       P3_SCENE = P_COUNT, P3_SHADOW, P3_UI, P3_COC, P3_BRIGHT, P3_FINAL,
       /* its ambient occlusion and blur, and FSR 1's EASU */
       P3_AOPREP, P3_AO, P3_AOBLUR, P3_EASU,
       /* the world map's surfaces and shadow map (WORLD_MAP) */
       P3_WSCENE, P3_WSHADOW,
       /* port: asset packs' models (MODEL) */
       P3_MSCENE, P3_MSHADOW,
       /* port: the 3D target's clear, drawn (see s3d_render) */
       P3_CLEAR, P_ALL };

typedef struct {
    GLuint tex, fbo;
    int w, h;
} target;

static struct {
    int gles, ready;
    GLenum float_fmt, float_type;   /* RGBA16F if renderable, else RGBA8 */
    GLuint prog[P_ALL];
    GLuint vao;
    GLuint t_color, t_layer;        /* uploads, source size */
    GLuint t_behind, t_blayer;      /* the behind G-buffer, source size */
    GLuint t_lit, t_mat, t_back, fbo_light; /* pass 1 targets, source size */
    target half[2], fgh[2];         /* pass 3: background, foreground plane */
    GLuint fbo_dof[2];              /* half[k] and fgh[k] */
    target quarter[2], eighth[2], soft[2], out;
    GLuint fbo_quarter;             /* quarter[0] (bloom) and [1] (plain) */
    target emit[2];                 /* point-light map, source / 4 */
    int pic_w, pic_h, out_w, out_h;
    float focus_y;
    unsigned frame;
    GLuint t_text;                  /* port: HD text overlay (hd2d_set_text_overlay) */
    GLuint t_panel;                 /* port: remastered menus' mask (hd2d_set_ui_panels) */
    unsigned text_gen;              /* what t_text holds */
    int text_w, text_h;
    /* On-screen message: text, frames left, and its mask texture. */
    char osd_text[160];   /* port: room for [token] glyphs */
    int osd_frames, osd_dirty, osd_w, osd_h;
    GLuint t_osd;
    /* The 3D diorama (scene3d.h): ok once its programs built. */
    struct {
        int ok;
        const ct_scene *scene;      /* hd2d_set_scene's, for this render */
        float amt;                  /* 0: the 2D path, 1: 3D; eased between (crossfade) */
        GLuint t_vram, t_cgram;     /* the console's VRAM (R16UI 256x128) and CGRAM */
        GLuint vao[2], vbo[2], ebo[2];   /* 0: the map, 1: this frame's sprites */
        int map_ni, map_ni_floor, spr_ni, spr_ni_low, blob_ni;   /* port: low: under OAM priority 3 */
        uint32_t map_gen;           /* scene generation the map mesh is from */
        int map_cards;              /* port: the style it was built in */
        float map_tilt;             /* port: and the tilt (the cards' shift) */
        int map_mode;               /* and its mode (field, world) */
        uint32_t empty_hash;        /* and which BG tiles were empty then */
        int have_map;
        s3d_mesh map, spr;
        GLuint t_color, t_mat, t_depth, fbo;   /* render size (the picture's x render scale) */
        int w, h;
        /* Below a render scale of 1: the picture-size target EASU fills
           and the UI goes over (RGB10_A2: fsr_easu.frag's encoding). */
        GLuint t_up, fbo_up;
        int up_w, up_h;
        target ao[3];                 /* ambient occlusion, the blur's scratch, its input */
        int pic_w, pic_h, scaled;     /* the last 3D frame: its picture size, upscaled */
        float ao_amt;                 /* and its occlusion strength */
        GLuint t_shadow, fbo_shadow;
        float dtilt, dyaw;   /* port: hd2d_set_camera_offset (gyro), degrees */
        float ao_radius;                  /* ambient occlusion's reach, map px */
        float tilt, fov, zoom, stretch;   /* camera: degrees from straight down, vertical fov,
                                             zoom; sprites' height scale */
        int hmax;
        /* The world map (CT_SCENE_WORLD, scene3d_world.c): the ground's
           normals, the overlay's tilemap; its own camera; entrance marks. */
        GLuint t_world_nrm, t_overlay;
        int cloud_pal;   /* port: the overlay's clouds' palette, -1 any */
        /* Console pixels the scene doesn't hold (s3d_box_mask): R8UI, the
           console's size; laid flat over the 3D picture with the UI. */
        GLuint t_box;
        float wtilt, wzoom;
        int marks;
        s3d_focus_state focus;   /* cutscene cuts and the 12-frame focus pull */
    } s3;
    /* port: asset packs: the atlas (array texture), its entries (two
       RGBA16UI texels each, 1024 entries a row), the per-pixel map. */
    GLuint t_pack_atlas, t_pack_rects, t_pack_map;
    int pack_entries;
    const uint16_t *pack_map;
    int pack_on, pack_have_behind;   /* this frame */
    /* 3D: (VRAM address / 8, palette + 8 for sprites) -> entry + 1 */
    hd2d_tile_lookup pack_lookup;
    void *pack_lookup_user;
    GLuint t_pack_lut;
    int pack3d_on;                   /* this frame */
    /* port: asset packs' models (hd2d_pack_models): the source, and what
       it gave for the current map (one buffer, its parts, textures). */
    hd2d_model_fn model_fn;
    void *model_user;
    unsigned model_gen, model_built;  /* hd2d_pack_models calls; the map's models are from */
    GLuint m_vao, m_vbo, m_ebo;
    GLuint *m_tex;
    int m_ntex, m_ni;
    hd2d_mpart *m_part;
    int m_nparts;
    /* Uniform locations, looked up once (key: program, name literal). */
    struct { const char *name; int prog; GLint loc; } uloc[256];
} g;

/* port: a pack's transparent texels show the behind G-buffer's pixel. */
int hd2d_wants_behind(void) { return presets[cur_preset].effects || g.pack_entries > 0; }

static GLuint make_tex(int w, int h, GLint ifmt, GLenum fmt, GLenum type, GLint filter);

/* port: asset packs (hd2d.h). The 3D lookup: (VRAM address / 8, palette
   + 8 for sprites) -> atlas entry + 1. */
enum { PACK_LUT_W = 4096, PACK_LUT_H = 16 };

/* port: asset packs (hd2d.h). */
static void pack_textures_init(void)
{
    static const uint16_t zero[4] = {0, 0, 0, 0};
    static const uint8_t clear[4] = {0, 0, 0, 0};
    g.t_pack_map = make_tex(SCHED_WIDTH, SCHED_HEIGHT, GL_RGBA16UI, GL_RGBA_INTEGER,
                            GL_UNSIGNED_SHORT, GL_NEAREST);
    g.t_pack_rects = make_tex(1, 1, GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, GL_NEAREST);
    g.t_pack_lut = make_tex(PACK_LUT_W, PACK_LUT_H, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT,
                            GL_NEAREST);
    /* (the entries' texture, not the lookup made last: OpenGL ES refuses
       an RGBA_INTEGER upload to an R16UI texture) */
    p_glBindTexture(GL_TEXTURE_2D, g.t_pack_rects);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, zero);
    p_glGenTextures(1, &g.t_pack_atlas);
    p_glBindTexture(GL_TEXTURE_2D_ARRAY, g.t_pack_atlas);
    p_glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, clear);
    p_glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void hd2d_pack_atlas(const uint8_t *rgba, int size, int pages, const int *rects, int n)
{
    if (!g.ready)
        return;
    g.pack_entries = 0;
    if (!rgba || n <= 0 || size <= 0 || pages <= 0)
        return;
    GLint max = 0;
    p_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max);
    if (size > max) {
        fprintf(stderr, "hd2d: pack atlas %d over the GPU's %d texels: not used\n", size, max);
        return;
    }
    /* Entries: texel 2k (page, x, y, w), 2k + 1 (h, src_w, src_h, 0). */
    int rows = (n + 1023) / 1024, cols = n < 1024 ? n * 2 : 2048;
    uint16_t *t = calloc((size_t)rows * cols * 4, sizeof *t);
    if (!t)
        return;
    for (int k = 0; k < n; k++) {
        uint16_t *e = t + ((size_t)(k / 1024) * cols + (k % 1024) * 2) * 4;
        for (int c = 0; c < 7; c++)
            e[c] = (uint16_t)rects[k * 8 + c];
    }
    p_glBindTexture(GL_TEXTURE_2D, g.t_pack_rects);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, cols, rows, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, t);
    free(t);
    int levels = 1;
    while ((size >> levels) >= 64 && levels < 4)
        levels++;
    p_glBindTexture(GL_TEXTURE_2D_ARRAY, g.t_pack_atlas);
    p_glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, size, size, pages, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   rgba);
    p_glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, levels - 1);
    p_glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    p_glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    p_glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    GLenum err = p_glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "hd2d: pack atlas upload failed (GL error 0x%x)\n", err);
        return;
    }
    g.pack_entries = n;
}

void hd2d_pack_map(const uint16_t *map) { g.pack_map = map; }

void hd2d_pack_tiles(hd2d_tile_lookup fn, void *user)
{
    g.pack_lookup = fn;
    g.pack_lookup_user = user;
}

void hd2d_pack_models(hd2d_model_fn fn, void *user)
{
    g.model_fn = fn;
    g.model_user = user;
    g.model_gen++;   /* the map is rebuilt with them at the next 3D frame */
}

static GLint U(int prog, const char *name);
static void bind_tex(int unit, GLuint tex);

/* port: the models' GL objects for this map (none if out has none). */
static void models_free(void)
{
    if (g.m_ntex)
        p_glDeleteTextures(g.m_ntex, g.m_tex);
    free(g.m_tex);
    free(g.m_part);
    g.m_tex = NULL;
    g.m_part = NULL;
    g.m_ntex = g.m_nparts = g.m_ni = 0;
}

static void models_upload(const hd2d_models *m)
{
    models_free();
    if (!m || !m->ni || !m->nv || !m->nparts)
        return;
    g.m_part = malloc(sizeof *g.m_part * (size_t)m->nparts);
    g.m_tex = m->ntex ? calloc((size_t)m->ntex, sizeof *g.m_tex) : NULL;
    if (!g.m_part || (m->ntex && !g.m_tex)) {
        models_free();
        return;
    }
    memcpy(g.m_part, m->part, sizeof *g.m_part * (size_t)m->nparts);
    g.m_nparts = m->nparts;
    if (!g.m_vao) {
        p_glGenVertexArrays(1, &g.m_vao);
        p_glGenBuffers(1, &g.m_vbo);
        p_glGenBuffers(1, &g.m_ebo);
        p_glBindVertexArray(g.m_vao);
        p_glBindBuffer(GL_ARRAY_BUFFER, g.m_vbo);
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.m_ebo);
        p_glEnableVertexAttribArray(0);
        p_glEnableVertexAttribArray(1);
        p_glEnableVertexAttribArray(3);
        p_glEnableVertexAttribArray(4);
        p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(hd2d_mvertex),
                                (const void *)offsetof(hd2d_mvertex, x));
        p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(hd2d_mvertex),
                                (const void *)offsetof(hd2d_mvertex, u));
        p_glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(hd2d_mvertex),
                                (const void *)offsetof(hd2d_mvertex, nx));
        p_glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, 1, sizeof(hd2d_mvertex),
                                (const void *)offsetof(hd2d_mvertex, rgba));
    }
    p_glBindVertexArray(g.m_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.m_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(hd2d_mvertex) * (size_t)m->nv), m->v,
                   GL_STATIC_DRAW);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.m_ebo);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * (size_t)m->ni), m->idx,
                   GL_STATIC_DRAW);
    p_glBindVertexArray(g.vao);
    g.m_ni = m->ni;
    if (m->ntex) {
        p_glGenTextures(m->ntex, g.m_tex);
        g.m_ntex = m->ntex;
    }
    for (int k = 0; k < m->ntex; k++) {
        const hd2d_mtex *t = &m->tex[k];
        p_glBindTexture(GL_TEXTURE_2D, g.m_tex[k]);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->rgba);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        if (t->linear) {
            p_glGenerateMipmap(GL_TEXTURE_2D);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        } else {
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
    }
    p_glBindTexture(GL_TEXTURE_2D, 0);
}

/* port: draw the models (prog P3_MSCENE or P3_MSHADOW), at each of the
   map's copies (offsets: copies, n). */
static void models_draw(int prog, const float (*offs)[2], int n)
{
    if (!g.m_ni)
        return;
    p_glUseProgram(g.prog[prog]);
    p_glUniform1i(U(prog, "uModelTex"), 13);
    p_glBindVertexArray(g.m_vao);
    for (int c = 0; c < n; c++) {
        p_glUniform3f(U(prog, "uOffset"), offs[c][0], 0.0f, offs[c][1]);
        for (int k = 0; k < g.m_nparts; k++) {
            const hd2d_mpart *pt = &g.m_part[k];
            int tex = pt->tex >= 0 && pt->tex < g.m_ntex;
            if (tex)
                bind_tex(13, g.m_tex[pt->tex]);
            p_glUniform2i(U(prog, "uModelMode"), tex, pt->cutout);
            p_glUniform4f(U(prog, "uModelColor"), pt->color[0], pt->color[1], pt->color[2],
                          pt->color[3]);
            p_glDrawElements(GL_TRIANGLES, pt->count, GL_UNSIGNED_INT,
                             (const void *)(sizeof(uint32_t) * (size_t)pt->first));
        }
    }
    p_glBindVertexArray(g.vao);
}

/* The 3D meshes' BG tiles -> atlas entries, for scene3d.frag's tileTexel.
   Every cell a drawn tile reads is filled this frame, so stale cells are
   never read. */
static uint16_t pack_lut[PACK_LUT_W * PACK_LUT_H];
static uint32_t pack_lut_gen[PACK_LUT_W * PACK_LUT_H], pack_gen;

static void pack_lut_tile(uint32_t t, const ct_scene *sc)
{
    int pal = (int)(t >> S3D_PAL_SHIFT & 7), obj = (t & S3D_OBJ) != 0;
    size_t i = (size_t)(pal + obj * 8) * PACK_LUT_W + ((t & S3D_ADDR_MASK) >> 3);
    if (pack_lut_gen[i] == pack_gen)
        return;
    pack_lut_gen[i] = pack_gen;
    int bpp = t & S3D_2BPP ? 2 : 4;
    pack_lut[i] = obj ? 0 : (uint16_t)g.pack_lookup(g.pack_lookup_user, t & S3D_ADDR_MASK,
                                                   pal * (bpp == 2 ? 4 : 16), bpp, sc->vram,
                                                   sc->cgram);
}

static void pack_lut_add(const s3d_mesh *m, const ct_scene *sc)
{
    for (int k = 0; k < m->nv; k += 4)
        pack_lut_tile(m->v[k].tile, sc);
    /* The world map's overlay (clouds), which the shader looks up itself. */
    const ct_scene_layer *L = &sc->layer[2];
    if (sc->mode == CT_SCENE_WORLD && L->present && L->tiles)
        for (int y = 0; y < sc->overlay_h && y < 128; y++)
            for (int x = 0; x < sc->overlay_w && x < CT_SCENE_WORLD_STRIDE; x++) {
                uint16_t e = L->tiles[y * CT_SCENE_WORLD_STRIDE + x];
                pack_lut_tile(((L->chr_base + (e & 0x3FFu) * (L->bpp == 2 ? 8u : 16u)) &
                               S3D_ADDR_MASK) | (uint32_t)((e >> 10) & 7) << S3D_PAL_SHIFT |
                                  (L->bpp == 2 ? S3D_2BPP : 0),
                              sc);
            }
}

static GLuint make_tex(int w, int h, GLint ifmt, GLenum fmt, GLenum type, GLint filter);
static int s3d_init(void);
static void s3d_shutdown(void);

void hd2d_set_scene(const ct_scene *scene) { g.s3.scene = scene; }

/* port: the user's 3D camera (hd2d_set_camera), kept apart from g, which
   hd2d_init clears; s3d_params_init starts from it. */
static float user_tilt = 40.0f, user_zoom = 0.75f;
static int user_cards = 1;   /* port: hd2d_set_3d_style */
static int shadows_3d = 1;   /* port: hd2d_set_3d_shadows */
static int depth_3d = 1;     /* port: hd2d_set_3d_depth: 0 full, 1 subtle, 2 flat */
void hd2d_set_3d_depth(int d) { depth_3d = d < 0 ? 0 : d > 2 ? 2 : d; }
int hd2d_3d_depth(void) { return depth_3d; }
void hd2d_set_3d_shadows(int on) { shadows_3d = on != 0; }
int hd2d_3d_shadows(void) { return shadows_3d; }
void hd2d_set_3d_style(int cards) { user_cards = cards != 0; }
int hd2d_3d_style(void) { return user_cards; }

void hd2d_set_camera(float tilt, float zoom)
{
    user_tilt = tilt < 0 ? 0 : tilt > 80 ? 80 : tilt;
    user_zoom = zoom < 0.25f ? 0.25f : zoom > 4 ? 4 : zoom;
    g.s3.tilt = user_tilt;
    g.s3.zoom = user_zoom;
}

void hd2d_camera(float *tilt, float *zoom)
{
    *tilt = user_tilt;
    *zoom = user_zoom;
}

/* port: the world map's camera (hd2d_set_world_camera), as above. */
static float user_wtilt = 40.0f, user_wzoom = 0.75f;

void hd2d_set_world_camera(float tilt, float zoom)
{
    user_wtilt = tilt < 0 ? 0 : tilt > 80 ? 80 : tilt;
    user_wzoom = zoom < 0.25f ? 0.25f : zoom > 4 ? 4 : zoom;
    g.s3.wtilt = user_wtilt;
    g.s3.wzoom = user_wzoom;
}

void hd2d_world_camera(float *tilt, float *zoom)
{
    *tilt = user_wtilt;
    *zoom = user_wzoom;
}

void hd2d_set_camera_offset(float dtilt, float dyaw)
{
    float m = HD2D_CAMERA_MAX;
    g.s3.dtilt = dtilt < -m ? -m : dtilt > m ? m : dtilt;
    g.s3.dyaw = dyaw < -m ? -m : dyaw > m ? m : dyaw;
}

#define OSD_FRAMES 130   /* about two seconds, the last 20 fading */

void hd2d_osd(const char *text)
{
    snprintf(g.osd_text, sizeof g.osd_text, "%s", text);
    g.osd_frames = OSD_FRAMES;
    g.osd_dirty = 1;
}

/* port: HD text (hd2d.h): the frontend's overlay, kept here (hd2d_init
   clears g) and uploaded to g.t_text when its generation changes. */
static struct {
    const uint8_t *rgba;
    int w, h;
    float x, y, cw, ch;
    unsigned gen;
} text_ov;

void hd2d_set_text_overlay(const uint8_t *rgba, int w, int h, float x, float y, float cw,
                           float ch, unsigned gen)
{
    text_ov.rgba = w > 0 && h > 0 ? rgba : NULL;
    text_ov.w = w;
    text_ov.h = h;
    text_ov.x = x;
    text_ov.y = y;
    text_ov.cw = cw;
    text_ov.ch = ch;
    text_ov.gen = gen;
}

/* port: remastered menus (hd2d_set_ui_panels) */
#define UI_PANELS 16
static struct {
    uint8_t mask[SCHED_WIDTH * SCHED_HEIGHT];
    float rect[UI_PANELS][4];
    int n, style, dirty;
} ui_pan;

void hd2d_set_ui_panels(const uint8_t *mask, const float (*rects)[4], int n, int style)
{
    n = n < 0 ? 0 : n > UI_PANELS ? UI_PANELS : n;
    ui_pan.style = style;
    ui_pan.n = style && mask ? n : 0;
    if (ui_pan.n) {
        memcpy(ui_pan.mask, mask, sizeof ui_pan.mask);
        memcpy(ui_pan.rect, rects, sizeof(float) * 4 * (size_t)n);
        ui_pan.dirty = 1;
    }
}

static void ui_panels_bind(int pf)
{
    if (!g.t_panel)
        g.t_panel = make_tex(SCHED_WIDTH, SCHED_HEIGHT, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST);
    if (ui_pan.n && ui_pan.dirty) {
        p_glBindTexture(GL_TEXTURE_2D, g.t_panel);
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCHED_WIDTH, SCHED_HEIGHT, GL_RED,
                          GL_UNSIGNED_BYTE, ui_pan.mask);
        ui_pan.dirty = 0;
    }
    p_glUniform1i(U(pf, "uPanelMask"), 14);
    bind_tex(14, g.t_panel);
    p_glUniform1i(U(pf, "uPanelN"), debug_view ? 0 : ui_pan.n);
    p_glUniform1i(U(pf, "uPanelStyle"), ui_pan.style);
    if (ui_pan.n)
        p_glUniform4fv(U(pf, "uPanel"), ui_pan.n, &ui_pan.rect[0][0]);
}

static void text_upload(void)
{
    if (!text_ov.rgba || (g.t_text && g.text_gen == text_ov.gen))
        return;
    if (!g.t_text || g.text_w != text_ov.w || g.text_h != text_ov.h) {
        if (g.t_text)
            p_glDeleteTextures(1, &g.t_text);
        g.t_text = make_tex(text_ov.w, text_ov.h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
        g.text_w = text_ov.w;
        g.text_h = text_ov.h;
    }
    p_glBindTexture(GL_TEXTURE_2D, g.t_text);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, text_ov.w, text_ov.h, GL_RGBA, GL_UNSIGNED_BYTE,
                      text_ov.rgba);
    g.text_gen = text_ov.gen;
}

/* Rasterize the message into its mask texture (1 = text), with button
   glyphs (port: osd_text.h; OSD_OS texels per font pixel). */
static void osd_upload(void)
{
    uint8_t *mask;
    int w, h;
    g.osd_dirty = 0;
    if (osd_text_mask(g.osd_text, &mask, &w, &h))
        return;
    g.osd_w = w / OSD_OS;   /* units: the rectangle on screen */
    g.osd_h = h / OSD_OS;
    if (g.t_osd)
        p_glDeleteTextures(1, &g.t_osd);
    g.t_osd = make_tex(w, h, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, mask);
    free(mask);
}

/* lib: an optional source compiled in front of body (upscale.frag). */
static GLuint compile(GLenum type, const char *lib, const char *body, const char *name)
{
    const char *prefix = g.gles ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                                  "precision highp usampler2D;\n"
#ifdef CT_PS5   /* port: the console's cheaper paths in the shaders */
                                : "#version 330 core\n#define CT_FAST 1\n";
#else
                                : "#version 330 core\n";
#endif
    const char *src[3] = {prefix, lib ? lib : "", body};
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 3, src, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "hd2d: %s: compile failed:\n%s\n", name, log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_vs(const char *vert, const char *lib, const char *frag, const char *name)
{
    GLuint vs = compile(GL_VERTEX_SHADER, NULL, vert, name);
    GLuint fs = compile(GL_FRAGMENT_SHADER, lib, frag, name);
    if (!vs || !fs)
        return 0;
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, vs);
    p_glAttachShader(p, fs);
    p_glLinkProgram(p);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "hd2d: %s: link failed:\n%s\n", name, log);
        p_glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint link(const char *lib, const char *frag, const char *name)
{
    return link_vs(hd2d_src_fullscreen_vert, lib, frag, name);
}

static GLuint make_tex(int w, int h, GLint ifmt, GLenum fmt, GLenum type, GLint filter)
{
    GLuint t;
    p_glGenTextures(1, &t);
    p_glBindTexture(GL_TEXTURE_2D, t);
    p_glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static int fbo_for(GLuint *fbo, const GLuint *tex, int n)
{
    static const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                                   GL_COLOR_ATTACHMENT2};
    p_glGenFramebuffers(1, fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    for (int k = 0; k < n; k++)
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, bufs[k], GL_TEXTURE_2D, tex[k], 0);
    p_glDrawBuffers(n, bufs);
    int ok = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok ? 0 : -1;
}

static void target_free(target *t)
{
    if (t->fbo)
        p_glDeleteFramebuffers(1, &t->fbo);
    if (t->tex)
        p_glDeleteTextures(1, &t->tex);
    memset(t, 0, sizeof *t);
}

static int target_make(target *t, int w, int h, int fp)
{
    target_free(t);
    t->w = w < 1 ? 1 : w;
    t->h = h < 1 ? 1 : h;
    t->tex = fp ? make_tex(t->w, t->h, (GLint)g.float_fmt, GL_RGBA, g.float_type, GL_LINEAR)
                : make_tex(t->w, t->h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
    return fbo_for(&t->fbo, &t->tex, 1);
}

int hd2d_init(int gles)
{
    memset(&g, 0, sizeof g);
    g.gles = gles;
    g.focus_y = 0.55f;
    if (load_gl())
        return -1;
    fprintf(stderr, "hd2d: %s / %s / %s\n", p_glGetString(GL_VENDOR), p_glGetString(GL_RENDERER),
            p_glGetString(GL_VERSION));
    prof_init(gles);

    /* up: the pass reads the picture, so upscale.frag goes in front. */
    static const struct { const char *src, *name; int up; } fs[P_COUNT] = {
        [P_EMIT] = {hd2d_src_emit_frag, "emit.frag", 0},
        [P_LIGHT] = {hd2d_src_light_frag, "light.frag", 0},
        [P_COC] = {hd2d_src_coc_frag, "coc.frag", 1},
        [P_DOFBLUR] = {hd2d_src_dofblur_frag, "dofblur.frag", 0},
        [P_BRIGHT] = {hd2d_src_bright_frag, "bright.frag", 1},
        [P_GAUSS] = {hd2d_src_gauss_frag, "gauss.frag", 0},
        [P_FINAL] = {hd2d_src_final_frag, "final.frag", 1},
    };
    for (int k = 0; k < P_COUNT; k++)
        if (!(g.prog[k] = link(fs[k].up ? hd2d_src_upscale_frag : NULL, fs[k].src, fs[k].name)))
            return -1;

    p_glGenVertexArrays(1, &g.vao);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 1);

    g.t_color = make_tex(SCHED_WIDTH, SCHED_HEIGHT, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST);
    g.t_layer = make_tex(SCHED_WIDTH, SCHED_HEIGHT, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                         GL_NEAREST);
    g.t_behind = make_tex(SCHED_WIDTH, SCHED_HEIGHT, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST);
    g.t_blayer = make_tex(SCHED_WIDTH, SCHED_HEIGHT, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                          GL_NEAREST);

    /* Half-float targets where they render (desktop GL 3.3 always; ES 3.0
       with EXT_color_buffer_float), else 8-bit. */
    g.float_fmt = GL_RGBA16F;
    g.float_type = GL_HALF_FLOAT;
    g.t_lit = make_tex(SCHED_WIDTH, SCHED_HEIGHT, (GLint)g.float_fmt, GL_RGBA, g.float_type,
                       GL_LINEAR);
    g.t_mat = make_tex(SCHED_WIDTH, SCHED_HEIGHT, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST);
    g.t_back = make_tex(SCHED_WIDTH, SCHED_HEIGHT, (GLint)g.float_fmt, GL_RGBA, g.float_type,
                        GL_LINEAR);
    GLuint lt[3] = {g.t_lit, g.t_mat, g.t_back};
    if (fbo_for(&g.fbo_light, lt, 3)) {
        fprintf(stderr, "hd2d: half-float targets not renderable: using RGBA8\n");
        p_glDeleteFramebuffers(1, &g.fbo_light);
        p_glDeleteTextures(1, &g.t_lit);
        p_glDeleteTextures(1, &g.t_back);
        g.float_fmt = GL_RGBA8;
        g.float_type = GL_UNSIGNED_BYTE;
        g.t_lit = make_tex(SCHED_WIDTH, SCHED_HEIGHT, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
        g.t_back = make_tex(SCHED_WIDTH, SCHED_HEIGHT, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
        lt[0] = g.t_lit;
        lt[2] = g.t_back;
        if (fbo_for(&g.fbo_light, lt, 3)) {
            fprintf(stderr, "hd2d: cannot make the lighting target\n");
            return -1;
        }
    }
    for (int k = 0; k < 2; k++)
        if (target_make(&g.emit[k], SCHED_WIDTH / 4, SCHED_HEIGHT / 4, 1)) {
            fprintf(stderr, "hd2d: cannot make the light map\n");
            return -1;
        }
    GLenum err = p_glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "hd2d: GL error 0x%x during setup\n", err);
        return -1;
    }
    pack_textures_init();   /* port */
    err = p_glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "hd2d: GL error 0x%x making the pack textures\n", err);
        return -1;
    }
    g.ready = 1;
    /* The 3D diorama: optional (its preset falls back to the 2D path). */
    if (s3d_init()) {
        fprintf(stderr, "hd2d: the 3D diorama is unavailable here\n");
        while (p_glGetError() != GL_NO_ERROR) {
        }
    }
    return 0;
}

/* Drivers (Mesa among them) build a program's final GPU code at its first
   draw, and the window-size targets are made on the first render: done on
   frame 1 that is a stall long enough to drain the audio queue. One
   throwaway render of a blank frame per preset forces both here, before
   any audio is queued. The eased state is put back, so the first real
   frame is as it would have been. */
void hd2d_warmup(int ow, int oh)
{
    if (!g.ready)
        return;
    static uint8_t blank[SCHED_WIDTH * SCHED_HEIGHT * 4], blank_layers[SCHED_WIDTH * SCHED_HEIGHT];
    float d = dark_amt, f = field_amt, fy = g.focus_y;
    unsigned fr = g.frame;
    int keep = cur_preset;
    int order[2] = {keep, HD2D_FULL};
    for (int k = 0; k < (keep == HD2D_FULL ? 1 : 2); k++) {
        cur_preset = order[k];
        hd2d_render(blank, blank_layers, blank, blank_layers, NULL, ow, oh, 0);
    }
    cur_preset = keep;
    p_glFinish();
    dark_amt = d;
    field_amt = f;
    g.focus_y = fy;
    g.frame = fr;
}

void hd2d_shutdown(void)
{
    if (!g.ready)
        return;
    if (prof.f) {
        for (int k = 1; k <= PROF_RING; k++)
            prof_flush((prof.slot + k) % PROF_RING);
        prof.del(PROF_RING * PROF_MARKS, &prof.q[0][0]);
        fclose(prof.f);
        prof.f = NULL;
    }
    target *ts[] = {&g.half[0], &g.half[1], &g.fgh[0], &g.fgh[1],
                    &g.quarter[0], &g.quarter[1], &g.eighth[0], &g.eighth[1], &g.soft[0],
                    &g.soft[1], &g.out, &g.emit[0], &g.emit[1]};
    for (size_t k = 0; k < sizeof ts / sizeof *ts; k++)
        target_free(ts[k]);
    GLuint fbos[3] = {g.fbo_quarter, g.fbo_dof[0], g.fbo_dof[1]};
    p_glDeleteFramebuffers(3, fbos);
    GLuint tex[7] = {g.t_color, g.t_layer, g.t_behind, g.t_blayer, g.t_lit, g.t_mat, g.t_back};
    p_glDeleteTextures(7, tex);
    GLuint ptex[4] = {g.t_pack_atlas, g.t_pack_rects, g.t_pack_map, g.t_pack_lut};   /* port */
    p_glDeleteTextures(4, ptex);
    g.pack_entries = 0;
    if (g.t_osd)
        p_glDeleteTextures(1, &g.t_osd);
    if (g.t_text)   /* port: HD text */
        p_glDeleteTextures(1, &g.t_text);
    if (g.t_panel)
        p_glDeleteTextures(1, &g.t_panel);
    p_glDeleteFramebuffers(1, &g.fbo_light);
    for (int k = 0; k < P_ALL; k++)
        p_glDeleteProgram(g.prog[k]);
    p_glDeleteVertexArrays(1, &g.vao);
    s3d_shutdown();
    g.ready = 0;
}

/* The picture's rectangle inside the output, top-left origin. */
static void picture_rect(int ow, int oh, int *x, int *y, int *w, int *h)
{
    double aspect = aspect_mode == HD2D_ASPECT_PIXEL ? (SCHED_WIDTH * 8.0 / 7.0) / SCHED_HEIGHT
                    : aspect_mode == HD2D_ASPECT_STRETCH ? (double)ow / oh
                                                         : 4.0 / 3.0;
    *h = oh;
    *w = (int)lround(oh * aspect);
    if (*w > ow) {
        *w = ow;
        *h = (int)lround(ow / aspect);
    }
    *x = (ow - *w) / 2;
    *y = (oh - *h) / 2;
}

static int resize(int ow, int oh, int pw, int ph)
{
    if (pw != g.pic_w || ph != g.pic_h) {
        g.pic_w = pw;
        g.pic_h = ph;
        int ok = 1;
        for (int k = 0; k < 2; k++) {
            ok &= !target_make(&g.half[k], pw / 2, ph / 2, 1);
            ok &= !target_make(&g.fgh[k], pw / 2, ph / 2, 1);
            ok &= !target_make(&g.quarter[k], pw / 4, ph / 4, 1);
            ok &= !target_make(&g.eighth[k], pw / 8, ph / 8, 1);
            ok &= !target_make(&g.soft[k], pw / 8, ph / 8, 1);
        }
        /* Two-target framebuffers for passes 3 and 4a. */
        GLuint fbos[3] = {g.fbo_quarter, g.fbo_dof[0], g.fbo_dof[1]};
        if (g.fbo_quarter)
            p_glDeleteFramebuffers(3, fbos);
        GLuint two[2] = {g.quarter[0].tex, g.quarter[1].tex};
        ok &= !fbo_for(&g.fbo_quarter, two, 2);
        for (int k = 0; k < 2; k++) {
            two[0] = g.half[k].tex;
            two[1] = g.fgh[k].tex;
            ok &= !fbo_for(&g.fbo_dof[k], two, 2);
        }
        if (!ok) {
            fprintf(stderr, "hd2d: cannot make %dx%d targets\n", pw, ph);
            return -1;
        }
    }
    if (ow != g.out_w || oh != g.out_h) {
        g.out_w = ow;
        g.out_h = oh;
        if (target_make(&g.out, ow, oh, 0)) {
            fprintf(stderr, "hd2d: cannot make the %dx%d output target\n", ow, oh);
            return -1;
        }
    }
    return 0;
}

/* Uniform location, cached: names are string literals, so the pointer is
   the key. */
static GLint U(int prog, const char *name)
{
    enum { N = sizeof g.uloc / sizeof *g.uloc };
    unsigned h = (unsigned)(((uintptr_t)name >> 2) * 2654435761u + (unsigned)prog * 40503u) % N;
    for (int probe = 0; probe < N; probe++, h = (h + 1) % N) {
        if (g.uloc[h].name == name && g.uloc[h].prog == prog)
            return g.uloc[h].loc;
        if (!g.uloc[h].name) {
            g.uloc[h].name = name;
            g.uloc[h].prog = prog;
            return g.uloc[h].loc = p_glGetUniformLocation(g.prog[prog], name);
        }
    }
    return p_glGetUniformLocation(g.prog[prog], name);
}

static void bind_tex(int unit, GLuint tex)
{
    p_glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    p_glBindTexture(GL_TEXTURE_2D, tex);
}

static void draw_to(GLuint fbo, int w, int h)
{
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glViewport(0, 0, w, h);
    p_glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* Separable Gaussian: src -> tmp (H) -> dst (V). */
static void gauss(const target *src, target *tmp, target *dst)
{
    p_glUseProgram(g.prog[P_GAUSS]);
    p_glUniform1i(U(P_GAUSS, "uSrc"), 0);
    bind_tex(0, src->tex);
    p_glUniform2f(U(P_GAUSS, "uDir"), 1.0f / src->w, 0.0f);
    draw_to(tmp->fbo, tmp->w, tmp->h);
    bind_tex(0, tmp->tex);
    p_glUniform2f(U(P_GAUSS, "uDir"), 0.0f, 1.0f / tmp->h);
    draw_to(dst->fbo, dst->w, dst->h);
}

/* The upscale's inputs (upscale.frag) for a pass that reads the picture:
   the source-size targets on units 0-2; in the 3D path (scene3d_pic.frag)
   the 3D render's picture-size ones. */
static int pic3d;

static void bind_picture(int prog, int pw, int ph, float tilt)
{
    p_glUniform1i(U(prog, "uLit"), 0);
    p_glUniform1i(U(prog, "uLitBack"), 1);
    p_glUniform1i(U(prog, "uMat"), 2);
    p_glUniform2f(U(prog, "uScale"), (float)pw / SCHED_WIDTH, (float)ph / SCHED_HEIGHT);
    p_glUniform2f(U(prog, "uPic"), (float)pw, (float)ph);
    p_glUniform3f(U(prog, "uTilt"), 0.5f, g.focus_y, tilt);
    /* The 3D picture (scene3d_pic.frag): below a render scale of 1, EASU's
       picture and the render-size targets for the depth and material. */
    bind_tex(0, pic3d ? (g.s3.scaled ? g.s3.t_up : g.s3.t_color) : g.t_lit);
    bind_tex(1, pic3d ? g.s3.t_color : g.t_back);
    bind_tex(2, pic3d ? g.s3.t_mat : g.t_mat);
    if (pic3d) {   /* the ambient occlusion (unit 14) at the native render scale (else
                      EASU applied it), not for the bloom source; sharpening off
                      (the final pass turns it on) */
        p_glUniform1i(U(prog, "uUp"), g.s3.scaled);
        p_glUniform1i(U(prog, "uPicDepth"), debug_view == HD2D_DEBUG_DEPTH);
        p_glUniform1i(U(prog, "uAoTex"), 14);
        p_glUniform1f(U(prog, "uAo"), g.s3.scaled || prog == P3_BRIGHT ? 0.0f : g.s3.ao_amt);
        p_glUniform2f(U(prog, "uSharp"), 0.0f, 0.0f);
        bind_tex(14, g.s3.ao[0].tex ? g.s3.ao[0].tex : g.t_mat);
    }
    /* port: asset packs, units 10-13 (always valid textures there: the
       samplers' types differ from unit 0's) */
    p_glUniform1i(U(prog, "uPack"), g.pack_on && !pic3d);   /* the map is 2D */
    p_glUniform1i(U(prog, "uPackMap"), 10);
    p_glUniform1i(U(prog, "uPackRects"), 11);
    p_glUniform1i(U(prog, "uPackAtlas"), 12);
    p_glUniform1i(U(prog, "uPackBehind"), 13);
    p_glUniform2f(U(prog, "uPackParams"), 0.0f, (float)g.pack_have_behind);
    bind_tex(10, g.t_pack_map);
    bind_tex(11, g.t_pack_rects);
    p_glActiveTexture(GL_TEXTURE0 + 12);
    p_glBindTexture(GL_TEXTURE_2D_ARRAY, g.t_pack_atlas);
    bind_tex(13, g.t_behind);
}

/* Bloom source from a smaller target (threshold < 0: a plain copy). */
static void bright(const target *src, target *dst, float threshold)
{
    p_glUseProgram(g.prog[P_BRIGHT]);
    p_glUniform1i(U(P_BRIGHT, "uFromPicture"), 0);
    p_glUniform1i(U(P_BRIGHT, "uSrc"), 3);
    p_glUniform1i(U(P_BRIGHT, "uMat"), 2);
    p_glUniform1f(U(P_BRIGHT, "uThreshold"), threshold);
    bind_tex(3, src->tex);
    bind_tex(2, g.t_mat);
    draw_to(dst->fbo, dst->w, dst->h);
}

/* Scene analysis on the CPU: which z level is UI, how much of the screen
   is UI (menus), and where the actors are (the focus line). */
static void analyze(const uint8_t *bgrx, const uint8_t *layers, const sched_ppu_state *st,
                    int *ui_z, float *ui_frac, float *field_frac, float *focus_target,
                    float *mean_luma)
{
    /* Mean brightness of the picture, every 4th pixel: channel sums in
       integers, weighted once at the end. */
    uint32_t sb = 0, sg = 0, sr = 0;
    for (int k = 0; k < SCHED_WIDTH * SCHED_HEIGHT; k += 4) {
        sb += bgrx[k * 4];
        sg += bgrx[k * 4 + 1];
        sr += bgrx[k * 4 + 2];
    }
    double lsum = 0.114 * sb + 0.587 * sg + 0.299 * sr;
    *mean_luma = (float)(lsum / (SCHED_WIDTH * SCHED_HEIGHT / 4) / 255.0);

    /* Weight actors near the middle: the camera follows the lead. */
    static float colw[SCHED_WIDTH];
    if (colw[0] == 0.0f)
        for (int x = 0; x < SCHED_WIDTH; x++) {
            double dx = (x - SCHED_WIDTH / 2) / (double)(SCHED_WIDTH / 2);
            colw[x] = (float)(1.0 / (1.0 + 4.0 * dx * dx));
        }
    /* What each layer byte counts as, packed so one add per pixel counts
       all three: UI in bits 0-10, ground and foreground planes in 11-21,
       actors in 22-31 (a row has at most 256 of each); and the actor mask
       as a float, for the weighted sum. */
    *ui_z = st && st->mode == 1 && st->bg3_priority ? 15 : -1;
    uint32_t pack[256];
    float actor[256];
    for (int v = 0; v < 256; v++) {
        int l = v & 7, z = v >> 4;
        pack[v] = *ui_z >= 0 && z == *ui_z && l < 4 ? 1u : l < 4 && z >= 4 ? 1u << 11
                  : l == 4 || l == 6 ? 1u << 22 : 0u;
        actor[v] = pack[v] == 1u << 22 ? 1.0f : 0.0f;
    }
    long n[4] = {0};
    double ysum = 0, wsum = 0;
    for (int y = 0; y < SCHED_HEIGHT; y++) {
        const uint8_t *row = layers + y * SCHED_WIDTH;
        uint32_t acc = 0;
        float r0 = 0, r1 = 0;   /* two sums: half the chain of dependent adds */
        for (int x = 0; x < SCHED_WIDTH; x += 2) {
            acc += pack[row[x]] + pack[row[x + 1]];
            r0 += colw[x] * actor[row[x]];
            r1 += colw[x + 1] * actor[row[x + 1]];
        }
        float rw = r0 + r1;
        n[1] += acc & 0x7ff;
        n[2] += acc >> 11 & 0x7ff;
        n[3] += acc >> 22;
        ysum += (double)rw * y;
        wsum += rw;
    }
    *ui_frac = (float)n[1] / (SCHED_WIDTH * SCHED_HEIGHT);
    *field_frac = (float)n[2] / (SCHED_WIDTH * SCHED_HEIGHT);
    *focus_target = n[3] > 24 ? (float)(ysum / wsum / SCHED_HEIGHT) : 0.55f;
}

/* 1: light, at the console's size (the 2D path). */
static void light_pass(const settings *s, int ui_z, float scene_amt)
{
    p_glUseProgram(g.prog[P_LIGHT]);
    p_glUniform1i(U(P_LIGHT, "uColor"), 0);
    p_glUniform1i(U(P_LIGHT, "uLayer"), 1);
    p_glUniform1i(U(P_LIGHT, "uUiZ"), ui_z);
    float a = s->effects ? scene_amt : 0.0f;
    p_glUniform4f(U(P_LIGHT, "uAmount"), s->relief * a, s->shadow * a, s->ao * a, s->rim * a);
    p_glUniform4f(U(P_LIGHT, "uAmount2"), s->haze * a, s->point_light * a * (0.25f + 0.75f * dark_amt),
                  0, 0);
    p_glUniform1i(U(P_LIGHT, "uLightMap"), 2);
    p_glUniform1i(U(P_LIGHT, "uBehind"), 3);
    p_glUniform1i(U(P_LIGHT, "uBLayer"), 4);
    bind_tex(2, g.emit[0].tex);
    p_glUniform3f(U(P_LIGHT, "uLightDir"), -0.5f, -0.65f, 0.58f);
    p_glUniform3f(U(P_LIGHT, "uKey"), 0.55f, 0.51f, 0.44f);
    p_glUniform3f(U(P_LIGHT, "uAmbient"), 0.47f, 0.5f, 0.58f);
    p_glUniform3f(U(P_LIGHT, "uHaze"), 0.75f, 0.82f, 0.95f);
    p_glUniform2i(U(P_LIGHT, "uShadowOffset"), 3, 4);
    draw_to(g.fbo_light, SCHED_WIDTH, SCHED_HEIGHT);
    prof_mark("light");
}

/* 2-5: the passes that read the picture, from the 2D path's source-size
   targets or (threeD) the 3D render's; with blend in (0, 1) the final
   pass is mixed over what is already there by that much (crossfade). */
static void post(const settings *s, int px, int py, int pw, int ph, int ow, int oh, int offscreen,
                 float tilt, float dof_tilt, float dof_depth, float osd_alpha, int threeD,
                 float blend, float aperture)
{
    int pc = threeD ? P3_COC : P_COC, pb = threeD ? P3_BRIGHT : P_BRIGHT,
        pf = threeD ? P3_FINAL : P_FINAL;
    pic3d = threeD;
    static const float dof_scale[3] = {0.0f, 0.4f, 1.0f};   /* port: hd2d_set_dof */
    dof_tilt *= dof_scale[dof_level];
    tilt *= dof_scale[dof_level];   /* the enlargement too: at Off no see-through gaps */
    dof_depth *= dof_scale[dof_level];
    const float fore_depth = 0.22f;   /* light.frag's depthOf(C_FORE) */
    if (s->effects) {
        /* 3: blur amount, then the variable blur, H and V, both planes. */
        p_glUseProgram(g.prog[pc]);
        bind_picture(pc, pw, ph, tilt);
        p_glUniform4f(U(pc, "uFocus"), g.focus_y, s->focus_band, s->focus_ramp, 0.5f);
        p_glUniform3f(U(pc, "uDof"), dof_tilt, dof_depth, fore_depth);
        draw_to(g.fbo_dof[0], g.half[0].w, g.half[0].h);
        prof_mark("coc");

        p_glUseProgram(g.prog[P_DOFBLUR]);
        p_glUniform1i(U(P_DOFBLUR, "uSrc"), 0);
        p_glUniform1i(U(P_DOFBLUR, "uFg"), 1);
        p_glUniform4f(U(P_DOFBLUR, "uFocus"), g.focus_y, s->focus_band, s->focus_ramp, 0.5f);
        p_glUniform3f(U(P_DOFBLUR, "uDof"), dof_tilt, dof_depth, fore_depth);
        float radius = s->dof_radius * (float)ph / 1080.0f * 2.0f * aperture;
        p_glUniform1f(U(P_DOFBLUR, "uRadius"), radius);
        bind_tex(0, g.half[0].tex);
        bind_tex(1, g.fgh[0].tex);
        p_glUniform2f(U(P_DOFBLUR, "uDir"), 1.0f / g.half[0].w, 0.0f);
        draw_to(g.fbo_dof[1], g.half[1].w, g.half[1].h);
        bind_tex(0, g.half[1].tex);
        bind_tex(1, g.fgh[1].tex);
        p_glUniform2f(U(P_DOFBLUR, "uDir"), 0.0f, 1.0f / g.half[1].h);
        draw_to(g.fbo_dof[0], g.half[0].w, g.half[0].h);
        prof_mark("dof");
    }
    /* 4: bloom, two levels, and the side fill: a soft copy of the
       picture, only made when the picture leaves part of the output
       uncovered. One pass reads the picture for both: quarter[0] gets the
       bloom source, quarter[1] the plain copy. */
    int sides = (px > 0 || py > 0) &&
                /* the deck look: only where the bars are over 20% of the width */
                (!s->side_fill_wide_only || (ow - pw) * 5 > ow);
    if (s->effects || sides) {
        p_glUseProgram(g.prog[pb]);
        bind_picture(pb, pw, ph, tilt);
        p_glUniform1i(U(pb, "uFromPicture"), 1);
        p_glUniform1i(U(pb, "uSrc"), 3);
        bind_tex(3, g.t_mat);   /* unused here, but a valid texture */
        p_glUniform1f(U(pb, "uThreshold"), s->effects ? s->bloom_threshold : -1.0f);
        draw_to(g.fbo_quarter, g.quarter[0].w, g.quarter[0].h);
    }
    if (s->effects) {
        /* quarter[1] is the blur's scratch target, so the soft copy is
           taken from it first. */
        if (sides)
            bright(&g.quarter[1], &g.soft[0], -1.0f);
        gauss(&g.quarter[0], &g.quarter[1], &g.quarter[0]);
        if (!s->bloom_quarter_only) {
            bright(&g.quarter[0], &g.eighth[0], -1.0f);
            gauss(&g.eighth[0], &g.eighth[1], &g.eighth[0]);
        }
        prof_mark("bloom");
    } else if (sides) {
        bright(&g.quarter[1], &g.soft[0], -1.0f);
    }
    if (sides) {
        gauss(&g.soft[0], &g.soft[1], &g.soft[0]);
        gauss(&g.soft[0], &g.soft[1], &g.soft[0]);
        gauss(&g.soft[0], &g.soft[1], &g.soft[0]);
        prof_mark("side");
    }
    int side_fill = sides;

    /* 5: final, with the on-screen message if any. */
    p_glUseProgram(g.prog[pf]);
    bind_picture(pf, pw, ph, tilt);
    {
        float scale = (float)(ph / 180 > 2 ? ph / 180 : 2);   /* 6x at 1080p */
        p_glUniform1i(U(pf, "uOsd"), 7);
        bind_tex(7, g.t_osd ? g.t_osd : g.t_mat);
        p_glUniform4f(U(pf, "uOsdRect"), px + scale * 4, py + scale * 4, g.osd_w * scale,
                      g.osd_h * scale);
        p_glUniform1f(U(pf, "uOsdAlpha"), g.t_osd ? osd_alpha : 0.0f);
    }
    ui_panels_bind(pf);   /* port: remastered menus */
    {   /* port: HD text, over the picture where its console pixels are */
        text_upload();
        int on = text_ov.rgba && g.t_text && !debug_view;
        p_glUniform1i(U(pf, "uText"), 15);
        bind_tex(15, on ? g.t_text : g.t_mat);
        p_glUniform4f(U(pf, "uTextRect"), px + text_ov.x * pw / SCHED_WIDTH,
                      py + text_ov.y * ph / SCHED_HEIGHT, text_ov.cw * pw / SCHED_WIDTH,
                      text_ov.ch * ph / SCHED_HEIGHT);
        p_glUniform1i(U(pf, "uTextOn"), on);
    }
    p_glUniform1i(U(pf, "uDof"), 3);
    p_glUniform1i(U(pf, "uBloomA"), 4);
    p_glUniform1i(U(pf, "uBloomB"), 5);
    p_glUniform1i(U(pf, "uSoft"), 6);
    p_glUniform1i(U(pf, "uOrig"), 8);
    p_glUniform1i(U(pf, "uFgDof"), 9);
    bind_tex(3, g.half[0].tex);
    bind_tex(4, g.quarter[0].tex);
    bind_tex(5, s->bloom_quarter_only ? g.quarter[0].tex : g.eighth[0].tex);
    bind_tex(6, g.soft[0].tex);
    bind_tex(8, g.t_color);
    bind_tex(9, g.fgh[0].tex);
    p_glUniform4f(U(pf, "uFocus"), g.focus_y, s->focus_band, s->focus_ramp, 0.5f);
    p_glUniform3f(U(pf, "uDof3"), dof_tilt, dof_depth, fore_depth);
    p_glUniform4f(U(pf, "uRect"), (float)px, (float)py, (float)pw, (float)ph);
    p_glUniform2f(U(pf, "uOut"), (float)ow, (float)oh);
    p_glUniform1i(U(pf, "uFlip"), offscreen ? 0 : 1);
    p_glUniform4f(U(pf, "uGrade"), s->bloom, s->exposure, s->saturation, s->vignette);
    p_glUniform4f(U(pf, "uGrade2"), s->grain, s->split, s->contrast, (float)s->effects);
    p_glUniform3f(U(pf, "uShadowTint"), 0.92f, 0.98f, 1.08f);
    p_glUniform3f(U(pf, "uHighTint"), 1.06f, 1.0f, 0.9f);
    p_glUniform1f(U(pf, "uSideDim"), side_fill ? s->side_dim : 0.0f);
#ifdef CT_PS5
    p_glUniform1i(U(pf, "uFillTB"), 0);   /* port: the mirrored blur there read as a smear */
#else
    p_glUniform1i(U(pf, "uFillTB"), 1);
#endif
    p_glUniform1f(U(pf, "uTime"), (float)(g.frame % 997) / 997.0f);
    p_glUniform1i(U(pf, "uDebug"), debug_view);
    if (threeD) {
        /* Sharpening (scene3d_pic.frag): RCAS after EASU, CAS at the
           native render scale; neither in the debug views. */
        float sh = user_sharpen >= 0.0f ? user_sharpen : g.s3.scaled ? 0.5f : 0.0f;
        if (debug_view || sh <= 0.0f || !s->effects)
            p_glUniform2f(U(pf, "uSharp"), 0.0f, 0.0f);
        else if (g.s3.scaled)
            p_glUniform2f(U(pf, "uSharp"), 2.0f, sh);   /* FsrRcasCon: exp2(-stops) */
        else   /* CasSetup's peak */
            p_glUniform2f(U(pf, "uSharp"), 1.0f, -1.0f / (8.0f + (5.0f - 8.0f) * sh));
    }
    if (blend > 0.0f && blend < 1.0f) {
        p_glEnable(GL_BLEND);
        p_glBlendColor(0.0f, 0.0f, 0.0f, blend);
        p_glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    }
    draw_to(offscreen ? g.out.fbo : 0, ow, oh);
    p_glDisable(GL_BLEND);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    pic3d = 0;
    prof_mark(threeD ? "s3d_final" : "final");
}

/* ---- the 3D diorama (scene3d.h) ----
   Map views the game describes (hd2d_set_scene) are drawn as geometry:
   a shadow map from the key light, the surfaces into a picture-size
   target with real depth (color, depth for the depth of field in alpha,
   material), the console frame's UI over them, then the 2D path's depth
   of field, bloom and grade (post) read that target through
   scene3d_pic.frag. Anything else (battles, menus, the world map, mode 7,
   a game with no scene) is the 2D path; between the two, both are drawn
   and crossfaded. */

#define S3D_SHADOW_SIZE 1024
#define S3D_FADE_STEP   0.125f   /* 8 frames */
#define S3D_AO_RADIUS   12.0f    /* ambient occlusion's reach, map px (a cell is 16) */

static void s3d_params_init(void)
{
    g.s3.tilt = user_tilt;   /* degrees from looking straight down (40; port: hd2d_set_camera) */
    g.s3.fov = 30.0f;    /* vertical field of view */
    g.s3.zoom = user_zoom;   /* 1: the console's 256 px across at the focus (0.75) */
    g.s3.hmax = 3;       /* wall height cap, cells */
    g.s3.stretch = -1.0f;   /* sprites: < 0 picks the scale that keeps their proportions */
    g.s3.ao_radius = S3D_AO_RADIUS;
    g.s3.wtilt = user_wtilt;   /* the world map's camera (port: hd2d_set_world_camera) */
    g.s3.wzoom = user_wzoom;
    g.s3.marks = 0;      /* world map entrance marks */
    /* CT_HD2D_3D=tilt=40,fov=30,zoom=0.75,hmax=3,stretch=1.2,aoradius=12 overrides them,
       and wtilt=,wzoom=,marks=1 for the world map. */
    const char *e = getenv("CT_HD2D_3D");
    for (const char *p = e; p && *p;) {
        char key[16];
        float v;
        int n = 0;
        if (sscanf(p, "%15[a-z]=%f%n", key, &v, &n) == 2 && n > 0) {
            if (!strcmp(key, "tilt")) g.s3.tilt = v < 0 ? 0 : v > 80 ? 80 : v;
            else if (!strcmp(key, "fov")) g.s3.fov = v < 5 ? 5 : v > 90 ? 90 : v;
            else if (!strcmp(key, "zoom")) g.s3.zoom = v < 0.25f ? 0.25f : v > 4 ? 4 : v;
            else if (!strcmp(key, "hmax")) g.s3.hmax = (int)v;
            else if (!strcmp(key, "stretch")) g.s3.stretch = v < 0.5f ? 0.5f : v > 2 ? 2 : v;
            else if (!strcmp(key, "aoradius")) g.s3.ao_radius = v < 2 ? 2 : v > 48 ? 48 : v;
            else if (!strcmp(key, "wtilt")) g.s3.wtilt = v < 0 ? 0 : v > 80 ? 80 : v;
            else if (!strcmp(key, "wzoom")) g.s3.wzoom = v < 0.25f ? 0.25f : v > 4 ? 4 : v;
            else if (!strcmp(key, "marks")) g.s3.marks = v != 0;
            p += n;
        } else {
            p++;
        }
        while (*p == ',' || *p == ' ')
            p++;
    }
}

/* Programs and the fixed-size resources. Failure only turns the 3D path
   off: the 2D presets don't need it. */
/* port: the 3D target's clear as a draw: on the PS5's GL a glClear of
   its color targets cost about as long as a frame (36 ms at 1080 lines,
   in proportion to the pixels), a full-screen triangle almost nothing. */
static const char clear3d_frag[] =
    "in vec2 vUV;\n"
    "uniform vec4 uC;\n"
    "layout(location = 0) out vec4 o0;\n"
    "layout(location = 1) out vec4 o1;\n"
    "void main() { o0 = uC; o1 = uC; }\n";

static int s3d_init(void)
{
    s3d_params_init();
    /* The picture's passes get FSR 1 and CAS (third_party/fidelityfx) in
       front of scene3d_pic.frag, for the sharpening. */
    size_t nf = strlen(hd2d_src_ffx_fsr1_cas_glsl), np = strlen(hd2d_src_scene3d_pic_frag);
    char *pic = malloc(nf + np + 1);
    if (!pic)
        return -1;
    memcpy(pic, hd2d_src_ffx_fsr1_cas_glsl, nf);
    memcpy(pic + nf, hd2d_src_scene3d_pic_frag, np + 1);
    const struct { int prog; const char *vert, *lib, *frag, *name; } ps[] = {
        {P3_SCENE, hd2d_src_scene3d_vert, NULL, hd2d_src_scene3d_frag, "scene3d.frag"},
        {P3_SHADOW, hd2d_src_scene3d_vert, "#define SHADOW_PASS\n", hd2d_src_scene3d_frag,
         "scene3d.frag (shadow)"},
        {P3_UI, NULL, NULL, hd2d_src_ui3d_frag, "ui3d.frag"},
        {P3_COC, NULL, pic, hd2d_src_coc_frag, "coc.frag (3D)"},
        {P3_BRIGHT, NULL, pic, hd2d_src_bright_frag, "bright.frag (3D)"},
        {P3_FINAL, NULL, pic, hd2d_src_final_frag, "final.frag (3D)"},
        {P3_AOPREP, NULL, NULL, hd2d_src_aoprep_frag, "aoprep.frag"},
        {P3_AO, NULL, NULL, hd2d_src_ssao_frag, "ssao.frag"},
        {P3_AOBLUR, NULL, NULL, hd2d_src_aoblur_frag, "aoblur.frag"},
        {P3_EASU, NULL, hd2d_src_ffx_fsr1_cas_glsl, hd2d_src_fsr_easu_frag, "fsr_easu.frag"},
        {P3_WSCENE, NULL, "#define WORLD_MAP\n", hd2d_src_scene3d_frag, "scene3d.frag (world)"},
        {P3_WSHADOW, NULL, "#define WORLD_MAP\n#define SHADOW_PASS\n", hd2d_src_scene3d_frag,
         "scene3d.frag (world shadow)"},
        /* port: asset packs' models */
        {P3_MSCENE, NULL, "#define MODEL\n", hd2d_src_scene3d_frag, "scene3d.frag (model)"},
        {P3_MSHADOW, NULL, "#define MODEL\n#define SHADOW_PASS\n", hd2d_src_scene3d_frag,
         "scene3d.frag (model shadow)"},
        {P3_CLEAR, NULL, NULL, clear3d_frag, "clear3d.frag"},
    };
    /* The world map's vertex shader: scene3d.vert with WORLD_MAP; the
       models' with MODEL. */
    static char wvert[8192], mvert[8192];
    snprintf(wvert, sizeof wvert, "#define WORLD_MAP\n%s", hd2d_src_scene3d_vert);
    snprintf(mvert, sizeof mvert, "#define MODEL\n%s", hd2d_src_scene3d_vert);
    for (size_t k = 0; k < sizeof ps / sizeof *ps; k++) {
        const char *vert = ps[k].prog == P3_WSCENE || ps[k].prog == P3_WSHADOW ? wvert
                           : ps[k].prog == P3_MSCENE || ps[k].prog == P3_MSHADOW ? mvert
                                                                                  : ps[k].vert;
        g.prog[ps[k].prog] = link_vs(vert ? vert : hd2d_src_fullscreen_vert, ps[k].lib,
                                     ps[k].frag, ps[k].name);
        if (!g.prog[ps[k].prog]) {
            free(pic);
            return -1;
        }
    }
    free(pic);
    g.s3.t_vram = make_tex(256, 128, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, GL_NEAREST);
    g.s3.t_cgram = make_tex(256, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST);
    /* The world map's (units 6 and 7, bound for every 3D draw). */
    g.s3.t_world_nrm = make_tex(1, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    g.s3.t_overlay = make_tex(CT_SCENE_WORLD_STRIDE, 128, GL_R16UI, GL_RED_INTEGER,
                              GL_UNSIGNED_SHORT, GL_NEAREST);
    g.s3.t_box = make_tex(SCHED_WIDTH, SCHED_HEIGHT, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                          GL_NEAREST);
    p_glUseProgram(g.prog[P3_WSCENE]);
    p_glUniform1i(U(P3_WSCENE, "uWorldNrm"), 6);
    p_glUniform1i(U(P3_WSCENE, "uOverlayMap"), 7);
    /* port: asset packs: the atlas and its entries (whole pictures, poses,
       in every pass), the models' texture */
    static const int pack_progs[] = {P3_SCENE, P3_SHADOW, P3_WSCENE, P3_WSHADOW, P3_MSCENE,
                                     P3_MSHADOW};
    for (size_t k = 0; k < sizeof pack_progs / sizeof *pack_progs; k++) {
        int P = pack_progs[k];
        p_glUseProgram(g.prog[P]);
        p_glUniform1i(U(P, "uPackRects"), 11);
        p_glUniform1i(U(P, "uPackAtlas"), 12);
        p_glUniform1i(U(P, "uModelTex"), 13);
        p_glUniform1i(U(P, "uVram"), 0);
    }
    p_glUseProgram(0);
    p_glGenVertexArrays(2, g.s3.vao);
    p_glGenBuffers(2, g.s3.vbo);
    p_glGenBuffers(2, g.s3.ebo);
    for (int k = 0; k < 2; k++) {
        p_glBindVertexArray(g.s3.vao[k]);
        p_glBindBuffer(GL_ARRAY_BUFFER, g.s3.vbo[k]);
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.s3.ebo[k]);
        p_glEnableVertexAttribArray(0);
        p_glEnableVertexAttribArray(1);
        p_glEnableVertexAttribArray(2);
        p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(s3d_vertex),
                                (const void *)offsetof(s3d_vertex, x));
        p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(s3d_vertex),
                                (const void *)offsetof(s3d_vertex, u));
        p_glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(s3d_vertex),
                                 (const void *)offsetof(s3d_vertex, tile));
    }
    p_glBindVertexArray(g.vao);
    /* The key light's shadow map: depth, compared on lookup. */
    g.s3.t_shadow = make_tex(S3D_SHADOW_SIZE, S3D_SHADOW_SIZE, GL_DEPTH_COMPONENT24,
                             GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    p_glGenFramebuffers(1, &g.s3.fbo_shadow);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.s3.fbo_shadow);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g.s3.t_shadow, 0);
    GLenum none = GL_NONE;
    p_glDrawBuffers(1, &none);
    p_glReadBuffer(GL_NONE);
    int ok = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok || p_glGetError() != GL_NO_ERROR)
        return -1;
    g.s3.ok = 1;
    return 0;
}

static void s3d_free_targets(void)
{
    if (g.s3.fbo)
        p_glDeleteFramebuffers(1, &g.s3.fbo);
    if (g.s3.fbo_up)
        p_glDeleteFramebuffers(1, &g.s3.fbo_up);
    GLuint t[4] = {g.s3.t_color, g.s3.t_mat, g.s3.t_depth, g.s3.t_up};
    p_glDeleteTextures(4, t);
    g.s3.fbo = g.s3.t_color = g.s3.t_mat = g.s3.t_depth = 0;
    g.s3.fbo_up = g.s3.t_up = 0;
    g.s3.w = g.s3.h = g.s3.up_w = g.s3.up_h = 0;
    for (int k = 0; k < 3; k++)
        target_free(&g.s3.ao[k]);
}

static void s3d_shutdown(void)
{
    s3d_free_targets();
    GLuint t[6] = {g.s3.t_vram, g.s3.t_cgram, g.s3.t_shadow, g.s3.t_world_nrm, g.s3.t_overlay,
                   g.s3.t_box};
    p_glDeleteTextures(6, t);
    if (g.s3.fbo_shadow)
        p_glDeleteFramebuffers(1, &g.s3.fbo_shadow);
    p_glDeleteBuffers(2, g.s3.vbo);
    p_glDeleteBuffers(2, g.s3.ebo);
    p_glDeleteVertexArrays(2, g.s3.vao);
    s3d_mesh_free(&g.s3.map);
    s3d_mesh_free(&g.s3.spr);
    memset(&g.s3, 0, sizeof g.s3);
    models_free();   /* port: asset packs' models */
    if (g.m_vao) {
        p_glDeleteBuffers(1, &g.m_vbo);
        p_glDeleteBuffers(1, &g.m_ebo);
        p_glDeleteVertexArrays(1, &g.m_vao);
        g.m_vao = g.m_vbo = g.m_ebo = 0;
    }
    g.model_built = g.model_gen - 1;
}

/* The targets, made the first time the 3D path draws: the render-size
   one with depth (rw x rh), below a render scale of 1 the picture-size
   one EASU fills (pw x ph), and the ambient occlusion's (aw x ah; 0: not
   made). */
static int s3d_resize(int rw, int rh, int pw, int ph, int aw, int ah)
{
    int up = rw != pw || rh != ph;
    if (rw == g.s3.w && rh == g.s3.h && (up ? pw : 0) == g.s3.up_w &&
        (up ? ph : 0) == g.s3.up_h && aw == g.s3.ao[0].w * (aw > 0) &&
        ah == g.s3.ao[0].h * (ah > 0) && (aw > 0) == (g.s3.ao[0].tex != 0))
        return 0;
    s3d_free_targets();
    g.s3.t_color = make_tex(rw, rh, (GLint)g.float_fmt, GL_RGBA, g.float_type, GL_LINEAR);
    g.s3.t_mat = make_tex(rw, rh, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST);
    g.s3.t_depth = make_tex(rw, rh, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT,
                            GL_NEAREST);
    GLuint two[2] = {g.s3.t_color, g.s3.t_mat};
    int ok = !fbo_for(&g.s3.fbo, two, 2);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.s3.fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g.s3.t_depth, 0);
    ok &= p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (ok && up) {
        g.s3.t_up = make_tex(pw, ph, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
                             GL_LINEAR);
        ok &= !fbo_for(&g.s3.fbo_up, &g.s3.t_up, 1);
    }
    /* The occlusion's: RG16F (two values each), its input read as it is
       (nearest), never blended. */
    for (int k = 0; ok && aw > 0 && k < 3; k++) {
        target *t = &g.s3.ao[k];
        t->w = aw;
        t->h = ah;
        t->tex = make_tex(aw, ah, GL_RG16F, GL_RG, GL_HALF_FLOAT, k == 2 ? GL_NEAREST : GL_LINEAR);
        ok &= !fbo_for(&t->fbo, &t->tex, 1);
    }
    if (!ok) {
        fprintf(stderr, "hd2d: cannot make the %dx%d 3D targets\n", pw, ph);
        s3d_free_targets();
        return -1;
    }
    g.s3.w = rw;
    g.s3.h = rh;
    g.s3.up_w = up ? pw : 0;
    g.s3.up_h = up ? ph : 0;
    return 0;
}

/* ---- 4x4 matrices, column-major (GL's order) ---- */
static void m4_mul(float *o, const float *a, const float *b)
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] +
                           a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, sizeof r);
}

static void v3_norm(float *v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int k = 0; k < 3; k++)
        v[k] /= l > 0 ? l : 1;
}

static void m4_look(float *m, const float *eye, const float *at, const float *up)
{
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    v3_norm(f);
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                  f[0] * up[1] - f[1] * up[0]};
    v3_norm(s);
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    float r[16] = {s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0,
                   -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]),
                   -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                   f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2], 1};
    memcpy(m, r, sizeof r);
}

/* flip_y: row 0 of the target is the picture's top, as every intermediate here. */
static void m4_persp(float *m, float fovy, float aspect, float n, float f, int flip_y)
{
    float t = 1.0f / tanf(fovy * 0.5f);
    float r[16] = {t / aspect, 0, 0, 0, 0, flip_y ? -t : t, 0, 0, 0, 0, (f + n) / (n - f), -1,
                   0, 0, 2 * f * n / (n - f), 0};
    memcpy(m, r, sizeof r);
}

static void m4_ortho(float *m, float r_, float t, float n, float f)
{
    float r[16] = {1 / r_, 0, 0, 0, 0, 1 / t, 0, 0, 0, 0, -2 / (f - n), 0,
                   0, 0, -(f + n) / (f - n), 1};
    memcpy(m, r, sizeof r);
}

/* Which BG character tiles are empty: a mesh built while the map's tiles
   were still loading is rebuilt once they are in. */
static uint32_t s3d_empty_hash(const ct_scene *sc)
{
    uint32_t h = 2166136261u;
    for (int l = 0; l < 2; l++) {
        const ct_scene_layer *L = &sc->layer[l];
        if (!L->present)
            continue;
        for (uint32_t t = 0; t < 1024; t++) {
            uint32_t a = (L->chr_base + t * 16) & 0x7FFF, any = 0;
            for (int k = 0; k < 16; k++)
                any |= sc->vram[(a + (uint32_t)k) & 0x7FFF];
            h = (h ^ (any ? 1u : 0u)) * 16777619u;
        }
    }
    return h;
}

static void s3d_upload(GLuint vbo, GLuint ebo, const s3d_mesh *m, GLenum usage)
{
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(s3d_vertex) * (size_t)(m->nv ? m->nv : 1)),
                   m->nv ? m->v : NULL, usage);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                   (GLsizeiptr)(sizeof(uint32_t) * (size_t)(m->ni ? m->ni : 1)),
                   m->ni ? m->idx : NULL, usage);
}

/* Geometry and the console's state for this frame. */
static void s3d_update(const ct_scene *sc)
{
    s3d_params p = {g.s3.hmax, 1.0f, user_cards, 0.0f};
    {   /* port: the cards' south shift for this tilt (rebuilt when it changes) */
        float t = g.s3.tilt * 3.14159265f / 180.0f;
        p.card_shift = t > 0.05f ? tanf(t) : 0.05f;
    }
    float tilt = g.s3.tilt * 3.14159265f / 180.0f;
    /* Upright things are foreshortened by sin(tilt), floors by cos(tilt):
       sprites taller by their ratio keep the proportions they were drawn
       with (walls keep the map's grid instead). */
    float st = tilt > 0.1f ? cosf(tilt) / sinf(tilt) : 1.4f;
    p.stretch = g.s3.stretch > 0 ? g.s3.stretch : st < 1.0f ? 1.0f : st > 1.4f ? 1.4f : st;

    bind_tex(0, g.s3.t_vram);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 128, GL_RED_INTEGER, GL_UNSIGNED_SHORT,
                      sc->vram);
    uint8_t pal[256 * 4];
    for (int k = 0; k < 256; k++) {
        uint16_t c = sc->cgram[k];
        pal[k * 4 + 0] = (uint8_t)((c & 31) * 255 / 31);
        pal[k * 4 + 1] = (uint8_t)((c >> 5 & 31) * 255 / 31);
        pal[k * 4 + 2] = (uint8_t)((c >> 10 & 31) * 255 / 31);
        pal[k * 4 + 3] = 255;
    }
    bind_tex(0, g.s3.t_cgram);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, pal);

    uint32_t eh = s3d_empty_hash(sc);
    if (!g.s3.have_map || sc->map_generation != g.s3.map_gen || eh != g.s3.empty_hash ||
        sc->mode != g.s3.map_mode || g.model_built != g.model_gen || g.s3.map_cards != user_cards ||
        g.s3.map_tilt != g.s3.tilt ||
        (sc->mode == CT_SCENE_FIELD && !s3d_room_matches(sc))) {
        /* port: asset packs' models: their cell hook first, then (below,
           once the map's heights are known) their triangles */
        if (g.model_fn)
            g.model_fn(g.model_user, sc, NULL);
        if ((sc->mode == CT_SCENE_WORLD ? s3d_build_world(sc, &g.s3.map)
                                        : s3d_build_map(sc, &p, &g.s3.map)) < 0)
            fprintf(stderr, "hd2d: out of memory for the 3D map\n");
        hd2d_models mo;
        memset(&mo, 0, sizeof mo);
        if (g.model_fn && g.model_fn(g.model_user, sc, &mo) > 0)
            models_upload(&mo);
        else
            models_free();
        g.model_built = g.model_gen;
        if (sc->mode == CT_SCENE_WORLD) {   /* the ground's normals */
            int nw, nh;
            const uint8_t *n = s3d_world_normals(&nw, &nh);
            bind_tex(0, g.s3.t_world_nrm);
            p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nw, nh, 0, GL_RGBA, GL_UNSIGNED_BYTE, n);
        }
        p_glBindVertexArray(g.s3.vao[0]);
        s3d_upload(g.s3.vbo[0], g.s3.ebo[0], &g.s3.map, GL_STATIC_DRAW);
        g.s3.map_ni = g.s3.map.ni;
        g.s3.map_ni_floor = g.s3.map.ni_floor;
        g.s3.map_gen = sc->map_generation;
        g.s3.map_mode = sc->mode;
        g.s3.map_cards = user_cards;
        g.s3.map_tilt = g.s3.tilt;
        g.s3.empty_hash = eh;
        g.s3.have_map = 1;
    }
    if (sc->mode == CT_SCENE_WORLD) {   /* the overlay's tilemap; sprites wait for the camera */
        bind_tex(0, g.s3.t_overlay);
        if (sc->layer[2].present && sc->layer[2].tiles && sc->overlay_h <= 128)
            p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CT_SCENE_WORLD_STRIDE, sc->overlay_h,
                              GL_RED_INTEGER, GL_UNSIGNED_SHORT, sc->layer[2].tiles);
        /* port: the clouds' palette, the overlay's commonest one: a caption
           (a place's name) is drawn into the same layer with another
           palette, and its box floated over the map as a pale box */
        g.s3.cloud_pal = -1;
        if (sc->layer[2].present && sc->layer[2].tiles && sc->overlay_h <= 128) {
            int cnt[8] = {0};
            for (int y = 0; y < sc->overlay_h; y++)
                for (int x = 0; x < sc->overlay_w && x < CT_SCENE_WORLD_STRIDE; x++) {
                    uint16_t e = sc->layer[2].tiles[y * CT_SCENE_WORLD_STRIDE + x];
                    if (e & 0x3FF)
                        cnt[(e >> 10) & 7]++;
                }
            int best = 0;
            for (int k = 1; k < 8; k++)
                best = cnt[k] > cnt[best] ? k : best;
            g.s3.cloud_pal = cnt[best] ? best : -1;
        }
    }
    /* port: asset packs: which of the map's tiles have replacements */
    g.pack3d_on = g.pack_entries > 0 && g.pack_lookup != NULL;
    if (g.pack3d_on) {
        pack_gen++;
        pack_lut_add(&g.s3.map, sc);
        bind_tex(0, g.t_pack_lut);
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PACK_LUT_W, PACK_LUT_H, GL_RED_INTEGER,
                          GL_UNSIGNED_SHORT, pack_lut);
    }
    p_glBindVertexArray(g.vao);
}

/* The map's copies to draw, as offsets (px): one, or for a wrapping world
   map those of its neighbours the view reaches (s3d_world_view). */
static int n_copies = 1;
static float copies[9][2];
static int cloud_first, cloud_ni;   /* the overlay plane, in the sprite mesh */

static void s3d_draw(int raised_only, int prog)
{
    if (!raised_only && prog == P3_SCENE && user_cards) {
        /* port: layered cards, drawn in the console's order: the ground
           and front layer, the sprites under priority 3 (depth tested
           against those), then the overhead cards over them, then the
           sprites over everything. A card and a sprite standing up through
           it no longer cut each other in two (a sitter in a bench, a
           walker under eaves): whichever the console drew in front is. */
        p_glBindVertexArray(g.s3.vao[0]);
        if (g.s3.map_ni_floor)
            p_glDrawElements(GL_TRIANGLES, g.s3.map_ni_floor, GL_UNSIGNED_INT, NULL);
        p_glBindVertexArray(g.s3.vao[1]);
        if (g.s3.blob_ni) {   /* contact shadows: multiplied in, no depth writes */
            p_glDepthMask(0);
            p_glEnable(GL_BLEND);
            p_glBlendFunc(GL_DST_COLOR, GL_ZERO);
            p_glDrawElements(GL_TRIANGLES, g.s3.blob_ni, GL_UNSIGNED_INT,
                             (const void *)(sizeof(uint32_t) * (size_t)g.s3.spr_ni));
            p_glDisable(GL_BLEND);
            p_glDepthMask(1);
        }
        if (g.s3.spr_ni_low)
            p_glDrawElements(GL_TRIANGLES, g.s3.spr_ni_low, GL_UNSIGNED_INT, NULL);
        p_glDepthFunc(GL_ALWAYS);
        p_glBindVertexArray(g.s3.vao[0]);
        if (g.s3.map_ni > g.s3.map_ni_floor)
            p_glDrawElements(GL_TRIANGLES, g.s3.map_ni - g.s3.map_ni_floor, GL_UNSIGNED_INT,
                             (const void *)(sizeof(uint32_t) * (size_t)g.s3.map_ni_floor));
        p_glBindVertexArray(g.s3.vao[1]);
        if (g.s3.spr_ni > g.s3.spr_ni_low)
            p_glDrawElements(GL_TRIANGLES, g.s3.spr_ni - g.s3.spr_ni_low, GL_UNSIGNED_INT,
                             (const void *)(sizeof(uint32_t) * (size_t)g.s3.spr_ni_low));
        p_glDepthFunc(GL_LESS);
        p_glBindVertexArray(g.vao);
        return;
    }
    p_glBindVertexArray(g.s3.vao[0]);
    int first = raised_only ? g.s3.map_ni_floor : 0;
    int world = prog == P3_WSCENE || prog == P3_WSHADOW;
    for (int k = 0; k < (world ? n_copies : 1); k++) {
        if (world)
            p_glUniform3f(U(prog, "uOffset"), copies[k][0], 0.0f, copies[k][1]);
        if (g.s3.map_ni > first)
            p_glDrawElements(GL_TRIANGLES, g.s3.map_ni - first, GL_UNSIGNED_INT,
                             (const void *)(sizeof(uint32_t) * (size_t)first));
    }
    if (world)
        p_glUniform3f(U(prog, "uOffset"), 0.0f, 0.0f, 0.0f);
    p_glBindVertexArray(g.s3.vao[1]);
    if (g.s3.spr_ni)
        p_glDrawElements(GL_TRIANGLES, g.s3.spr_ni, GL_UNSIGNED_INT, NULL);
    p_glBindVertexArray(g.vao);
}

/* The world map's camera-dependent part: the sprites (snapped to the
   target's pixels, so they need the camera), the overlay plane, and which
   copies of the wrapping map the view reaches. */
#define S3D_CLOUD_Y 110.0f
static void s3d_world_view(const ct_scene *sc, const float *eye, const float *at,
                           const float *up, float fovy, float dist, int pw, int ph)
{
    s3d_view v = {.focal = 0.5f * (float)ph / tanf(fovy * 0.5f), .near = dist * 0.15f,
                  .w = pw, .h = ph};
    for (int k = 0; k < 3; k++) {
        v.eye[k] = eye[k];
        v.at[k] = at[k];
        v.fwd[k] = at[k] - eye[k];
    }
    v3_norm(v.fwd);   /* as m4_look */
    float *f = v.fwd, *r = v.right, *u = v.up;
    r[0] = f[1] * up[2] - f[2] * up[1];
    r[1] = f[2] * up[0] - f[0] * up[2];
    r[2] = f[0] * up[1] - f[1] * up[0];
    v3_norm(r);
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];

    /* What the view sees of the ground (y = -10), a margin for the
       mountains, and the far plane where a ray misses it. */
    float th = tanf(fovy * 0.5f), tw = th * (float)pw / (float)ph, far_ = dist * 5.0f;
    float x0 = eye[0], x1 = eye[0], z0 = eye[2], z1 = eye[2];
    for (int k = 0; k < 4; k++) {
        float sx = k & 1 ? 1.0f : -1.0f, sy = k & 2 ? 1.0f : -1.0f, d[3];
        for (int j = 0; j < 3; j++)
            d[j] = f[j] + r[j] * sx * tw + u[j] * sy * th;
        for (int pl = 0; pl < 2; pl++) {   /* the ground, and the overlay plane above it */
            float y = pl ? S3D_CLOUD_Y : -10.0f;
            float t = d[1] < -1e-3f ? (eye[1] - y) / -d[1] : far_;
            t = t > far_ ? far_ : t;
            float px = eye[0] + d[0] * t, pz = eye[2] + d[2] * t;
            x0 = px < x0 ? px : x0;
            x1 = px > x1 ? px : x1;
            z0 = pz < z0 ? pz : z0;
            z1 = pz > z1 ? pz : z1;
        }
    }
    x0 -= 64;
    x1 += 64;
    z0 -= 64;
    z1 += 64;
    float mw = (float)sc->map_w * sc->cell_px, mh = (float)sc->map_h * sc->cell_px;
    n_copies = 0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            if (!sc->wrap && (i || j))
                continue;
            float ox = (float)i * mw, oz = (float)j * mh;
            if (ox + mw < x0 || ox > x1 || oz + mh < z0 || oz > z1)
                continue;
            copies[n_copies][0] = ox;
            copies[n_copies][1] = oz;
            n_copies++;
        }

    s3d_build_world_sprites(sc, &v, g.s3.marks, &g.s3.spr);
    g.s3.spr_ni = g.s3.spr.ni;
    cloud_first = g.s3.spr.ni;
    cloud_ni = 0;
    if (sc->overlay_blend && sc->layer[2].present && sc->overlay_w && sc->overlay_h) {
        float c[4][3] = {{x0, S3D_CLOUD_Y, z0}, {x1, S3D_CLOUD_Y, z0}, {x0, S3D_CLOUD_Y, z1},
                         {x1, S3D_CLOUD_Y, z1}};
        if (!s3d_emit_corners(&g.s3.spr, c, 0, 0, 0))
            cloud_ni = 6;
    }
    p_glBindVertexArray(g.s3.vao[1]);
    s3d_upload(g.s3.vbo[1], g.s3.ebo[1], &g.s3.spr, GL_STREAM_DRAW);
    p_glBindVertexArray(g.vao);
}


/* ssao.frag's kernel of n samples (at most 16), x y z each, in units of
   its radius: a golden-angle spiral over the unit disc lifted to the
   hemisphere (cosine-weighted, z along the normal), the samples' reach
   growing from a fifth of the radius to all of it. */
static void s3d_ao_kernel(float *k, int n)
{
    for (int i = 0; i < n; i++) {
        float q = (i + 0.5f) / (float)n, phi = (float)i * 2.3999632f, r = sqrtf(q);
        float len = 0.2f + 0.8f * q * q;
        k[i * 3 + 0] = cosf(phi) * r * len;
        k[i * 3 + 1] = sinf(phi) * r * len;
        k[i * 3 + 2] = sqrtf(1.0f - q) * len;
    }
}

/* The 3D view into g.s3's targets: the surfaces at the render scale,
   their ambient occlusion, EASU up to the picture size when the render
   scale is below 1, the UI on top at the picture size. post() reads the
   result (bind_picture). */
/* Console pixels the 3D scene doesn't hold: the scene's BG1 is the map as
   the game keeps it, but the console's BG1 can show more, drawn into its
   tilemap only (a text box's window, under its BG3 text). Per 8x8 tile of
   BG1 on the screen (the scene's camera is BG1's scroll), the console's
   BG1 pixels there are compared with the scene's tile decoded from VRAM
   and CGRAM; a tile where they mostly differ is marked, and where marked
   tiles make a box, every console pixel in it is laid flat over the 3D
   picture, as the UI is. */
static void s3d_box_mask(const ct_scene *sc, const uint8_t *bgrx, const uint8_t *layers,
                         int ui_z)
{
    static uint8_t mask[SCHED_WIDTH * SCHED_HEIGHT];
    static uint8_t changed[SCHED_WIDTH * SCHED_HEIGHT];
    static int was_set;
    memset(mask, 0, sizeof mask);
    memset(changed, 0, sizeof changed);
    int any = 0;
    uint8_t mk[29][33];   /* marked tiles, from the camera's top-left one */
    memset(mk, 0, sizeof mk);
    const ct_scene_layer *L = &sc->layer[0];
    if (sc->mode == CT_SCENE_FIELD && L->present && L->tiles && sc->vram && sc->cgram &&
        sc->brightness >= 4) {
        uint8_t lev[32];
        for (int c = 0; c < 32; c++) {
            int v = c * sc->brightness / 15;
            lev[c] = (uint8_t)(v << 3 | v >> 2);
        }
        int tw = 2 * sc->map_w, th = 2 * sc->map_h, cx = sc->camera_x, cy = sc->camera_y;
        for (int ty = cy >> 3; ty * 8 - cy < SCHED_HEIGHT; ty++) {
            if (ui_z < 0 && ty * 8 - cy + 7 < SCHED_HEIGHT / 2)
                continue;   /* the opening bar can only be a dialogue window near the bottom */
            for (int tx = cx >> 3; tx * 8 - cx < SCHED_WIDTH; tx++) {
                uint16_t e = tx >= 0 && ty >= 0 && tx < tw && ty < th
                                 ? L->tiles[ty * CT_SCENE_STRIDE + tx] : 0;
                uint32_t a = (L->chr_base + (e & 0x3FFu) * 16u) & 0x7FFFu;
                int seen = 0, diff = 0;
                for (int v = 0; v < 8; v++) {
                    int y = ty * 8 - cy + v;
                    if (y < 0 || y >= SCHED_HEIGHT)
                        continue;
                    int r = e & 0x8000 ? 7 - v : v;
                    uint16_t w0 = sc->vram[(a + (uint32_t)r) & 0x7FFF],
                             w1 = sc->vram[(a + 8u + (uint32_t)r) & 0x7FFF];
                    for (int u = 0; u < 8; u++) {
                        int x = tx * 8 - cx + u, k = y * SCHED_WIDTH + x;
                        if (x < 0 || x >= SCHED_WIDTH || (layers[k] & 7) != 0)
                            continue;   /* not BG1 there */
                        int sh = e & 0x4000 ? u : 7 - u;
                        int ci = (w0 >> sh & 1) | (w0 >> (sh + 8) & 1) << 1 |
                                 (w1 >> sh & 1) << 2 | (w1 >> (sh + 8) & 1) << 3;
                        seen++;
                        if (!ci) {
                            diff++;
                            changed[k] = 1;
                            continue;
                        }
                        uint16_t c = sc->cgram[(L->pal_base + ((e >> 10) & 7) * 16 + ci) & 255];
                        const uint8_t *p = &bgrx[k * 4];
                        int different = abs(p[2] - lev[c & 31]) > 8 ||
                                        abs(p[1] - lev[c >> 5 & 31]) > 8 ||
                                        abs(p[0] - lev[c >> 10 & 31]) > 8;
                        diff += different;
                        changed[k] = (uint8_t)different;
                    }
                }
                if (diff < 6 || diff * 2 < seen)
                    continue;
                mk[ty - (cy >> 3)][tx - (cx >> 3)] = 1;
            }
        }
        /* A box is a band of tiles across the screen with the UI's text
           in it: runs of at least 16 marked tiles in a row, within 10
           tile rows of a row with UI pixels (scattered marks are tiles
           the game changed, or color math; a band far from any text is
           the void below a map, which the console fills and the scene
           doesn't). */
        int text_row[29] = {0};
        for (int y = 0; ui_z >= 0 && y < SCHED_HEIGHT; y++) {
            const uint8_t *row = layers + y * SCHED_WIDTH;
            int n = 0;
            for (int x = 0; x < SCHED_WIDTH; x++)
                n += (row[x] >> 4) == ui_z && (row[x] & 7) < 4;
            if (n >= 4)
                text_row[(y + (cy & 7)) >> 3] = 1;
        }
        for (int r = 0; r < 29; r++) {
            int near = 0;
            for (int t = r - 10; t <= r + 10 && !near; t++)
                near = t >= 0 && t < 29 && text_row[t];
            for (int c0 = 0; near && c0 < 33;) {
                int c1 = c0;
                while (c1 < 33 && mk[r][c1])
                    c1++;
                for (int c = c0; c1 - c0 >= 16 && c < c1; c++) {
                    any = 1;
                    for (int v = 0; v < 8; v++)
                        for (int u = 0; u < 8; u++) {
                            int x = ((cx >> 3) + c) * 8 - cx + u, y = ((cy >> 3) + r) * 8 - cy + v;
                            if (x >= 0 && x < SCHED_WIDTH && y >= 0 && y < SCHED_HEIGHT)
                                mask[y * SCHED_WIDTH + x] = 1;
                        }
                }
                c0 = c1 > c0 ? c1 : c0 + 1;
            }
        }

        /* Before the first glyph is drawn, the dialogue window opens as a
           thin horizontal band. It has no text row to anchor the normal
           box detector, so preserve a broad, line-like BG1 difference at
           the bottom of the picture pixel-for-pixel. */
        if (ui_z < 0) {
            const int run_w = 96, min_changed = 64;
            for (int y = SCHED_HEIGHT / 2; y < SCHED_HEIGHT; y++)
                for (int x0 = 0; x0 + run_w <= SCHED_WIDTH; x0 += 8) {
                    int n = 0;
                    for (int x = x0; x < x0 + run_w; x++)
                        n += changed[y * SCHED_WIDTH + x] != 0;
                    if (n < min_changed)
                        continue;
                    for (int x = x0; x < x0 + run_w; x++) {
                        int k = y * SCHED_WIDTH + x;
                        if (changed[k]) {
                            mask[k] = 1;
                            any = 1;
                        }
                    }
                }
        }
    }
    if (any || was_set) {
        bind_tex(0, g.s3.t_box);
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCHED_WIDTH, SCHED_HEIGHT, GL_RED_INTEGER,
                          GL_UNSIGNED_BYTE, mask);
    }
    was_set = any;
}

/* The focus row's map pixels should occupy a whole number of target
   pixels. zoom is expressed as source-pixel scale at the focus, so this
   snaps it to the nearest integer density at the current render width. */
static float s3d_integer_zoom(float zoom, int render_width)
{
    if (render_width <= 0)
        return zoom;
    float pixels = floorf((float)render_width * zoom / 256.0f + 0.5f);
    float max_pixels = floorf((float)render_width * 4.0f / 256.0f);
    if (max_pixels < 1.0f)
        max_pixels = 1.0f;
    pixels = pixels < 1.0f ? 1.0f : pixels > max_pixels ? max_pixels : pixels;
    return pixels * 256.0f / (float)render_width;
}

static int s3d_render(const ct_scene *sc, const settings *s, int pw, int ph, int ui_z,
                      float scene_amt)
{
    float a = s->effects ? scene_amt : 0.0f;
    float rs = user_render_scale > 0.0f ? user_render_scale : s->render_scale;
#ifdef CT_PS5
    /* port: the PS5's Auto: the diorama drawn at no more than 1080 lines
       and upscaled with FSR (30 fps at 4K full size). Not fewer: at 720
       the pixel art came out uneven and blocky in the 3D view. */
    if (user_render_scale <= 0.0f && ph > 1080) {
        float cap = 1080.0f / (float)ph;
        rs = rs > 0.0f && rs < cap ? rs : cap;
    }
    /* port: and below that, the dynamic resolution (prof_flush): only as
       far down as needed to keep 60 fps, never under 810 lines' worth. */
    if (user_render_scale <= 0.0f && dyn_scale < 1.0f)
        rs = (rs > 0.0f ? rs : 1.0f) * dyn_scale;
#endif
    int rw = pw, rh = ph;
    if (rs > 0.0f && rs < 0.995f) {
        rw = (int)lroundf(pw * rs);
        rh = (int)lroundf(ph * rs);
        rw = rw < 8 ? 8 : rw;
        rh = rh < 8 ? 8 : rh;
    }
    /* (its targets kept while menus ease it off: no remaking them each time) */
    float ao_base = user_ao >= 0.0f ? user_ao : s->ao3d, ao_amt = ao_base * a;
#ifdef CT_PS5
    if (user_ao < 0.0f)   /* port: Auto is off on the PS5 (the costliest pass) */
        ao_base = ao_amt = 0.0f;
#endif
    if (g.float_type != GL_HALF_FLOAT)   /* its depths need half floats */
        ao_base = ao_amt = 0.0f;
    int div = user_ao_div ? user_ao_div : s->ao_div > 0 ? s->ao_div : 2;
    int aw = ao_base > 0.0f ? rw / div : 0, ah = ao_base > 0.0f ? rh / div : 0;
    if (s3d_resize(rw, rh, pw, ph, aw, ah))
        return -1;
    s3d_update(sc);

    /* The camera: looking at the middle of the console's view, tilted
       back from straight down, far enough off to show about its width. */
    /* The world map has a camera of its own (a higher one, if the user
       likes), centred on the console camera, including scripted pans (it wraps). */
    int world = sc->mode == CT_SCENE_WORLD;
    float zoom = world ? g.s3.wzoom : s3d_integer_zoom(s3d_room_zoom(g.s3.zoom), rw);
    float live_tilt, live_yaw;
    s3d_camera_offsets(sc, g.s3.dtilt, g.s3.dyaw, &live_tilt, &live_yaw);
    float tilt_deg = (world ? g.s3.wtilt : g.s3.tilt) + live_tilt;
    tilt_deg = tilt_deg < 0 ? 0 : tilt_deg > 80 ? 80 : tilt_deg;
    /* port: the field's layered cards with a long lens (14 degrees, from
       further off: the same view): with the wide one, cards and sprites
       drifted apart as the camera moved, and upright sprites leaned at the
       sides of the screen. */
    float fov_deg = !world && user_cards && g.s3.fov > 14.0f ? 14.0f : g.s3.fov;
    float tilt = tilt_deg * 3.14159265f / 180.0f, fovy = fov_deg * 3.14159265f / 180.0f;
    float aspect = (float)pw / (float)ph;   /* the picture's, at any render scale */
    float dist = 128.0f / zoom / (tanf(fovy * 0.5f) * aspect);
    float at[3];
    s3d_camera_target(sc, at);
    /* The console's camera stops at the map's edges. This one sees
       further at the top of the picture (and wider there), so it stops
       sooner: the floor at the picture's edges stays on the map, give or
       take a margin the walls along an edge cover; a map smaller than the
       view is centered. */
    if (!world) {
        int x0, z0, x1, z1;
        s3d_map_bounds(&x0, &z0, &x1, &z1);
        const float margin = 24.0f;
        float below = tilt - fovy * 0.5f, above = tilt + fovy * 0.5f;   /* from straight down */
        if (above > 1.45f)
            above = 1.45f;
        float near_z = dist * sinf(tilt) - dist * cosf(tilt) * tanf(below > 0 ? below : 0);
        float far_z = dist * sinf(tilt) - dist * cosf(tilt) * tanf(above);
        float far_len = dist * cosf(tilt) / cosf(above);   /* eye to the far edge's floor */
        float half_w = far_len * tanf(fovy * 0.5f) * aspect;
        if (z1 > z0) {
            float lo = (float)z0 - margin - far_z, hi = (float)z1 - near_z;
            at[2] = lo > hi ? 0.5f * (lo + hi) : at[2] < lo ? lo : at[2] > hi ? hi : at[2];
        }
        if (x1 > x0) {
            float lo = (float)x0 - margin + half_w, hi = (float)x1 + margin - half_w;
            at[0] = lo > hi ? 0.5f * (lo + hi) : at[0] < lo ? lo : at[0] > hi ? hi : at[0];
        }
    }
    float eye[3] = {at[0], at[1] + dist * cosf(tilt), at[2] + dist * sinf(tilt)};
    float up[3] = {0.0f, sinf(tilt), -cosf(tilt)};
    if (live_yaw != 0.0f) {   /* port: the gyro's turn, about the vertical through at */
        float yaw = live_yaw * 3.14159265f / 180.0f, sy = sinf(yaw), cy = cosf(yaw);
        float h = dist * sinf(tilt);
        eye[0] = at[0] + h * sy;
        eye[2] = at[2] + h * cy;
        up[0] = -cosf(tilt) * sy;
        up[2] = -cosf(tilt) * cy;
    }
    float view[16], proj[16], mvp[16];
    m4_look(view, eye, at, up);
    m4_persp(proj, fovy, aspect, dist * 0.15f, dist * 5.0f, 1);
    m4_mul(mvp, proj, view);
    if (world) {
        s3d_world_view(sc, eye, at, up, fovy, dist, rw, rh);   /* snapped to the render target */
    } else {
        s3d_view sv = {.focal = 0.5f * (float)rh / tanf(fovy * 0.5f), .near = dist * 0.15f,
                       .w = rw, .h = rh};
        s3d_params sp = {g.s3.hmax, 1.0f, user_cards, tanf(tilt)};
        float st = tilt_deg > 0.1f ? cosf(tilt) / sinf(tilt) : 1.4f;
        sp.stretch = g.s3.stretch > 0.0f ? g.s3.stretch
                    : st < 1.0f               ? 1.0f
                    : st > 1.4f               ? 1.4f
                                               : st;
        for (int k = 0; k < 3; k++) {
            sv.eye[k] = eye[k];
            sv.at[k] = at[k];
            sv.fwd[k] = at[k] - eye[k];
        }
        v3_norm(sv.fwd);
        sv.right[0] = sv.fwd[1] * up[2] - sv.fwd[2] * up[1];
        sv.right[1] = sv.fwd[2] * up[0] - sv.fwd[0] * up[2];
        sv.right[2] = sv.fwd[0] * up[1] - sv.fwd[1] * up[0];
        v3_norm(sv.right);
        sv.up[0] = sv.right[1] * sv.fwd[2] - sv.right[2] * sv.fwd[1];
        sv.up[1] = sv.right[2] * sv.fwd[0] - sv.right[0] * sv.fwd[2];
        sv.up[2] = sv.right[0] * sv.fwd[1] - sv.right[1] * sv.fwd[0];
        s3d_build_sprites_view(sc, &sp, &sv, &g.s3.spr);
        p_glBindVertexArray(g.s3.vao[1]);
        s3d_upload(g.s3.vbo[1], g.s3.ebo[1], &g.s3.spr, GL_STREAM_DRAW);
        g.s3.blob_ni = s3d_sprite_blobs() * 6;
        g.s3.spr_ni = g.s3.spr.ni - g.s3.blob_ni;
        g.s3.spr_ni_low = g.s3.spr.ni_floor;
        n_copies = 1;
        copies[0][0] = copies[0][1] = 0.0f;
        cloud_ni = 0;
    }

    /* The key light, low from the south-west: lights the walls facing the
       camera, and casts shadows back and to the right. */
    float L[3] = {-0.5f, 0.75f, 0.45f};
    v3_norm(L);
    float R = 300.0f / zoom, far_ = 1200.0f;
    float leye[3] = {at[0] + L[0] * far_ * 0.5f, at[1] + L[1] * far_ * 0.5f,
                     at[2] + L[2] * far_ * 0.5f - 60.0f};
    float lat[3] = {at[0], 0.0f, at[2] - 60.0f};
    float lup[3] = {0.0f, 0.0f, -1.0f};
    float lview[16], lproj[16], lmvp[16];
    m4_look(lview, leye, lat, lup);
    m4_ortho(lproj, R, R, 1.0f, far_);
    m4_mul(lmvp, lproj, lview);

    p_glEnable(GL_DEPTH_TEST);
    p_glDepthFunc(world ? GL_LEQUAL : GL_LESS);   /* the world map's front layer: on the same corners */
    p_glDepthMask(1);

    /* Shadow map: what stands up (walls, roofs, sprites). (port: none
       with the 3D shadows setting off: the scene's shader skips the map.) */
    int PS = world ? P3_WSHADOW : P3_SHADOW;
    if (shadows_3d) {
    p_glUseProgram(g.prog[PS]);
    p_glUniformMatrix4fv(U(PS, "uMVP"), 1, GL_FALSE, lmvp);
    p_glUniformMatrix4fv(U(PS, "uLightMVP"), 1, GL_FALSE, lmvp);
    p_glUniform1i(U(PS, "uVram"), 0);
    bind_tex(0, g.s3.t_vram);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.s3.fbo_shadow);
    p_glViewport(0, 0, S3D_SHADOW_SIZE, S3D_SHADOW_SIZE);
    p_glClear(GL_DEPTH_BUFFER_BIT);
    p_glEnable(GL_POLYGON_OFFSET_FILL);
    p_glPolygonOffset(1.5f, 2.0f);
    bind_tex(11, g.t_pack_rects);   /* port: asset packs (whole pictures) */
    p_glActiveTexture(GL_TEXTURE0 + 12);
    p_glBindTexture(GL_TEXTURE_2D_ARRAY, g.t_pack_atlas);
    s3d_draw(1, PS);
    if (g.m_ni) {   /* port: asset packs' models */
        p_glUseProgram(g.prog[P3_MSHADOW]);
        p_glUniformMatrix4fv(U(P3_MSHADOW, "uMVP"), 1, GL_FALSE, lmvp);
        p_glUniformMatrix4fv(U(P3_MSHADOW, "uLightMVP"), 1, GL_FALSE, lmvp);
        models_draw(P3_MSHADOW, (const float (*)[2])copies, n_copies);
    }
    p_glDisable(GL_POLYGON_OFFSET_FILL);
    }
    prof_mark("s3d_shadow");

    /* The surfaces. */
    float fog[3] = {0.27f, 0.30f, 0.37f};   /* haze with distance */
    const float void_[3] = {0.07f, 0.075f, 0.09f};   /* around the map */
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.s3.fbo);
    p_glViewport(0, 0, rw, rh);
    float bright = sc->brightness / 15.0f;
#ifdef CT_PS5
    p_glDisable(GL_DEPTH_TEST);
    p_glUseProgram(g.prog[P3_CLEAR]);
    p_glUniform4f(U(P3_CLEAR, "uC"), void_[0] * bright, void_[1] * bright, void_[2] * bright,
                  1.0f);
    p_glBindVertexArray(g.vao);
    draw_to(g.s3.fbo, rw, rh);
    p_glClear(GL_DEPTH_BUFFER_BIT);   /* (a depth clear is quick there) */
    p_glEnable(GL_DEPTH_TEST);
#else
    p_glClearColor(void_[0] * bright, void_[1] * bright, void_[2] * bright, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    p_glClearColor(0, 0, 0, 0);
#endif
    prof_mark("s3d_clear");   /* port: the scene pass in parts, for the readout */
    /* port: the models' program gets the same (asset packs) */
    for (int pass = g.m_ni ? 1 : 0; pass >= 0; pass--) {
    int P = pass ? P3_MSCENE : world ? P3_WSCENE : P3_SCENE;
    p_glUseProgram(g.prog[P]);
    p_glUniformMatrix4fv(U(P, "uMVP"), 1, GL_FALSE, mvp);
    p_glUniformMatrix4fv(U(P, "uLightMVP"), 1, GL_FALSE, lmvp);
    p_glUniform1i(U(P, "uVram"), 0);
    p_glUniform1i(U(P, "uCgram"), 1);
    p_glUniform1i(U(P, "uShadowMap"), 2);
    p_glUniform1i(U(P, "uLightMap"), 3);
    bind_tex(0, g.s3.t_vram);
    bind_tex(1, g.s3.t_cgram);
    bind_tex(2, g.s3.t_shadow);
    bind_tex(3, g.emit[0].tex);
    /* port: asset packs (tileTexel), units 10-12 */
    p_glUniform1i(U(P, "uPack"), g.pack3d_on);
    p_glUniform1i(U(P, "uPackLut"), 10);
    p_glUniform1i(U(P, "uPackRects"), 11);
    p_glUniform1i(U(P, "uPackAtlas"), 12);
    bind_tex(10, g.t_pack_lut);
    bind_tex(11, g.t_pack_rects);
    p_glActiveTexture(GL_TEXTURE0 + 12);
    p_glBindTexture(GL_TEXTURE_2D_ARRAY, g.t_pack_atlas);
    p_glUniform3f(U(P, "uLightDir"), L[0], L[1], L[2]);
    p_glUniform3f(U(P, "uKey"), 0.55f, 0.51f, 0.44f);
    p_glUniform3f(U(P, "uAmbient"), 0.47f, 0.5f, 0.58f);
    p_glUniform3f(U(P, "uEye"), eye[0], eye[1], eye[2]);
    p_glUniform2f(U(P, "uCam"), (float)sc->camera_x, (float)sc->camera_y);
    p_glUniform4f(U(P, "uFog"), fog[0], fog[1], fog[2], 0.6f * s->haze / 0.5f);
    p_glUniform4f(U(P, "uDepth"), dist, dist * 1.6f, bright,
                  shadows_3d ? 0.55f * s->shadow * a : 0.0f);
    if (!pass)
        p_glUniform1i(U(P, "uSmooth"), smooth_3d);
    if (P == P3_SCENE) {   /* port: the cards' snap toward the console's picture */
        static const float snap[3] = {0.0f, 0.8f, 1.0f};
        float t = g.s3.map_tilt * 3.14159265f / 180.0f;
        p_glUniform2f(U(P, "uCard"), user_cards ? snap[depth_3d] : 0.0f,
                      t > 0.05f ? tanf(t) : 0.05f);
        p_glUniform3f(U(P, "uEyeV"), eye[0], eye[1], eye[2]);
    }
    p_glUniform4f(U(P, "uMisc"), s->point_light * a * (0.25f + 0.75f * dark_amt), dist * 1.25f,
                  dist * 3.5f, 1.0f / S3D_SHADOW_SIZE);
    if (pass) {
        models_draw(P, (const float (*)[2])copies, n_copies);
        continue;
    }
    if (world) {   /* the world map's (scene3d_world.c) */
        bind_tex(6, g.s3.t_world_nrm);
        bind_tex(7, g.s3.t_overlay);
        p_glUniform1i(U(P, "uPassKind"), 0);
        p_glUniform1f(U(P, "uTime"), (float)(g.frame % 36000u) / 60.0f);
        p_glUniform1i(U(P, "uOverlayChr"), sc->layer[2].chr_base);
        p_glUniform1i(U(P, "uCloudPal"), g.s3.cloud_pal);
        p_glUniform4f(U(P, "uOverlay"), (float)sc->overlay_x, (float)sc->overlay_y,
                      8.0f * sc->overlay_w, 8.0f * sc->overlay_h);
        p_glUniform4f(U(P, "uCloud"), S3D_CLOUD_Y, sc->overlay_blend / 16.0f * 0.8f,
                      cloud_ni ? 0.3f * a : 0.0f, 0.0f);
        /* where the line of sight to the point looked at crosses the
           overlay: it thins out there, so it never hides the player */
        float t = eye[1] > S3D_CLOUD_Y ? (eye[1] - S3D_CLOUD_Y) / (eye[1] - at[1]) : 0.0f;
        p_glUniform2f(U(P, "uCloudClear"), eye[0] + (at[0] - eye[0]) * t,
                      eye[2] + (at[2] - eye[2]) * t);
    }
    prof_mark("s3d_setup");
    s3d_draw(0, P);
    }
    int P = world ? P3_WSCENE : P3_SCENE;
    if (world && cloud_ni) {   /* the overlay plane, blended over (its alpha: opacity) */
        p_glDepthMask(0);
        p_glEnable(GL_BLEND);
        p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        p_glUniform1i(U(P, "uPassKind"), 1);
        p_glBindVertexArray(g.s3.vao[1]);
        p_glDrawElements(GL_TRIANGLES, cloud_ni, GL_UNSIGNED_INT,
                         (const void *)(sizeof(uint32_t) * (size_t)cloud_first));
        p_glBindVertexArray(g.vao);
        p_glUniform1i(U(P, "uPassKind"), 0);
        p_glDisable(GL_BLEND);
        p_glDepthMask(1);
    }
    p_glDisable(GL_DEPTH_TEST);
    prof_mark("s3d_scene");

    /* Ambient occlusion at the render size / ao_div: its input from
       the render-size depth and material (aoprep.frag), the occlusion
       (ssao.frag), its bilateral blur, H and V. Applied by EASU below, or
       at the native render scale where the picture is read
       (scene3d_pic.frag). */
    g.s3.ao_amt = ao_amt;
    if (ao_amt > 0.0f) {
        float near_ = dist * 0.15f, far_ = dist * 5.0f;   /* m4_persp's above */
        p_glUseProgram(g.prog[P3_AOPREP]);
        p_glUniform1i(U(P3_AOPREP, "uDepth"), 0);
        p_glUniform1i(U(P3_AOPREP, "uMat"), 1);
        bind_tex(0, g.s3.t_depth);
        bind_tex(1, g.s3.t_mat);
        p_glUniform4f(U(P3_AOPREP, "uProj"), proj[0], proj[5], near_, far_);
        p_glUniform1f(U(P3_AOPREP, "uFocus"), dist);
        draw_to(g.s3.ao[2].fbo, g.s3.ao[2].w, g.s3.ao[2].h);
        prof_mark("s3d_aoprep");
        p_glUseProgram(g.prog[P3_AO]);
        p_glUniform1i(U(P3_AO, "uPrep"), 0);
        bind_tex(0, g.s3.ao[2].tex);
        p_glUniformMatrix4fv(U(P3_AO, "uView"), 1, GL_FALSE, view);
        p_glUniform4f(U(P3_AO, "uProj"), proj[0], proj[5], near_, far_);
        int n = div > 2 ? 8 : 12;   /* samples */
        float kernel[16 * 3];
        s3d_ao_kernel(kernel, n);
        p_glUniform3fv(U(P3_AO, "uKernel"), n, kernel);
        p_glUniform3f(U(P3_AO, "uRadius"), g.s3.ao_radius, (float)n, dist);
        p_glUniform1f(U(P3_AO, "uRange"), dist * 1.6f);   /* scene3d.frag's uDepth */
        draw_to(g.s3.ao[0].fbo, g.s3.ao[0].w, g.s3.ao[0].h);
        prof_mark("s3d_ao");
        p_glUseProgram(g.prog[P3_AOBLUR]);
        p_glUniform1i(U(P3_AOBLUR, "uSrc"), 0);
        bind_tex(0, g.s3.ao[0].tex);
        p_glUniform2f(U(P3_AOBLUR, "uDir"), 1.0f / g.s3.ao[0].w, 0.0f);
        draw_to(g.s3.ao[1].fbo, g.s3.ao[1].w, g.s3.ao[1].h);
        bind_tex(0, g.s3.ao[1].tex);
        p_glUniform2f(U(P3_AOBLUR, "uDir"), 0.0f, 1.0f / g.s3.ao[1].h);
        draw_to(g.s3.ao[0].fbo, g.s3.ao[0].w, g.s3.ao[0].h);
        prof_mark("s3d_aoblur");
    }

    /* Below a render scale of 1: FSR 1's EASU up to the picture size
       (fsr_easu.frag; FsrEasuCon's con0). The UI then goes on at full
       size; RCAS sharpens in the final pass. */
    g.s3.scaled = rw != pw || rh != ph;
    g.s3.pic_w = pw;
    g.s3.pic_h = ph;
    if (g.s3.scaled) {
        p_glUseProgram(g.prog[P3_EASU]);
        p_glUniform1i(U(P3_EASU, "uSrc"), 0);
        p_glUniform1i(U(P3_EASU, "uSrcMat"), 1);
        p_glUniform1i(U(P3_EASU, "uAoTex"), 2);
        p_glUniform1f(U(P3_EASU, "uAo"), ao_amt);
        bind_tex(0, g.s3.t_color);
        bind_tex(1, g.s3.t_mat);
        bind_tex(2, g.s3.ao[0].tex ? g.s3.ao[0].tex : g.t_mat);
        float sx = (float)rw / (float)pw, sy = (float)rh / (float)ph;
        p_glUniform4f(U(P3_EASU, "uCon0"), sx, sy, 0.5f * sx - 0.5f, 0.5f * sy - 0.5f);
        draw_to(g.s3.fbo_up, pw, ph);
        prof_mark("s3d_easu");
    } else {
        p_glBindFramebuffer(GL_FRAMEBUFFER, g.s3.fbo);
        p_glViewport(0, 0, pw, ph);
    }

    /* The UI, flat on top, at the picture's size. */
    p_glUseProgram(g.prog[P3_UI]);
    p_glUniform1i(U(P3_UI, "uColor"), 0);
    p_glUniform1i(U(P3_UI, "uLayer"), 1);
    p_glUniform1i(U(P3_UI, "uUiZ"), world ? -1 : ui_z);   /* the world map's BG3 is clouds */
    p_glUniform1i(U(P3_UI, "uEncode"), g.s3.scaled);
    p_glUniform1i(U(P3_UI, "uBox"), 2);
    bind_tex(0, g.t_color);
    bind_tex(1, g.t_layer);
    bind_tex(2, g.s3.t_box);
    p_glDrawArrays(GL_TRIANGLES, 0, 3);
    return 0;
}

/* Read one opaque BG pixel from the tilemap the scene module supplied.
   Field maps store two 8 px tiles per cell; world maps already use 8 px
   cells. Both use the same fixed 256-tile row stride. */
static int s3d_scene_pixel(const ct_scene *sc, int layer, int x, int y, uint8_t rgb[3])
{
    if (!sc || layer < 0 || layer > 1 || !sc->vram || !sc->cgram || !sc->brightness ||
        !sc->layer[layer].present || !sc->layer[layer].tiles)
        return 0;
    int tw = sc->mode == CT_SCENE_WORLD ? sc->map_w : 2 * sc->map_w;
    int th = sc->mode == CT_SCENE_WORLD ? sc->map_h : 2 * sc->map_h;
    int mapw = tw * 8, maph = th * 8;
    int mx = sc->camera_x + x, my = sc->camera_y + y;
    if (sc->wrap && mapw > 0 && maph > 0) {
        mx = (mx % mapw + mapw) % mapw;
        my = (my % maph + maph) % maph;
    } else if (mx < 0 || my < 0 || mx >= mapw || my >= maph) {
        return -1;
    }
    const ct_scene_layer *L = &sc->layer[layer];
    if (L->bpp != 2 && L->bpp != 4)
        return 0;
    int tx = mx >> 3, ty = my >> 3, u = mx & 7, v = my & 7;
    uint16_t e = L->tiles[(size_t)ty * CT_SCENE_STRIDE + tx];
    int row = e & 0x8000 ? 7 - v : v;
    int sh = e & 0x4000 ? u : 7 - u;
    uint32_t a = (L->chr_base + (e & 0x3FFu) * (L->bpp == 2 ? 8u : 16u)) & 0x7FFFu;
    unsigned ci = 0;
    for (int plane = 0; plane < L->bpp; plane++) {
        uint16_t bits = sc->vram[(a + (unsigned)(plane / 2) * 8u + (unsigned)row) & 0x7FFFu];
        ci |= (unsigned)((bits >> (sh + (plane & 1) * 8)) & 1u) << plane;
    }
    if (!ci)   /* color zero is transparent on BG1/BG2 */
        return 0;
    unsigned pal_size = 1u << L->bpp;
    uint16_t c = sc->cgram[(L->pal_base + ((e >> 10) & 7u) * pal_size + ci) & 255u];
    unsigned r = (c & 31u) * sc->brightness / 15u;
    unsigned g = (c >> 5 & 31u) * sc->brightness / 15u;
    unsigned b = (c >> 10 & 31u) * sc->brightness / 15u;
    rgb[0] = (uint8_t)((b << 3) | (b >> 2));
    rgb[1] = (uint8_t)((g << 3) | (g >> 2));
    rgb[2] = (uint8_t)((r << 3) | (r >> 2));
    return 1;
}

/* Cheap, game-independent scene gate. A regular grid of the console's
   winning BG1/BG2 pixels must agree with the same tile coordinates in the
   module's map description. If the map is stale, shifted, blanked or only
   partially loaded, the caller keeps the frame on the 2D path. */
static int s3d_scene_matches_console(const ct_scene *sc, const uint8_t *bgrx,
                                    const uint8_t *layers, const sched_ppu_state *st, int ui_z)
{
    if (!sc || !bgrx || !layers || !sc->vram || !sc->cgram || !sc->brightness ||
        (sc->mode != CT_SCENE_FIELD && sc->mode != CT_SCENE_WORLD) || (st && st->mode == 7))
        return 0;
    static const int sx[] = {16, 60, 104, 148, 192, 236};
    static const int sy[] = {16, 54, 92, 130, 168, 206};
    int compared = 0, different = 0;
    for (size_t yi = 0; yi < sizeof sy / sizeof sy[0]; yi++)
        for (size_t xi = 0; xi < sizeof sx / sizeof sx[0]; xi++) {
            int x = sx[xi], y = sy[yi], k = y * SCHED_WIDTH + x;
            uint8_t tag = layers[k], layer = tag & 7u;
            if (layer > 1 || (ui_z >= 0 && (tag >> 4) == ui_z) || (tag & 8u))
                continue;
            if (st && !(st->main_enabled & (1u << layer)))
                continue;
            uint8_t expected[3];
            int got = s3d_scene_pixel(sc, layer, x, y, expected);
            if (!got)
                continue;
            compared++;
            if (got < 0 || abs((int)bgrx[k * 4 + 0] - expected[0]) > 12 ||
                abs((int)bgrx[k * 4 + 1] - expected[1]) > 12 ||
                abs((int)bgrx[k * 4 + 2] - expected[2]) > 12)
                different++;
        }
    /* Too few comparable map pixels is itself inconclusive: use the safe
       renderer. A small number of local differences is allowed for windows
       and sprites crossing sample points. */
    return compared >= 6 && different * 4 <= compared;
}

static int s3d_has_color_math(const uint8_t *layers)
{
    if (layers)
        for (int k = 0; k < SCHED_WIDTH * SCHED_HEIGHT; k++)
            if (layers[k] & 8u)
                return 1;
    return 0;
}

void hd2d_render(const uint8_t *bgrx, const uint8_t *layers, const uint8_t *behind,
                 const uint8_t *behind_layers, const sched_ppu_state *st, int ow, int oh,
                 int offscreen)
{
    if (!g.ready || ow < 8 || oh < 8)
        return;
    /* The profile's own readback goes before the clock starts. */
    prof.slot = (prof.slot + 1) % PROF_RING;
    prof_flush(prof.slot);
    uint64_t t0 = 0;
    if (timing) {
        p_glFinish();
        t0 = SDL_GetPerformanceCounter();
    }
    uint64_t tc = SDL_GetPerformanceCounter();
    int px, py, pw, ph;
    picture_rect(ow, oh, &px, &py, &pw, &ph);
    if (resize(ow, oh, pw, ph))
        return;
    const settings *s = &presets[cur_preset];
    /* Furniture/sprite overlap has no reliable 3D height or pose metadata.
       Preserve the complete console composition, including the blanket. */
    if ((g.s3.scene && g.s3.scene->mode == CT_SCENE_UI) || (st && st->mode == 7) ||
        (s->scene3d && (s3d_field_needs_2d(g.s3.scene) || s3d_has_color_math(layers))))
        s = &presets[HD2D_OFF];
    g.frame++;

    int ui_z;
    float ui_frac, field_frac, focus_target, mean_luma;
    analyze(bgrx, layers, st, &ui_z, &ui_frac, &field_frac, &focus_target, &mean_luma);
    prof.analyze_ms[prof.slot] = ms_since(tc);
    /* Point lights matter in the dark: full below 0.18 mean brightness,
       gone above 0.42 (daylight). */
    float dark_target = mean_luma < 0.18f ? 1.0f : mean_luma > 0.42f ? 0.0f
                                                 : (0.42f - mean_luma) / 0.24f;
    dark_amt += (dark_target - dark_amt) * 0.1f;
    g.focus_y += (focus_target - g.focus_y) * 0.08f;
    /* Menus and full-screen text: ease the scene effects off. */
    float scene_amt = ui_frac > 0.3f ? 0.0f : ui_frac > 0.15f ? (0.3f - ui_frac) / 0.15f : 1.0f;
    if (st && st->mode == 7)
        ui_z = -1;
    /* Tilt-shift only where there is a scene to look down on: none on
       title cards and black-backed screens, full from 45% ground up. */
    float field_target = field_frac < 0.2f ? 0.0f : field_frac > 0.45f ? 1.0f
                                                   : (field_frac - 0.2f) / 0.25f;
    field_amt += (field_target - field_amt) * 0.1f;

    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_BLEND);
    p_glDisable(GL_SCISSOR_TEST);
    p_glBindVertexArray(g.vao);

    /* Uploads. */
    uint64_t tu = SDL_GetPerformanceCounter();
    prof_mark("start");
    bind_tex(0, g.t_color);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCHED_WIDTH, SCHED_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE,
                      bgrx);
    bind_tex(1, g.t_layer);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCHED_WIDTH, SCHED_HEIGHT, GL_RED_INTEGER,
                      GL_UNSIGNED_BYTE, layers);
    /* Without the behind G-buffer the front layer stands in for it: the
       planes then match the old single-plane depth of field, and the
       tilt is off (it would have nothing to reveal). */
    int have_behind = behind && behind_layers;
    bind_tex(3, g.t_behind);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCHED_WIDTH, SCHED_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE,
                      have_behind ? behind : bgrx);
    bind_tex(4, g.t_blayer);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCHED_WIDTH, SCHED_HEIGHT, GL_RED_INTEGER,
                      GL_UNSIGNED_BYTE, have_behind ? behind_layers : layers);
    /* port: the asset pack's per-pixel map, when there is one */
    g.pack_on = g.pack_entries > 0 && g.pack_map != NULL;
    if (g.pack_on) {
        bind_tex(5, g.t_pack_map);
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCHED_WIDTH, SCHED_HEIGHT, GL_RGBA_INTEGER,
                          GL_UNSIGNED_SHORT, g.pack_map);
        g.pack_have_behind = have_behind;
    }
    prof.upload_ms[prof.slot] = ms_since(tu);
    prof_mark("texup");

    /* 0: light sources -> blurred light map. */
    p_glUseProgram(g.prog[P_EMIT]);
    p_glUniform1i(U(P_EMIT, "uColor"), 0);
    p_glUniform1i(U(P_EMIT, "uLayer"), 1);
    p_glUniform1i(U(P_EMIT, "uUiZ"), ui_z);
    p_glUniform1f(U(P_EMIT, "uThreshold"), 0.66f);
    draw_to(g.emit[0].fbo, g.emit[0].w, g.emit[0].h);
    gauss(&g.emit[0], &g.emit[1], &g.emit[0]);
    gauss(&g.emit[0], &g.emit[1], &g.emit[0]);
    prof_mark("emit");
    bind_tex(0, g.t_color);
    bind_tex(1, g.t_layer);

    /* The 3D diorama is allowed only when its map agrees with the console's
       visible BG pixels. A missing or mismatched scene goes straight to 2D;
       fading an old 3D frame would briefly show the wrong map. */
    const ct_scene *sc = g.s3.scene;
    int scene_candidate = s->scene3d && g.s3.ok && sc &&
                         (sc->mode == CT_SCENE_FIELD || sc->mode == CT_SCENE_WORLD) && sc->vram &&
                         !s3d_scene_exact_2d(sc) &&
                         !(st && st->mode == 7) &&
                         (sc->mode != CT_SCENE_FIELD || sc->player < sc->n_objects) &&
                         !(st && !((st->main_enabled | st->sub_enabled) & 3));
    int scene_matches = scene_candidate && s3d_scene_matches_console(sc, bgrx, layers, st, ui_z);
    int want3 = scene_candidate && scene_matches;
    int focus_change = 0;
    float aperture = s3d_focus_step(&g.s3.focus, sc, want3, &focus_change);
    if (focus_change & S3D_SCENE_CUT)
        g.s3.amt = 0.0f;
    g.s3.amt = s->scene3d && want3 ? fminf(g.s3.amt + S3D_FADE_STEP, 1.0f) : 0.0f;
    float amt3 = g.s3.amt;
    int world2d = sc && sc->mode == CT_SCENE_WORLD;

    if (amt3 < 1.0f)
        light_pass(s, ui_z, scene_amt);

    /* The on-screen message (5 draws it). */
    float osd_alpha = 0.0f;
    if (g.osd_frames > 0) {
        if (g.osd_dirty)
            osd_upload();
        osd_alpha = g.osd_frames < 20 ? g.osd_frames / 20.0f : 1.0f;
        if (!offscreen)
            g.osd_frames--;   /* screenshots don't use up the message's time */
    }

    if (amt3 < 1.0f) {
        /* 2: the upscale into two planes, with the tilt: worked out in the
           passes that read the picture (bind_picture). */
        float tilt = have_behind && !world2d ? s->tilt * scene_amt * field_amt : 0.0f;
        /* Depth of field settings, shared by passes 3 and 5. */
        float dof_tilt = world2d ? 0.0f : s->dof_tilt * scene_amt * field_amt;
        float dof_depth = world2d ? 0.0f : s->dof_depth * scene_amt * field_amt;
        post(s, px, py, pw, ph, ow, oh, offscreen, tilt, dof_tilt, dof_depth, osd_alpha, 0,
             0.0f, 1.0f);
    }
    if (amt3 > 0.0f) {
        if (want3)
            s3d_box_mask(sc, bgrx, layers, ui_z);
        if (want3 && s3d_render(sc, s, pw, ph, ui_z, scene_amt)) {
            g.s3.amt = 0.0f;
            s3d_focus_step(&g.s3.focus, NULL, 0, NULL);
            /* Allocation failure must also replace a fully opaque 3D frame
               immediately; no 2D pass was drawn above when amt3 was 1. */
            if (amt3 >= 1.0f) {
                light_pass(s, ui_z, scene_amt);
                float tilt = have_behind && !world2d ? s->tilt * scene_amt * field_amt : 0.0f;
                post(s, px, py, pw, ph, ow, oh, offscreen, tilt,
                     world2d ? 0.0f : s->dof_tilt * scene_amt * field_amt,
                     world2d ? 0.0f : s->dof_depth * scene_amt * field_amt, osd_alpha, 0, 0.0f,
                     1.0f);
            }
        } else
            /* Real depth now: the tilt-shift band only a little, the depth
               term doing most of the blur. */
            post(s, px, py, pw, ph, ow, oh, offscreen, 0.0f, s->dof_tilt * 0.25f * scene_amt,
                 0.9f * scene_amt, osd_alpha, 1, amt3, aperture);
    }
    prof.frame[prof.slot] = g.frame;
    prof.submit_ms[prof.slot] = ms_since(tc);

    if (timing) {
        p_glFinish();
        last_ms = ms_since(t0);
    }
    prof.total_ms[prof.slot] = timing ? last_ms : 0.0;

    /* CT_HD2D_GLERR=1: report GL errors a lenient driver lets through
       (a strict one, NVIDIA's for example, may drop the draw). */
    static int check = -1, reported;
    if (check < 0) {
        const char *e = getenv("CT_HD2D_GLERR");
        check = e && *e && *e != '0';
    }
    if (check)
        for (GLenum err; reported < 20 && (err = p_glGetError()) != GL_NO_ERROR; reported++)
            fprintf(stderr, "hd2d: GL error 0x%x after frame %u\n", err, g.frame);
}

int hd2d_read(uint8_t *bgrx, int ow, int oh)
{
    if (!g.ready || ow != g.out_w || oh != g.out_h)
        return -1;
    uint8_t *rgba = malloc((size_t)ow * oh * 4);
    if (!rgba)
        return -1;
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.out.fbo);
    p_glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* The offscreen target is in picture order already (row 0 = top). */
    for (size_t k = 0; k < (size_t)ow * oh; k++) {
        bgrx[k * 4 + 0] = rgba[k * 4 + 2];
        bgrx[k * 4 + 1] = rgba[k * 4 + 1];
        bgrx[k * 4 + 2] = rgba[k * 4 + 0];
        bgrx[k * 4 + 3] = 0;
    }
    free(rgba);
    return p_glGetError() == GL_NO_ERROR ? 0 : -1;
}
