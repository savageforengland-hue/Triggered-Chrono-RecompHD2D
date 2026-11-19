/* SPDX-FileCopyrightText: 2026 ct-recomp contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port: PlayStation 5 homebrew (ps5.h, docs/PS5.md).
 */
#ifdef CT_PS5

#include "ps5.h"

#include <SDL.h>
#undef SDL_GetPrefPath   /* below: SDL's own, to find what to bring over */
extern DECLSPEC char *SDLCALL SDL_GetPrefPath(const char *org, const char *app);

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* libkernel and libSceSystemService (linked through SDL2). */
typedef struct {
    char reserved[45];
    char message[3075];
} ps5_notify_request;
int sceKernelSendNotificationRequest(int device, ps5_notify_request *req, size_t size,
                                     int blocking);
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);

/* The program's own folder (a homebrew launcher's app folder) first, then
   USB: a drive the player plugged in wins over an older copy in /data. */
static char self_dir[256];
static const char *dirs[] = {
    self_dir,
    "/app0",   /* an installed title's own folder (eboot.bin), read-only */
    "/mnt/usb0/ct-recomp",
    "/mnt/usb1/ct-recomp",
    "/data/ct-recomp",
};
#define N_DIRS (sizeof dirs / sizeof *dirs)

static int is_dir(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static int is_file(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISREG(st.st_mode);
}

void ps5_notify(const char *fmt, ...)
{
    ps5_notify_request req;
    memset(&req, 0, sizeof req);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, ap);
    va_end(ap);
    fprintf(stderr, "ct_ps5: notify: %s\n", req.message);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static char log_path[256];

/* The line of the log that says why the program stopped: the frontend's
   "exit:" line, else the last fatal "ct:" one, else the last line. */
static void why_stopped(char *out, size_t n)
{
    out[0] = 0;
    FILE *f = log_path[0] ? fopen(log_path, "rb") : NULL;
    if (!f)
        return;
    /* The last three lines that name a failure, oldest first: the cause
       is usually just before the "exit:" line. */
    char line[512], hit[3][512] = {"", "", ""}, last[512] = "";
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0])
            continue;
        snprintf(last, sizeof last, "%s", line);
        if (!strncmp(line, "ct: ", 4) || strstr(line, "exit: ") || strstr(line, "failed") ||
            strstr(line, "PS5: ") || strstr(line, ": window") || strstr(line, "SDL_Init")) {
            memmove(hit[0], hit[1], sizeof hit[0] * 2);
            snprintf(hit[2], sizeof hit[2], "%s", line);
        }
    }
    fclose(f);
    if (!hit[2][0]) {
        snprintf(out, n, "%s", last);
        return;
    }
    snprintf(out, n, "%s%s%s%s%s", hit[0], hit[0][0] ? " | " : "", hit[1],
             hit[1][0] ? " | " : "", hit[2]);
}

/* Runs last at exit (registered first): tell the player why, then leave
   the way a title must. A title's exit() ends in a system call the
   console refuses (SIGSYS, seen on 13.20). */
static void ps5_at_exit(void)
{
    fflush(stdout);
    fflush(stderr);
    char why[1200];
    why_stopped(why, sizeof why);
    ps5_notify("CT Recomp stopped: %s", why[0] ? why : "(no log)");
#ifdef CT_PS5_TITLE
    sceSystemServiceLoadExec("exit", NULL);
    for (;;)
        sleep(1);
#endif
}

void ps5_init(const char *argv0)
{
    atexit(ps5_at_exit);
    const char *slash = argv0 ? strrchr(argv0, '/') : NULL;
    if (slash && argv0[0] == '/' && (size_t)(slash - argv0) < sizeof self_dir)
        snprintf(self_dir, sizeof self_dir, "%.*s", (int)(slash - argv0), argv0);
    else if (!getcwd(self_dir, sizeof self_dir) || !strcmp(self_dir, "/"))
        self_dir[0] = 0;   /* no usable folder: is_dir("") fails */
    mkdir("/data/ct-recomp", 0777);   /* so the log always has a home */
    /* The first folder the log can be written in (/app0 can't). */
    char path[256];
    int logged = 0;
    for (size_t k = 0; k < N_DIRS && !logged; k++) {
        if (!is_dir(dirs[k]))
            continue;
        snprintf(path, sizeof path, "%s/ct-recomp.log", dirs[k]);
        FILE *probe = fopen(path, "w");
        if (!probe)
            continue;
        fclose(probe);
        chmod(path, 0666);   /* FTP servers can read it */
        logged = 1;
    }
    if (logged)
        snprintf(log_path, sizeof log_path, "%s", path);
    if (logged && freopen(path, "w", stderr))
        setvbuf(stderr, NULL, _IOLBF, 0);   /* line by line: survives a crash */
    if (logged && freopen(path, "a", stdout))
        setvbuf(stdout, NULL, _IOLBF, 0);
    fprintf(stderr, "ct_ps5: log %s (program folder \"%s\")\n", logged ? path : "(none)",
            self_dir);
    sceSystemServiceHideSplashScreen();
}

/* Copy the regular files of from/ into to/ that to/ doesn't have yet, and
   one level of folders (states/). */
static void copy_missing(const char *from, const char *to, int depth)
{
    DIR *d = opendir(from);
    if (!d)
        return;
    mkdir(to, 0777);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char src[1024], dst[1024];
        snprintf(src, sizeof src, "%s/%s", from, e->d_name);
        snprintf(dst, sizeof dst, "%s/%s", to, e->d_name);
        if (is_dir(src)) {
            if (depth > 0)
                copy_missing(src, dst, depth - 1);
            continue;
        }
        if (!is_file(src) || is_file(dst))
            continue;
        FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
        char buf[65536];
        size_t n;
        while (out && (n = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, n, out);
        if (out)
            fclose(out);
        if (in)
            fclose(in);
        fprintf(stderr, "ct_ps5: brought %s over to %s\n", src, dst);
    }
    closedir(d);
}

/* SDL_GetPrefPath on the PS5 (CMake renames every call to this):
   /data/<org>/<app>/, the folder the log is written in, so known to be
   writable by an installed title and reachable over FTP. SDL's own PS5
   folder (/user/home/<user>/...) kept saves from coming back on 13.20.
   What is there already is copied over once. */
char *ps5_pref_path(const char *org, const char *app)
{
    if (!app) {
        SDL_InvalidParamError("app");
        return NULL;
    }
    char path[512];
    snprintf(path, sizeof path, "/data/%s%s%s/", org && *org ? org : "",
             org && *org ? "/" : "", app);
    int existed = is_dir(path);
    for (char *p = path + 1; *p; p++)   /* every folder on the way */
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0777);
            *p = '/';
        }
    if (!existed) {   /* first use: bring over SDL's folder */
        char *old = SDL_GetPrefPath(org, app);
        if (old) {
            size_t n = strlen(old);
            if (n && old[n - 1] == '/')
                old[n - 1] = 0;
            char to[512];
            snprintf(to, sizeof to, "%.*s", (int)strlen(path) - 1, path);
            copy_missing(old, to, 1);
            SDL_free(old);
        }
    }
    char *out = SDL_strdup(path);
    if (!out)
        SDL_OutOfMemory();
    return out;
}

/* rom.sfc in dir, else the first *.sfc / *.smc there: 1 with its path in
   out, or 0. */
static int find_rom_in(const char *dir, char *out, size_t n)
{
    if (!*dir)
        return 0;
    snprintf(out, n, "%s/rom.sfc", dir);
    if (is_file(out))
        return 1;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && (!strcasecmp(dot, ".sfc") || !strcasecmp(dot, ".smc"))) {
            snprintf(out, n, "%s/%s", dir, e->d_name);
            found = is_file(out);
        }
    }
    closedir(d);
    return found;
}

int ps5_find_files(const char **rom, const char **module, int want_module)
{
    static char rom_buf[512], mod_buf[512];
    const char *rom_dir = NULL;
    if (!*rom) {
        for (size_t k = 0; k < N_DIRS && !*rom; k++)
            if (find_rom_in(dirs[k], rom_buf, sizeof rom_buf)) {
                *rom = rom_buf;
                rom_dir = dirs[k];
            }
        if (!*rom) {
            ps5_notify("ct-recomp: no ROM found.\nCopy your own headerless dump as "
                       "rom.sfc into the app's folder or /data/ct-recomp.");
            return 1;
        }
    }
    fprintf(stderr, "ct_ps5: ROM %s\n", *rom);
    if (want_module && module && !*module) {
        /* Beside the ROM first, then every folder. */
        if (rom_dir) {
            snprintf(mod_buf, sizeof mod_buf, "%s/ct_game.so", rom_dir);
            if (is_file(mod_buf))
                *module = mod_buf;
        }
        for (size_t k = 0; k < N_DIRS && !*module; k++) {
            snprintf(mod_buf, sizeof mod_buf, "%s/ct_game.so", dirs[k]);
            if (is_file(mod_buf))
                *module = mod_buf;
        }
        if (*module)
            fprintf(stderr, "ct_ps5: game module %s\n", *module);
        else
            fprintf(stderr, "ct_ps5: no ct_game.so: running in the interpreter only\n");
    }
    return 0;
}

#else
typedef int ct_ps5_unused;   /* ISO C: no empty translation unit */
#endif /* CT_PS5 */

/* port: the title's own (title.c) replaces this; elsewhere nothing to say */
__attribute__((weak)) void ps5_dir_status(int out[3])
{
    out[0] = out[1] = out[2] = 0;
}

__attribute__((weak)) const unsigned char *ps5_dir_peek(void)
{
    static const unsigned char none[48];
    return none;
}
