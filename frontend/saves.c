/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Save RAM on disk: see saves.h. */
#include "saves.h"
#include "ps5.h"   /* port: ps5_notify */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "bus.h"

#define PATHLEN 4096

static char path[PATHLEN];   /* "" while saving is off */
static uint8_t on_disk[CT_SRAM_SIZE];   /* what the file holds */
static uint8_t prev[CT_SRAM_SIZE];      /* the SRAM at the previous frame edge */
static Uint32 last_write, dirty_since;
static int have_write, dirty, exit_hooked;
static const char *dump_path;
static long cur_frame = -1;   /* for the log: the frame a timed write follows */

static void at_exit(void);

static void hook_exit(void)
{
    if (!exit_hooked) {
        exit_hooked = 1;
        atexit(at_exit);
    }
}

/* Move a wrong-size file out of the way: FILE.bad, else FILE.bad.N. */
static void set_aside(void)
{
    char bad[PATHLEN + 16];
    for (int n = 0; n < 100; n++) {
        if (n)
            snprintf(bad, sizeof bad, "%s.bad.%d", path, n);
        else
            snprintf(bad, sizeof bad, "%s.bad", path);
        if (access(bad, F_OK) && errno == ENOENT) {
            if (rename(path, bad))
                break;
            fprintf(stderr, "ct_sdl: save: WARNING: %s is not an %u-byte save file; kept it "
                            "as %s and started without a save\n", path, CT_SRAM_SIZE, bad);
            return;
        }
    }
    fprintf(stderr, "ct_sdl: save: WARNING: %s is not an %u-byte save file and cannot be "
                    "moved aside (%s): saving is off\n", path, CT_SRAM_SIZE, strerror(errno));
    path[0] = 0;
}

int saves_open(const char *file, const char *game_id)
{
    if (file) {
        snprintf(path, sizeof path, "%s", file);
    } else {
        char *dir = SDL_GetPrefPath("ct-recomp", game_id);
        if (!dir) {
            fprintf(stderr, "ct_sdl: save: no preferences folder (%s): saving is off\n",
                    SDL_GetError());
            return 1;
        }
        snprintf(path, sizeof path, "%s%s.srm", dir, game_id);
        SDL_free(dir);
    }
    uint8_t *sram = bus_sram();
    memcpy(on_disk, sram, CT_SRAM_SIZE);   /* no file: the power-on SRAM counts as saved */
    FILE *f = fopen(path, "rb");
    if (!f && errno != ENOENT) {
        fprintf(stderr, "ct_sdl: save: cannot read %s (%s): saving is off\n", path,
                strerror(errno));
        path[0] = 0;
        return 1;
    }
    if (f) {
        size_t n = fread(on_disk, 1, CT_SRAM_SIZE, f);
        int extra = fgetc(f), err = ferror(f);
        fclose(f);
        if (err) {
            fprintf(stderr, "ct_sdl: save: read error on %s: saving is off\n", path);
            path[0] = 0;
            return 1;
        }
        if (n != CT_SRAM_SIZE || extra != EOF) {
            memcpy(on_disk, sram, CT_SRAM_SIZE);
            set_aside();
            if (!path[0])
                return 1;
        } else {
            memcpy(sram, on_disk, CT_SRAM_SIZE);
            fprintf(stderr, "ct_sdl: save: loaded %s\n", path);
#ifdef CT_PS5
            ps5_notify("CT5: save file loaded (%s)", path);   /* port: visible on the TV */
#endif
        }
    } else {
        fprintf(stderr, "ct_sdl: save: %s (new)\n", path);
#ifdef CT_PS5
        ps5_notify("CT5: no save file yet (%s)", path);
#endif
    }
    memcpy(prev, sram, CT_SRAM_SIZE);
    hook_exit();
    return 0;
}

static int fsync_dir(const char *file)
{
    char dir[PATHLEN];
    snprintf(dir, sizeof dir, "%s", file);
    char *slash = strrchr(dir, '/');
    if (slash)
        slash[slash == dir] = 0;   /* "/x" -> "/" */
    else
        snprintf(dir, sizeof dir, ".");
    int fd = open(dir, O_RDONLY);
    if (fd < 0)
        return -1;
    int r = fsync(fd);
    close(fd);
    return r;
}

/* FILE.tmp, fsync, FILE -> FILE.bak (a hard link, else a copy), FILE.tmp
   -> FILE. 0 on success. port: any file (export), and the .bak optional. */
static int write_atomic_to(const char *target, const uint8_t *data, int keep_bak)
{
    char tmp[PATHLEN + 8], bak[PATHLEN + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", target);
    snprintf(bak, sizeof bak, "%s.bak", target);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "ct_sdl: save: cannot create %s (%s)\n", tmp, strerror(errno));
        return -1;
    }
    size_t done = 0;
    while (done < CT_SRAM_SIZE) {
        ssize_t w = write(fd, data + done, CT_SRAM_SIZE - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    /* port: a file system that has no fsync for us (the PS5's, for an
       installed title, seen as saves lost there) is not a failed write:
       only a real I/O error is. */
    int synced = done == CT_SRAM_SIZE ? fsync(fd) : -1;
    if (synced && done == CT_SRAM_SIZE &&
        (errno == EINVAL || errno == ENOTSUP || errno == ENOSYS || errno == EPERM ||
         errno == EACCES || errno == EBADF)) {
        static int said;
        if (!said++)
            fprintf(stderr, "ct_sdl: save: fsync not available here (%s): writing without it\n",
                    strerror(errno));
        synced = 0;
    }
    if (done != CT_SRAM_SIZE || synced) {
        fprintf(stderr, "ct_sdl: save: writing %s failed (%s)\n", tmp,
                done != CT_SRAM_SIZE ? "short write" : strerror(errno));
        close(fd);
        remove(tmp);
        return -1;
    }
    if (close(fd)) {
        remove(tmp);
        return -1;
    }
    if (keep_bak && !access(target, F_OK)) {
        remove(bak);
        if (link(target, bak)) {
            /* No hard links here: copy it. */
            FILE *in = fopen(target, "rb"), *out = in ? fopen(bak, "wb") : NULL;
            uint8_t buf[CT_SRAM_SIZE];
            size_t n = in ? fread(buf, 1, sizeof buf, in) : 0;
            if (out)
                fwrite(buf, 1, n, out);
            if (out)
                fclose(out);
            if (in)
                fclose(in);
        }
    }
    if (rename(tmp, target)) {
        int e = errno;
        /* port: some file systems won't rename over a file: move it aside first */
        if (!access(target, F_OK) && !remove(target) && !rename(tmp, target)) {
            fsync_dir(target);
            return 0;
        }
        fprintf(stderr, "ct_sdl: save: cannot rename %s to %s (%s)\n", tmp, target, strerror(e));
        remove(tmp);
        return -1;
    }
    fsync_dir(target);
    return 0;
}

static int write_atomic(const uint8_t *data)
{
    return write_atomic_to(path, data, 1);
}

static int save_now(const uint8_t *sram)
{
    if (write_atomic(sram)) {
        fprintf(stderr, "ct_sdl: save: cannot write %s (%s)\n", path, strerror(errno));
#ifdef CT_PS5
        static int told;   /* port: no terminal on a console: say it once on screen */
        if (!told++)
            ps5_notify("CT5: your save could not be written (%s). See ct-recomp.log.",
                       strerror(errno));
#endif
        return 1;   /* on_disk unchanged: retried after the interval */
    }
    memcpy(on_disk, sram, CT_SRAM_SIZE);
#ifdef CT_PS5
    if (cur_frame >= 0)
        ps5_notify("CT5: save written to the PS5 (%s)", path);
#endif
    if (cur_frame >= 0)
        fprintf(stderr, "ct_sdl: save: wrote %s (after frame %ld)\n", path, cur_frame);
    else
        fprintf(stderr, "ct_sdl: save: wrote %s (at exit)\n", path);
    return 0;
}

#define MAX_POKES 16
static struct { long frame; unsigned addr, val; } pokes[MAX_POKES];
static int n_pokes;

int saves_poke(const char *spec)
{
    long f;
    unsigned a, v;
    char end;
    if (n_pokes == MAX_POKES || sscanf(spec, "%ld:%x=%x%c", &f, &a, &v, &end) != 3 ||
        a >= CT_SRAM_SIZE || v > 0xFF)
        return -1;
    pokes[n_pokes].frame = f;
    pokes[n_pokes].addr = a;
    pokes[n_pokes].val = v;
    n_pokes++;
    return 0;
}

void saves_frame(long f)
{
    for (int k = 0; k < n_pokes; k++)
        if (pokes[k].frame == f)
            bus_sram()[pokes[k].addr] = (uint8_t)pokes[k].val;
    if (!path[0])
        return;
    const uint8_t *sram = bus_sram();
    int stable = !memcmp(sram, prev, CT_SRAM_SIZE);
    if (!stable)
        memcpy(prev, sram, CT_SRAM_SIZE);
    if (!memcmp(sram, on_disk, CT_SRAM_SIZE)) {
        dirty = 0;
        return;
    }
    Uint32 now = SDL_GetTicks();
    if (!dirty) {
        dirty = 1;
        dirty_since = now;
    }
    /* Not in the middle of the game's own write, unless it never settles. */
    if (!stable && now - dirty_since < 2 * SAVES_INTERVAL_MS)
        return;
    if (have_write && now - last_write < SAVES_INTERVAL_MS)
        return;
    last_write = now;
    have_write = 1;
    cur_frame = f;
    if (!save_now(sram))
        dirty = 0;
    cur_frame = -1;
}

void saves_flush(void)
{
    if (path[0] && memcmp(bus_sram(), on_disk, CT_SRAM_SIZE))
        save_now(bus_sram());
}

void saves_dump_at_exit(const char *file)
{
    dump_path = file;
    hook_exit();
}

static void at_exit(void)
{
    saves_flush();
    if (!dump_path)
        return;
    FILE *f = fopen(dump_path, "wb");
    int ok = f && fwrite(bus_sram(), 1, CT_SRAM_SIZE, f) == CT_SRAM_SIZE;
    if (f && fclose(f))
        ok = 0;
    fprintf(stderr, "ct_sdl: SRAM %s %s\n", ok ? "dumped to" : "cannot be dumped to", dump_path);
}

/* ---- port: save states and import/export (saves.h) ---- */

void saves_after_state_load(void)
{
    if (!path[0])
        return;   /* no save file: the state's save RAM stays */
    memcpy(bus_sram(), on_disk, CT_SRAM_SIZE);
    memcpy(prev, on_disk, CT_SRAM_SIZE);
    dirty = 0;
}

/* The save file's path, as saves_open finds it. 0, or -1 (printed). */
static int find_path(const char *file, const char *game_id)
{
    if (file) {
        snprintf(path, sizeof path, "%s", file);
        return 0;
    }
    char *dir = SDL_GetPrefPath("ct-recomp", game_id);
    if (!dir) {
        fprintf(stderr, "ct_sdl: save: no preferences folder (%s)\n", SDL_GetError());
        return -1;
    }
    snprintf(path, sizeof path, "%s%s.srm", dir, game_id);
    SDL_free(dir);
    return 0;
}

/* Exactly CT_SRAM_SIZE bytes of file into buf. 0, or -1 (printed). */
static int read_srm(const char *file, uint8_t *buf)
{
    FILE *f = fopen(file, "rb");
    if (!f) {
        fprintf(stderr, "ct_sdl: cannot read %s (%s)\n", file, strerror(errno));
        return -1;
    }
    size_t n = fread(buf, 1, CT_SRAM_SIZE, f);
    int extra = fgetc(f), err = ferror(f);
    fclose(f);
    if (err || n != CT_SRAM_SIZE || extra != EOF) {
        fprintf(stderr, "ct_sdl: %s is not a %u-byte (%u KiB) save RAM file\n", file,
                CT_SRAM_SIZE, CT_SRAM_SIZE / 1024);
        return -1;
    }
    return 0;
}

int saves_import(const char *from, const char *file, const char *game_id)
{
    uint8_t data[CT_SRAM_SIZE], old[CT_SRAM_SIZE];
    if (read_srm(from, data) || find_path(file, game_id))
        return 1;
    if (!access(path, F_OK)) {   /* the save it replaces, kept under a new name */
        if (read_srm(path, old))
            return 1;
        char keep[PATHLEN + 64];
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        size_t n = (size_t)snprintf(keep, sizeof keep, "%s.before-import-", path);
        strftime(keep + n, sizeof keep - n, "%Y%m%d-%H%M%S", &tm);
        char saved[PATHLEN];
        snprintf(saved, sizeof saved, "%s", path);
        if (write_atomic_to(keep, old, 0)) {
            fprintf(stderr, "ct_sdl: cannot back up %s to %s (%s): nothing imported\n", saved,
                    keep, strerror(errno));
            return 1;
        }
        fprintf(stderr, "ct_sdl: the old save is kept as %s\n", keep);
    }
    if (write_atomic(data)) {
        fprintf(stderr, "ct_sdl: cannot write %s (%s)\n", path, strerror(errno));
        return 1;
    }
    fprintf(stderr, "ct_sdl: imported %s as %s\n", from, path);
    path[0] = 0;
    return 0;
}

int saves_export(const char *to, const char *file, const char *game_id)
{
    uint8_t data[CT_SRAM_SIZE];
    if (find_path(file, game_id))
        return 1;
    char from[PATHLEN];
    snprintf(from, sizeof from, "%s", path);
    path[0] = 0;
    if (read_srm(from, data))
        return 1;
    if (write_atomic_to(to, data, 0)) {
        fprintf(stderr, "ct_sdl: cannot write %s (%s)\n", to, strerror(errno));
        return 1;
    }
    fprintf(stderr, "ct_sdl: exported %s to %s\n", from, to);
    return 0;
}
