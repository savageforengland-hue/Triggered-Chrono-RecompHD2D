/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port: audio packs (music.h). */
#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#endif
#include "music.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "packs.h"
#include "resample.h"

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#include "dr_flac.h"
#include "dr_wav.h"

#define MAX_DIRS 16
#define FRAME_HZ 60.0988   /* NTSC frames per second: the game's fade times are in frames */
#define DECLICK_S 0.02     /* a stop, start or switch ramps over this long */

/* ---- decoders: one stream of 16-bit stereo frames, whatever the file ---- */

enum { DEC_NONE, DEC_VORBIS, DEC_FLAC, DEC_WAV, DEC_MSU1 };

typedef struct {
    int kind;
    stb_vorbis *vorbis;
    drflac *flac;
    drwav wav;
    FILE *pcm;
    uint32_t loop_start;
    int channels, rate;
    uint64_t frames;   /* 0: unknown */
} decoder;

static int has_ext(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    return n > e && !strcasecmp(path + n - e, ext);
}

#ifdef _WIN32
#define pcm_seek _fseeki64
#define pcm_tell _ftelli64
#elif defined(CT_PS5)
/* Native titles already import fseek/ftell for the other audio codecs.
   PS5 uses LP64, so these retain 64-bit offsets without new libc imports. */
_Static_assert(sizeof(long) >= 8, "PS5 PCM streams require 64-bit file offsets");
#define pcm_seek fseek
#define pcm_tell ftell
#else
#define pcm_seek fseeko
#define pcm_tell ftello
#endif

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int dec_open(decoder *d, const char *path, char *err, size_t errlen)
{
    memset(d, 0, sizeof *d);
    if (has_ext(path, ".pcm")) {
        unsigned char header[8];
        d->pcm = fopen(path, "rb");
        if (!d->pcm) {
            snprintf(err, errlen, "cannot open MSU-1 PCM");
            return -1;
        }
        d->kind = DEC_MSU1;
        if (fread(header, 1, sizeof header, d->pcm) != sizeof header ||
            memcmp(header, "MSU1", 4) || pcm_seek(d->pcm, 0, SEEK_END)) {
            snprintf(err, errlen, "invalid MSU-1 header");
            return -1;
        }
        int64_t bytes = pcm_tell(d->pcm);
        if (bytes < 12 || (bytes - 8) % 4 || pcm_seek(d->pcm, 8, SEEK_SET)) {
            snprintf(err, errlen, "empty or truncated MSU-1 stereo data");
            return -1;
        }
        d->frames = (uint64_t)(bytes - 8) / 4;
        d->loop_start = read_le32(header + 4);
        if (d->loop_start >= d->frames) {
            snprintf(err, errlen, "MSU-1 loop point outside audio data");
            return -1;
        }
        d->channels = 2;
        d->rate = 44100;
    } else if (has_ext(path, ".ogg") || has_ext(path, ".oga")) {
        int e = 0;
        d->vorbis = stb_vorbis_open_filename(path, &e, NULL);
        if (!d->vorbis) {
            snprintf(err, errlen, "not a readable Ogg Vorbis file (stb_vorbis error %d)", e);
            return -1;
        }
        stb_vorbis_info i = stb_vorbis_get_info(d->vorbis);
        d->kind = DEC_VORBIS;
        d->channels = i.channels;
        d->rate = (int)i.sample_rate;
        d->frames = stb_vorbis_stream_length_in_samples(d->vorbis);
    } else if (has_ext(path, ".flac")) {
        d->flac = drflac_open_file(path, NULL);
        if (!d->flac) {
            snprintf(err, errlen, "not a readable FLAC file");
            return -1;
        }
        d->kind = DEC_FLAC;
        d->channels = d->flac->channels;
        d->rate = (int)d->flac->sampleRate;
        d->frames = d->flac->totalPCMFrameCount;
    } else if (has_ext(path, ".wav")) {
        if (!drwav_init_file(&d->wav, path, NULL)) {
            snprintf(err, errlen, "not a readable WAV file");
            return -1;
        }
        d->kind = DEC_WAV;
        d->channels = d->wav.channels;
        d->rate = (int)d->wav.sampleRate;
        d->frames = d->wav.totalPCMFrameCount;
    } else {
        snprintf(err, errlen, "not .ogg, .flac, .wav or MSU-1 .pcm");
        return -1;
    }
    if (d->channels < 1 || d->rate < 4000 || d->rate > 384000) {
        snprintf(err, errlen, "%d channels at %d Hz: not playable", d->channels, d->rate);
        return -1;
    }
    return 0;
}

static void dec_close(decoder *d)
{
    if (d->kind == DEC_VORBIS)
        stb_vorbis_close(d->vorbis);
    else if (d->kind == DEC_FLAC)
        drflac_close(d->flac);
    else if (d->kind == DEC_WAV)
        drwav_uninit(&d->wav);
    else if (d->kind == DEC_MSU1)
        fclose(d->pcm);
    d->kind = DEC_NONE;
}

/* Up to n frames as stereo (mono doubled, channels past two left out);
   returns how many (0 at the end). */
static int dec_read(decoder *d, int16_t *out, int n)
{
    static int16_t tmp[1024 * 8];
    int ch = d->channels, got = 0;
    if (n > 1024)
        n = 1024;
    if (d->kind == DEC_VORBIS) {
        int want = ch >= 2 ? 2 : 1;
        got = stb_vorbis_get_samples_short_interleaved(d->vorbis, want, tmp, n * want);
        ch = want;
    } else if (d->kind == DEC_FLAC) {
        if (ch > 8)
            return 0;
        got = (int)drflac_read_pcm_frames_s16(d->flac, (drflac_uint64)n, tmp);
    } else if (d->kind == DEC_WAV) {
        if (ch > 8)
            return 0;
        got = (int)drwav_read_pcm_frames_s16(&d->wav, (drwav_uint64)n, tmp);
    }
    if (d->kind == DEC_MSU1) {
        unsigned char raw[1024 * 4];
        got = (int)fread(raw, 4, (size_t)n, d->pcm);
        for (int k = 0; k < got * 2; k++) {
            unsigned v = (unsigned)raw[2 * k] | (unsigned)raw[2 * k + 1] << 8;
            tmp[k] = (int16_t)(v >= 32768 ? (int)v - 65536 : (int)v);
        }
    }
    for (int k = 0; k < got; k++) {
        out[2 * k] = tmp[k * ch];
        out[2 * k + 1] = tmp[k * ch + (ch > 1)];
    }
    return got;
}

static int dec_seek(decoder *d, uint64_t frame)
{
    if (d->kind == DEC_MSU1)
        return frame < d->frames && !pcm_seek(d->pcm, 8 + (int64_t)frame * 4, SEEK_SET) ? 0 : -1;
    if (d->kind == DEC_VORBIS)
        return stb_vorbis_seek(d->vorbis, (unsigned)frame) ? 0 : -1;
    if (d->kind == DEC_FLAC)
        return drflac_seek_to_pcm_frame(d->flac, frame) ? 0 : -1;
    if (d->kind == DEC_WAV)
        return drwav_seek_to_pcm_frame(&d->wav, frame) ? 0 : -1;
    return -1;
}

/* ---- the entries ---- */

typedef struct {
    int song;
    char file[4096];
    int loop;
    int64_t loop_start, loop_end;   /* loop_end -1: the end of the file */
    double volume;
    int follow_fades;
} entry;

static struct {
    char dirs[MAX_DIRS][4096];
    int ndirs;
    entry *e;
    int n;
} L;

static const entry *find(int song)
{
    for (int k = 0; k < L.n; k++)
        if (L.e[k].song == song)
            return &L.e[k];
    return NULL;
}

static int truthy(const char *v)
{
    return !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") ||
           !strcmp(v, "1");
}

static void add(const entry *e)
{
    if (e->song < 0 || e->song > 0xFFFF || !e->file[0] || find(e->song))
        return;   /* incomplete, or a pack before this one has the song */
    entry *ne = realloc(L.e, (size_t)(L.n + 1) * sizeof *ne);
    if (!ne)
        return;
    L.e = ne;
    L.e[L.n++] = *e;
}

static void load_one(const char *dir)
{
    char path[4096], line[2048];
    snprintf(path, sizeof path, "%s/pack.toml", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;   /* packs.c says so */
    packs_toml_line t = {0};
    entry e;
    int in_music = 0, before = L.n;
    memset(&e, 0, sizeof e);
    for (int eof = 0; !eof;) {
        int r = fgets(line, sizeof line, f) ? packs_toml_parse(line, &t) : (eof = 1, 1);
        if (r == 1) {
            if (in_music)
                add(&e);
            in_music = !eof && t.is_array && !strcmp(t.table, "music");
            memset(&e, 0, sizeof e);
            e.song = -1;
            e.loop = 1;
            e.loop_start = -1;
            e.loop_end = -1;
            e.volume = 1.0;
            e.follow_fades = 1;
        } else if (r == 2 && in_music) {
            if (!strcmp(t.key, "song"))
                e.song = (int)strtol(t.val, NULL, 0);
            else if (!strcmp(t.key, "file")) {
                if (snprintf(e.file, sizeof e.file, "%s/%s", dir, t.val) >= (int)sizeof e.file)
                    e.file[0] = 0;
            } else if (!strcmp(t.key, "loop"))
                e.loop = truthy(t.val);
            else if (!strcmp(t.key, "loop_start"))
                e.loop_start = strtoll(t.val, NULL, 0);
            else if (!strcmp(t.key, "loop_end"))
                e.loop_end = strtoll(t.val, NULL, 0);
            else if (!strcmp(t.key, "volume")) {
                e.volume = strtod(t.val, NULL);
                e.volume = e.volume < 0 ? 0 : e.volume > 4 ? 4 : e.volume;
            } else if (!strcmp(t.key, "fade"))
                e.follow_fades = strcasecmp(t.val, "none") != 0;
        }
    }
    fclose(f);
    if (L.n > before)
        fprintf(stderr, "music: %s: %d songs\n", dir, L.n - before);
}

/* ---- playback ---- */

typedef struct {
    const entry *e;
    decoder d;
    resampler rs;
    uint64_t loop_start; /* manifest override, otherwise decoder metadata */
    uint64_t pos;        /* the file's next frame */
    int ended;           /* the file is over (no loop) */
    double ramp;         /* 0-1: the declick envelope */
    double ramp_step;    /* per output frame: + rising, - falling (and then closed) */
} voice;

static voice cur, old;   /* playing; and fading out after a switch */
static int out_rate = 48000;
static FILE *log_f;
static int replacing;
static double game_gain = 1.0, game_target = 1.0, game_step;   /* the game's fades */

static void voice_close(voice *v)
{
    if (v->e)
        dec_close(&v->d);
    memset(v, 0, sizeof *v);
}

static int voice_open(voice *v, const entry *e)
{
    char err[256];
    memset(v, 0, sizeof *v);
    if (dec_open(&v->d, e->file, err, sizeof err)) {
        fprintf(stderr, "music: %s: %s\n", e->file, err);
        dec_close(&v->d);
        return -1;
    }
    v->loop_start = e->loop_start >= 0 ? (uint64_t)e->loop_start : v->d.loop_start;
    uint64_t end = e->loop_end >= 0 ? (uint64_t)e->loop_end : v->d.frames;
    if (e->loop && end && (v->loop_start >= end || (v->d.frames && end > v->d.frames))) {
        fprintf(stderr, "music: %s: invalid loop range\n", e->file);
        dec_close(&v->d);
        return -1;
    }
    v->e = e;
    rs_init(&v->rs, v->d.rate, out_rate);
    v->ramp = 0.0;
    v->ramp_step = 1.0 / (DECLICK_S * out_rate);
    return 0;
}

static void voice_fade_out(voice *v)
{
    if (v->e)
        v->ramp_step = -1.0 / (DECLICK_S * out_rate);
}

/* Up to n frames of v (before its gain) into out; returns how many. */
static int voice_render(voice *v, int16_t *out, int n)
{
    int done = 0;
    /* A valid PCM loop can contain just one frame. Bound work by the
       source frames needed, rather than starving after 64 tiny loops. */
    uint64_t budget = 576 + (uint64_t)n * (unsigned)v->d.rate / (unsigned)out_rate;
    for (uint64_t guard = 0; done < n && guard < budget; guard++) {
        int got = rs_process(&v->rs, NULL, 0, out + 2 * done, n - done);
        done += got > 0 ? got : 0;
        if (done >= n || v->ended)
            break;
        int16_t in[512 * 2];
        int want = 512;
        uint64_t end = v->e->loop_end >= 0 ? (uint64_t)v->e->loop_end : v->d.frames;
        if (end && v->pos + (uint64_t)want > end)
            want = v->pos < end ? (int)(end - v->pos) : 0;
        int m = want > 0 ? dec_read(&v->d, in, want) : 0;
        v->pos += (uint64_t)m;
        if (m > 0) {
            got = rs_process(&v->rs, in, m, out + 2 * done, n - done);
            done += got > 0 ? got : 0;
        }
        if (m < want || (end && v->pos >= end) || m == 0) {   /* at the loop's end */
            if (v->e->loop && v->pos > v->loop_start &&
                !dec_seek(&v->d, v->loop_start))
                v->pos = v->loop_start;
            else
                v->ended = 1;
        }
    }
    return done;
}

static void mix_voice(voice *v, int16_t *stereo, int n, double gain)
{
    static int16_t buf[4096 * 2];
    while (n > 0 && v->e) {
        int chunk = n > 4096 ? 4096 : n;
        int got = voice_render(v, buf, chunk);
        for (int k = 0; k < got; k++) {
            double g = gain * v->ramp;
            for (int c = 0; c < 2; c++) {
                long s = stereo[2 * k + c] + lrint(buf[2 * k + c] * g);
                stereo[2 * k + c] = (int16_t)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
            }
            v->ramp += v->ramp_step;
            if (v->ramp >= 1.0) {
                v->ramp = 1.0;
                v->ramp_step = 0.0;
            } else if (v->ramp <= 0.0) {
                voice_close(v);
                return;
            }
        }
        if (got < chunk)
            return;   /* ended */
        stereo += 2 * chunk;
        n -= chunk;
    }
}

static void log_event(long frame, const char *what, int song)
{
    if (!log_f)
        return;
    const char *file = song >= 0 ? music_file(song) : NULL;
    if (song >= 0)
        fprintf(log_f, "music: frame %ld: %s song 0x%02X (%s)\n", frame, what, song,
                file ? file : "not in a pack");
    else
        fprintf(log_f, "music: frame %ld: %s\n", frame, what);
    fflush(log_f);
}

void music_event(const ct_music_event *e, long frame)
{
    if (e->kind == CT_MUSIC_PLAY) {
        const entry *en = find(e->song);
        log_event(frame, "play", e->song);
        if (en && cur.e == en && cur.ramp_step >= 0.0 && !cur.ended)
            return;   /* the same song again (a new room with the same music): it plays on */
        voice_close(&old);
        old = cur;
        voice_fade_out(&old);
        memset(&cur, 0, sizeof cur);
        replacing = en && !voice_open(&cur, en);
    } else if (e->kind == CT_MUSIC_STOP) {
        log_event(frame, "stop", -1);
        voice_fade_out(&cur);
        replacing = 0;
    } else if (e->kind == CT_MUSIC_FADE) {
        if (log_f)
            fprintf(log_f, "music: frame %ld: volume to %d/255 over %d frames\n", frame,
                    e->volume, e->frames);
        game_target = e->volume / 255.0;
        if (!e->frames) {
            game_gain = game_target;
            game_step = 0.0;
        } else {
            game_step = (game_target - game_gain) / (e->frames * out_rate / FRAME_HZ);
        }
    }
}

void music_stop(void)
{
    voice_close(&cur);
    voice_close(&old);
    replacing = 0;
    game_gain = game_target = 1.0;
    game_step = 0.0;
}

int music_replacing(void) { return replacing; }

double music_gain(void)
{
    double g = cur.e ? cur.e->volume : 1.0;
    return (cur.e && !cur.e->follow_fades) ? g : g * game_gain;
}

void music_mix(int16_t *stereo, int n)
{
    if (!cur.e && !old.e) {
        /* nothing plays, but the game's fades go on in time */
        if (game_step != 0.0) {
            game_gain += game_step * n;
            if ((game_step > 0) == (game_gain >= game_target)) {
                game_gain = game_target;
                game_step = 0.0;
            }
        }
        return;
    }
    /* The game's fade moves within the buffer, in steps of 64 frames. */
    for (int at = 0; at < n; at += 64) {
        int m = n - at < 64 ? n - at : 64;
        if (old.e)
            mix_voice(&old, stereo + 2 * at, m,
                      old.e->volume * (old.e->follow_fades ? game_gain : 1.0));
        if (cur.e)
            mix_voice(&cur, stereo + 2 * at, m, music_gain());
        if (game_step != 0.0) {
            game_gain += game_step * m;
            if ((game_step > 0) == (game_gain >= game_target)) {
                game_gain = game_target;
                game_step = 0.0;
            }
        }
    }
}

/* ---- loading ---- */

static int load_dirs(void)
{
    music_unload();
    for (int k = 0; k < L.ndirs; k++)
        load_one(L.dirs[k]);
    return L.n;
}

int music_load(const char *const *dirs, int n)
{
    L.ndirs = 0;
    for (int k = 0; k < n && k < MAX_DIRS; k++)
        snprintf(L.dirs[L.ndirs++], sizeof L.dirs[0], "%s", dirs[k]);
    return load_dirs();
}

int music_reload(void) { return load_dirs(); }

void music_unload(void)
{
    music_stop();
    free(L.e);
    L.e = NULL;
    L.n = 0;
}

int music_count(void) { return L.n; }

const char *music_file(int song)
{
    const entry *e = find(song);
    return e ? e->file : NULL;
}

void music_set_rate(int hz)
{
    if (hz > 0 && hz != out_rate) {
        music_stop();
        out_rate = hz;
    }
}

void music_set_log(FILE *f) { log_f = f; }
