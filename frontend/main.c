/* SDL2 frontend: the game in $CT_ROM from reset, in the system-mode
 * interpreter under the frame scheduler, in a window titled from the ROM
 * header.
 *
 * usage: ct_sdl [--scale N] [--frames N] [--dump DIR] [--needed-hw FILE] [--fast]
 *               [--require-render] [--log-input] [--interp-only]
 *               [--record FILE] [--script FILE] [--log-audio] [--music-log] [--hd-font FILE]
 *               [--require-no-underrun]
 *
 * 256x224, integer scaled (--scale, default 3); paced to 60.0988 Hz
 * (NTSC) unless --fast; 48 kHz stereo audio (below); keyboard (arrows, Z=B X=A
 * A=Y S=X Q=L W=R, Enter=Start, Right Shift=Select) and the first game
 * controller on pad 1. Ctrl+Q or closing the window quits (not Esc: Steam
 * Input's desktop layout sends Esc for controller B). While a controller
 * is active, key presses within 2 frames of a controller button press are
 * ignored (key_filter in input.h). --frames, --dump
 * and --needed-hw work as in ct_boot.
 *
 * --record FILE writes pad 1's buttons per frame as an input script
 * (replay.h), on every exit including Ctrl+Q and fatal errors: play to any
 * point, then `ct_boot --script FILE --frames N` reproduces the run.
 * --script FILE plays a script's buttons, combined with live input.
 * Input is applied at the frame edge, as in ct_boot.
 *
 * Audio: the DSP's 32 kHz output, band-limited resampling to 48 kHz
 * (resample.h), queued to SDL. Playback starts once AUDIO_TARGET samples
 * are queued; after that the resampling rate moves within +-0.5% to hold
 * the queue there (dynamic rate control), so emulation pacing and the
 * sound card's clock can't drift apart. An empty queue at a frame is an
 * underrun; each exit prints the count. --log-audio prints the queue
 * level and rate factor every frame; --require-no-underrun exits 1 if any
 * happened. --music-log prints the songs the game asks for (port: audio
 * packs, music.h), with the pack file that replaces each. --hd-font FILE
 * draws the text boxes with that TrueType / OpenType font (port: HD text,
 * hdtext.h; settings.ini hd_font=, a bare name in the fonts/ folder of the
 * data folder; it needs the GL renderer and switches to it, as packs do).
 *
 * Every exit prints its reason ("ct_sdl: exit: ..."); startup prints each
 * joystick and the button mapping. --log-input prints every key and
 * controller button event (port: and, with frontend/hid_deck.c reading
 * InputPlumber's Deck pad, each of its button changes, "hid_deck: ...").
 *
 * port additions (GPL-3.0-or-later):
 *   --hd2d [--preset off|subtle|hd2d|deck|hd2d3d|deck3d] [--aspect 4:3|pixel|stretch]
 *          [--debug-view N] [--fullscreen] [--log-gpu]
 *          [--render-scale N] [--sharpen X] [--ao X]
 *       draw through the HD-2D renderer (frontend/hd2d) in an OpenGL 3.3
 *       (or ES 3.0) window. F1 cycles the preset, F2 the debug view
 *       (layer classes, depth, blur), F3 the aspect, F11 fullscreen; on a
 *       controller, R3 cycles the preset and L3 the aspect. Preset and
 *       aspect are saved (SDL pref path ct-recomp/hd2d/settings.ini).
 *       The 3D diorama's quality (hd2d.h, docs/HD2D.md): --render-scale
 *       50-100 (percent of the picture; FSR 1 upscales), --sharpen 0-1
 *       (CAS, or RCAS below 100%), --ao 0-1 (ambient occlusion); each
 *       "auto" for the preset's. settings.ini render_scale, sharpen, ao.
 *   --shot F:PATH (repeatable) [--shot-size WxH]
 *       at the end of frame F, render the HD-2D picture offscreen at
 *       WxH (default 1920x1080) and write it to PATH as PNG; with
 *       --hd2d unset, writes the plain 256x224 frame.
 *   --module FILE
 *       take the game code from a game module (runtime/module.h): the
 *       engine-only build (ct_sdl_engine) contains none of its own.
 *   --rom FILE
 *       the ROM to run (instead of $CT_ROM).
 *   engine-only build, without --module: the first-run flow (firstrun.h)
 *       finds the ROM (--rom, $CT_ROM, the last one used, or a file dropped
 *       on the window), builds the game module from it if needed, with a
 *       progress screen, and loads it. --interp-only skips the module.
 *       --data-dir DIR (default: the source tree) holds game/ and tools/;
 *       --game ID picks DIR/game/ID (default: the game it was configured
 *       with). --shot status:PATH writes the setup screen each time it
 *       changes (a %d in PATH numbers the shots). --build-only stops once
 *       the module is built and loads, without a window (the launcher's
 *       --build-module).
 *   --touch [--touch-hide SECS] [--touch-test FILE]
 *       the on-screen touch controller (touch.h): shown on touchscreens
 *       once touched (from the start on Android); --touch shows it from
 *       the start and lets the left mouse button act as one finger. It
 *       hides when a controller is used and after SECS (default 15, 0:
 *       never) without a touch. --touch-test FILE injects scripted
 *       touches (format in touch.h). While it is shown, plain-path --shot
 *       PNGs are --shot-size with the overlay, as HD-2D ones are.
 *   --save FILE | --no-save, --dump-sram FILE, --poke-sram F:ADDR=VAL
 *       the save RAM file (frontend/saves.h; default GAME_ID.srm in the
 *       per-game preferences folder), or none. For testing, --dump-sram
 *       writes the save RAM as it is at exit, and --poke-sram changes a
 *       byte of it after frame F.
 *   handhelds (docs/HANDHELD.md, device.h): under Steam's Game Mode
 *       (gamescope) the window is fullscreen, the mouse cursor hidden and
 *       HD-2D on unless --no-hd2d; on a Steam Deck with no saved look the
 *       "deck" preset is the default. Holding View+Menu (Back+Start) for
 *       2 s asks whether to quit (A quits, B goes back).
 *   controls (docs/HANDHELD.md): SDL's own Steam Deck / Steam Controller
 *       drivers when Steam Input isn't in between (input_sdl_hints); the
 *       back buttons and grips, and F6 F9 F8 F10 Tab, emit frontend
 *       actions (actions.h, bindings.h; settings.ini bind_*, key_*);
 *       messages, in both pictures, show the buttons of the device last
 *       used (glyphs.h, osd_text.h); settings.ini gyro=on aims the 3D
 *       diorama's camera with pad 1's gyro.
 *   quality of life (frontend/qol.h, docs/USER-GUIDE.md): save states
 *       (quick and 4 slots), resume where you left off (on quit, SIGTERM
 *       and sleep), fast-forward, turbo, auto-pause, battery mode,
 *       screenshots, always run, the plain picture's integer scale and
 *       CRT look; --import-srm / --export-srm. qol_usage lists the keys.
 *   --coop | --no-coop
 *       two players (docs/USER-GUIDE.md, co-op): a second controller plays
 *       SNES pad 2, and the game module's co-op patches share the game
 *       with it (sched_set_players). Default: on whenever two controllers
 *       are connected; F4 turns it on or off; saved (settings.ini
 *       coop=auto|on|off). Pad 1 is the keyboard plus the first
 *       controller, pad 2 the second, pad 3 a third. settings.ini
 *       coop_chars=2,3 lists the party slots player 2 has (default 2 and
 *       3), coop_chars3= player 3's (none by default; a third player only
 *       plays with one). A controller that goes hands its characters back
 *       to player 1 at once. --record writes pads 2 and 3 as "p2"/"p3"
 *       lines and --script plays them (with --coop, a script's pad 2 is a
 *       second player without a controller).
 *   --coop-split
 *       experimental: when player 2's party member is about 3/4 of a
 *       screen from the leader, the window shows a second view centred on
 *       it (side by side on a wide window, else one above the other), and
 *       one view again once they are close. Plain (not HD-2D) picture
 *       only. The game module draws it (docs/USER-GUIDE.md, co-op).
 *   --pack NAME (repeatable) | --pack none, --pack-dump NAME
 *       asset packs (docs/ASSET-PACKS.md, frontend/packs): load replacement
 *       art from <pref path>/packs/NAME (stacked, the first wins; saved as
 *       packs= in settings.ini; F5 reloads), or dump every new tile and
 *       sprite drawn into a pack. A pack needs the GL renderer: without
 *       --hd2d it is used with the off preset.
 *   --pack-export-sheets NAME [--pack-export-scale N]
 *       write every pose (whole sprite frame) seen while playing to sprite
 *       sheets in <pref path>/sheets/NAME, one per character, at 1x and Nx
 *       (4), with an index, for tools/pack_import_sheet.py; and each map's
 *       cells (pack_scene.h).
 *   --mute | --no-mute
 *       silence the game for this run only (the volume setting is kept).
 *   --rumble off|light|medium|strong, --log-rumble
 *       simulated rumble (rumble_out.h, docs/USER-GUIDE.md): controller
 *       pulses made from the game's events (a party member hit, an enemy
 *       down, an earthquake: the game module's) and from the screen
 *       shaking or flashing. settings.ini rumble= (default light);
 *       rumble_sounds=on adds low booms in the sound. --log-rumble prints
 *       every event and motor change; --rumble-test F plays the test pulse
 *       (the menu's Test rumble) at frame F.
 *   --pad-test
 *       the controller test (padtest.h, docs/HANDHELD.md): a checklist of
 *       every control in the corner, ticked as each is used; quits once all
 *       work. Muted unless --no-mute.
 *   --pacing auto|timer|vsync [--log-pacing]
 *       how frames are paced: a sleep to each 60.0988 Hz frame time
 *       (timer; auto, the default, is the timer for now), or the
 *       display's vertical sync (vsync, for 60 Hz displays: the audio
 *       rate control absorbs the 0.16% difference, and while the sound
 *       queue runs low vsync is off until it refills). A vsync that
 *       doesn't give ~60 Hz falls back to the timer. --log-pacing prints frame-time statistics every 60 frames;
 *       every exit prints them for the whole run.
 *   the menu (menu.h, settings.h; docs/USER-GUIDE.md, "The menu"): F10,
 *       R5 or L3 + R3 open it; the game waits (no frame runs, no sound)
 *       until it closes. Its rows come from the settings registry, which
 *       also reads and writes most of settings.ini; every change is saved at
 *       once. settings.ini volume=0-100, mute_unfocused=on, renderer=
 *       plain|hd2d (the menu's choice when neither --hd2d nor --no-hd2d is
 *       given), cam_tilt, cam_zoom, pacing, menu_scale. The mouse cursor
 *       hides after 2 s without moving.
 *   --menu-test FILE, --hash-log FILE
 *       for tests: scripted menu use (menu_test_load), and per frame a hash
 *       of WRAM and the picture. */
#include <math.h>     /* port: fabsf (the 3D quality settings) */
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <time.h>
#include <dirent.h>     /* port: the menu lists the asset packs */
#include <sys/stat.h>
#endif

#include <SDL.h>

#include "actions.h"  /* port: back buttons, quick save / load, fast-forward */
#include "bindings.h"
#include "bus.h"
#include "device.h"   /* port: Steam Game Mode, Steam Deck */
#include "glyphs.h"   /* port: button glyphs in messages */
#include "hid_deck.h" /* port: Deck-format hidraw pads SDL misses (InputPlumber) */
#include "osd_text.h"
#include "module.h"   /* port: --module */
#ifdef CT_FIRSTRUN
#include "firstrun.h" /* port: engine-only first run */
#endif
#include "hd2d/hd2d.h"
#include "hwlog.h"
#include "input.h"
#include "interp.h"
#include "packs.h"    /* port: asset packs (docs/ASSET-PACKS.md) */
#include "music.h"    /* port: audio packs: [[music]] in the same packs */
#include "hdtext.h"
#include "pack_scene.h"   /* port: their models, poses and sheets */
#include "png.h"
#include "replay.h"
#include "resample.h"
#include "saves.h"    /* port: save RAM on disk */
#include "sched.h"
#include "touch.h"      /* port: on-screen touch controller */
#include "hd2d/osd_font.h" /* port: the co-op turn tag */
#include "touch_gl.h"
#include "qol.h"        /* port: quality of life (save states, fast-forward, ...) */
#include "menu.h"       /* port: the in-game settings menu */
#include "settings.h"   /* port: the settings registry the menu shows */
#include "padtest.h"    /* port: the controller test (--pad-test) */
#include "rumble.h"     /* port: simulated rumble: the events */
#include "rumble_out.h" /* port: simulated rumble: the controllers */
#include "ps5.h"        /* port: PlayStation 5 homebrew (docs/PS5.md) */

#ifndef CT_VERSION
#define CT_VERSION "dev"
#endif
#define APP_NAME "CT Recomp"

#define RATE 48000
/* Device buffer: 960 samples, exactly 20 ms at 48 kHz. Queue target: two
   device buffers plus one frame (800), ~57 ms. */
#define AUDIO_BUFFER 960
#ifdef CT_PS5
/* port: on the PS5 the queue is kept fuller (~107 ms): a frame that runs
   long at a higher resolution or in the 3D view emptied the shorter one and
   the music stuttered. */
#define AUDIO_TARGET (4 * AUDIO_BUFFER + 1280)
#else
#define AUDIO_TARGET (2 * AUDIO_BUFFER + 800)
#endif
#define AUDIO_MAX_DEV 0.005
#define FRAME_NS 16639267ull   /* 1 / 60.0988 Hz */

static jmp_buf fatal_jmp;
static char fatal_msg[256];

static void on_fatal(const char *msg)
{
    snprintf(fatal_msg, sizeof fatal_msg, "%s", msg);
    longjmp(fatal_jmp, 1);
}

/* Why the process is ending; printed by an atexit handler so no exit path
   goes unexplained. */
static char exit_reason[256] =
    "exit() before the frame loop: see the message above (a ct: line is a fatal error)";
static long exit_frame = -1;
static long audio_underruns, audio_dropped;

/* port: frame pacing (--pacing) and its statistics: the time between
   successive frame edges, after the wait. */
enum { PACE_AUTO, PACE_TIMER, PACE_VSYNC };
static int pace_opt = PACE_AUTO, pace_vsync, log_pacing;
static int pace_catchup;     /* vsync off for now: the sound queue ran low */
static long pace_catchups;   /* times that happened */
static struct {
    uint64_t last;
    long n, late;               /* late: over 25 ms, a missed display refresh at 60 Hz */
    double sum, min, max;       /* ms */
    long wn, wlate;             /* the current 60-frame window */
    double wsum, wmin, wmax;
} pace;

static void pace_print(const char *what, long n, double sum, double mn, double mx, long late)
{
    if (n && sum > 0)
        fprintf(stderr, "ct_sdl: pacing %s (%s): %ld frames, mean %.3f ms (%.3f Hz), min %.3f, "
                        "max %.3f, %ld over 25 ms, %ld vsync catch-ups\n", what,
                pace_vsync ? "vsync" : "timer", n, sum / n, 1000.0 * n / sum, mn, mx, late,
                pace_catchups);
}

static void set_exit_reason(const char *fmt, ...) CT_PRINTF(1, 2);
static void set_exit_reason(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(exit_reason, sizeof exit_reason, fmt, ap);
    va_end(ap);
}

static void report_exit(void)
{
    fprintf(stderr, "ct_sdl: exit: %s (frame %ld)\n", exit_reason, exit_frame);
    fprintf(stderr, "ct_sdl: audio: %ld underruns, %ld frames dropped\n", audio_underruns,
            audio_dropped);
    pace_print("run", pace.n, pace.sum, pace.min, pace.max, pace.late);   /* port */
}

static void print_controllers(void)
{
    int n = SDL_NumJoysticks();
    fprintf(stderr, "ct_sdl: %d joystick(s)\n", n);
    for (int k = 0; k < n; k++) {
        const char *name = SDL_JoystickNameForIndex(k);
        fprintf(stderr, "ct_sdl:   #%d \"%s\" %s\n", k, name ? name : "?",
                SDL_IsGameController(k) ? "(game controller)" : "(joystick only: not used)");
        if (SDL_IsGameController(k)) {
            char *m = SDL_GameControllerMappingForDeviceIndex(k);
            fprintf(stderr, "ct_sdl:     SDL mapping: %s\n", m ? m : "?");
            SDL_free(m);
            /* port: what it has for the frontend's actions and glyphs */
            int type = SDL_GameControllerTypeForIndex(k);
            uint16_t vid = SDL_JoystickGetDeviceVendor(k), pid = SDL_JoystickGetDeviceProduct(k);
            int fam = glyphs_family(type, vid, pid, name, device_kind() == DEVICE_STEAM_DECK);
            fprintf(stderr, "ct_sdl:     %04x:%04x, SDL type %d, glyphs %s\n", vid, pid, type,
                    glyphs_family_name(fam));
        }
    }
    {
        const char *why;
        if (input_under_steam_input(&why))
            fprintf(stderr, "ct_sdl: Steam Input is on (%s): Steam's virtual pad, the back "
                            "buttons as Steam's layout sends them\n", why);
    }
    fprintf(stderr, "ct_sdl: pad 1: keyboard arrows, Z=B X=A A=Y S=X Q=L W=R, Enter=Start, "
                    "Right Shift=Select; controller south=B east=A west=Y north=X, "
                    "shoulders=L/R, Start, Back=Select, d-pad/left stick. Ctrl+Q or closing "
                    "the window quits.\n");
}

static int pad_button(void *ctx, SDL_GameControllerButton b)
{
    return SDL_GameControllerGetButton(ctx, b);
}

static int pad_axis(void *ctx, SDL_GameControllerAxis a)
{
    return SDL_GameControllerGetAxis(ctx, a);
}

static SDL_JoystickID pad_id(SDL_GameController *c)
{
    return c ? SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) : -1;
}

/* The first game controller other than a and b (port: co-op's pads skip
   the ones already open). */
static SDL_GameController *open_controller_except2(SDL_GameController *a, SDL_GameController *b)
{
    for (int k = 0; k < SDL_NumJoysticks(); k++) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(k);
        if (SDL_IsGameController(k) && id != pad_id(a) && id != pad_id(b))
            return SDL_GameControllerOpen(k);
    }
    return NULL;
}

static SDL_GameController *open_controller(void)
{
    return open_controller_except2(NULL, NULL);
}

static replay script;
static long poke_frame = -1;   /* port: --poke-wram F:ADDR^M: XOR a WRAM byte at frame F's edge */
static uint32_t poke_addr;
static uint8_t poke_val;
static long set_from = 1, set_to = 0;   /* port: --set-wram F1-F2:ADDR=V: a WRAM byte held at each edge */
static uint32_t set_addr;
static uint8_t set_val;
static replay_rec rec;

/* Host state shared by main and the frame hook. */
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioDeviceID audio;
static SDL_GameController *pad;
static SDL_GameController *pad2, *pad3;   /* port: co-op, the second and third controllers */
static resampler rs;
static int playing;
static double level_avg = AUDIO_TARGET;
static key_filter keys;
static uint16_t live_buttons;
static uint64_t perf_freq, next_frame;
static int running = 1, fast, log_input, log_audio, log_music;

/* port: asset packs (frontend/packs, docs/ASSET-PACKS.md). They live in
   the per-game data folder, <pref path>/packs/NAME; a NAME with a slash
   in it is a directory of its own. */
static const char *pack_dump;   /* --pack-dump NAME */
static int want_prov(void)
{
    return packs_active() || pack_dump || hdtext_ready();
}
static const char *pack_sheets;   /* --pack-export-sheets NAME */
static int pack_sheets_scale = 4;   /* --pack-export-scale N */
#define MAX_PACK_NAMES 16
static char pack_names[MAX_PACK_NAMES][256];   /* loaded, highest priority first */
static int n_pack_names, packs_given;          /* --pack seen: beats settings.ini */
static int preset_forced = -1;   /* the saved look, while packs force the GL path */
static uint16_t pack_map[SCHED_WIDTH * SCHED_HEIGHT * 4];
static double pack_map_ms;   /* CPU time building the maps, and how many */
static long pack_map_frames, pack_map_pixels;

static void pack_map_report(void)
{
    if (pack_map_frames)
        fprintf(stderr, "ct_sdl: asset packs: %ld frames, %.1f%% of pixels replaced, "
                        "map %.3f ms per frame (CPU)\n", pack_map_frames,
                100.0 * pack_map_pixels / ((double)pack_map_frames * SCHED_WIDTH * SCHED_HEIGHT),
                pack_map_ms / pack_map_frames);
}

/* "a,b": the names, in order. */
static void pack_names_parse(const char *list)
{
    n_pack_names = 0;
    while (*list && n_pack_names < MAX_PACK_NAMES) {
        size_t n = strcspn(list, ",");
        if (n && n < sizeof pack_names[0] && strncmp(list, "none", n))
            snprintf(pack_names[n_pack_names++], sizeof pack_names[0], "%.*s", (int)n, list);
        list += n + (list[n] == ',');
    }
}

/* port: where packs and fonts live: the game's folder in the user data
   directory; on the PS5, /data/ct-recomp/ (copied there over FTP or from a
   USB drive: SDL's PS5 data folder can't be reached that way). PREFIX
   gets a trailing slash. */
static void mods_root(const char *game_id, char *out, size_t n)
{
#ifdef CT_PS5
    (void)game_id;
    mkdir("/data/ct-recomp", 0777);
    snprintf(out, n, "/data/ct-recomp/");
#else
    char *dir = SDL_GetPrefPath("ct-recomp", game_id);
    snprintf(out, n, "%s", dir ? dir : "./");
    SDL_free(dir);
#endif
}

static char *pack_dir(const char *game_id, const char *name)
{
    char *p = malloc(4096);
    if (!p)
        return NULL;
    if (strchr(name, '/') || strchr(name, '\\')) {
        snprintf(p, 4096, "%s", name);
        return p;
    }
    char root[400];
    mods_root(game_id, root, sizeof root);
    snprintf(p, 4096, "%spacks/%s", root, name);
    return p;
}

/* port: HD text (hdtext.h): settings.ini hd_font=PATH (a name without a
   slash: the fonts/ folder in the data folder), --hd-font PATH. The
   picture with the console's glyphs taken out, and the overlay, are made
   once a frame (hdtext_frame_now) and drawn by present_hd2d and the
   screenshots. */
static char hd_font[4096];
static int hd_font_given, hd_text_size = 100, hd_text_style;
static hdtext_result hd_text_now;

static void hd_font_path(const char *game_id, const char *name, char *out, size_t n)
{
    if (strchr(name, '/') || strchr(name, '\\')) {
        snprintf(out, n, "%s", name);
        return;
    }
    char root[400];
    mods_root(game_id, root, sizeof root);
    snprintf(out, n, "%sfonts/%s", root, name);
}

static int hd_font_load(const char *game_id)
{
    char path[4200], err[4400];
    if (!hd_font[0] || !strcmp(hd_font, "none"))
        return hdtext_load_font(NULL, err, sizeof err);
    hd_font_path(game_id, hd_font, path, sizeof path);
    if (hdtext_load_font(path, err, sizeof err)) {
        fprintf(stderr, "ct_sdl: HD text: %s\n", err);
        return -1;
    }
    fprintf(stderr, "ct_sdl: HD text: font %s\n", path);
    return 0;
}

/* Load the named packs (their art only when `art`; their [[music]]
   always: it needs no GL); returns how many art replacements. */
static int packs_setup_ex(const char *game_id, int art)
{
    const char *dirs[MAX_PACK_NAMES] = {0};
    char *owned[MAX_PACK_NAMES];
    int n = 0;
    for (int k = 0; k < n_pack_names; k++)
        if ((owned[n] = pack_dir(game_id, pack_names[k])))
            dirs[n] = owned[n], n++;
    int r = n && art ? packs_load(dirs, n) : 0;
    music_load(dirs, n);   /* port: audio packs */
    for (int k = 0; k < n; k++)
        free(owned[k]);
    return r;
}
static int packs_setup(const char *game_id) { return packs_setup_ex(game_id, 1); }

/* The 3D diorama's tiles -> atlas entries (hd2d_pack_tiles). */
static int pack_tile_lookup(void *user, unsigned chr, int pal, int bpp, const uint16_t *vram,
                            const uint16_t *cgram)
{
    (void)user;
    const packs_rect *r = packs_lookup_tile(packs_key_tile(vram, cgram, chr, pal, bpp));
    int n;
    const packs_rect *e = packs_entries(&n);
    return r ? (int)(r - e) + 1 : 0;
}

/* The loaded packs' atlas to the renderer. */
static void packs_upload(void)
{
    int size, pages, n;
    const uint8_t *atlas = packs_atlas(&size, &pages);
    const packs_rect *e = packs_entries(&n);
    int *rects = n ? malloc((size_t)n * 8 * sizeof *rects) : NULL;
    for (int k = 0; k < n && rects; k++) {
        int v[8] = {e[k].page, e[k].x, e[k].y, e[k].w, e[k].h, e[k].src_w, e[k].src_h, 0};
        memcpy(rects + k * 8, v, sizeof v);
    }
    hd2d_pack_atlas(rects ? atlas : NULL, size, pages, rects, rects ? n : 0);
    hd2d_pack_tiles(rects ? pack_tile_lookup : NULL, NULL);
    free(rects);
    pack_scene_attach();   /* models and poses (pack_scene.h) */
}

static void pack_sheets_report(void)
{
    pack_scene_export_close();
}

#ifdef CT_PS5
/* port: the sheet export from the menu (no command line on the console) */
static int pose_export_on;
static int get_pose_export(void *u) { (void)u; return pose_export_on || pack_sheets != NULL; }
static void set_pose_export(void *u, int v)
{
    (void)u;
    if (v && !pose_export_on && !pack_sheets) {
        pose_export_on = !pack_scene_export_open("/data/ct-recomp/sheets/ct5", 4);
        ps5_notify(pose_export_on ? "Exporting character poses" : "Cannot write the poses");
    } else if (!v && pose_export_on) {
        pack_scene_export_close();
        pose_export_on = 0;
        ps5_notify("Character poses saved");
    }
}
#endif

static void pack_dump_report(void)
{
    long pieces, objects;
    packs_dump_counts(&pieces, &objects);
    fprintf(stderr, "ct_sdl: pack dump: %ld new pieces, %ld new objects\n", pieces, objects);
    packs_dump_close();
}
static const char *dump;
static int no_pad_hint;   /* port: scripted / dumping runs: no "No controller" message */

/* port: HD-2D presentation and screenshots. */
static SDL_Window *win;
static SDL_GLContext glctx;
static int hd2d_mode, log_gpu, gl_es;   /* gl_es: the context is OpenGL ES (port: menu) */
static int game_mode, preset_saved, preset_given;   /* port: device.h defaults */
static quit_combo quitc;                            /* port: View+Menu quits */
/* port: co-op (--coop): COOP_AUTO is on while two controllers are open. */
enum { COOP_AUTO, COOP_ON, COOP_OFF };
static int coop_pref = COOP_AUTO, coop_players = 1;
static uint16_t live_buttons2, live_buttons3;       /* pads 2 and 3, this frame */
/* port: which player (1-3) has party slot 1-3 (settings.ini coop_chars,
   coop_chars3: the slots players 2 and 3 have). */
static uint8_t slot_player[3] = {1, 2, 2};
/* port: the back buttons / grips and their keys (bindings.h, actions.h),
   per pad (pad, pad2, pad3); the glyphs follow the device last used
   (glyphs.h); gyro aiming (settings.ini gyro=on, off by default). */
static binder pad_binder[3];
static long last_pad_press = -1000;   /* frame of the last controller button */
static int gyro_on, steam_hidapi = 1;
static float gyro_strength = 1.0f;
static gyro_cam gyro;
static SDL_GameController *gyro_pad;  /* the pad whose gyro is on */
static long quit_asked = -1;                        /* frame the question went up */
#define MAX_SHOTS 64
static struct { long frame; const char *path; } shots[MAX_SHOTS];
static int n_shots, shot_w = 1920, shot_h = 1080;

/* port: the touch overlay (touch.h); its GL drawer for the HD-2D path. */
static int touch_gl_ok;
static touch_opts touch_o = {0, 15};

/* port: the menu (menu.h) and the small settings beside it: the volume
   (0-100, applied to the samples queued), muting while the window isn't
   focused, the mouse cursor hidden after 2 s without moving. */
static int volume = 100, mute_unfocused, focused = 1;
static int mute_run;   /* port: --mute (and --pad-test): silent this run, nothing saved */
static int renderer_saved = -1;   /* settings.ini renderer=plain|hd2d: the menu's choice, -1 none */
static int menu_ready;        /* menu_init done: ACTION_MENU opens it */
static int menu_hold_mask;    /* after closing: pad 1 kept from the game until let go */
static int frontend_ready;    /* window, audio and scheduler up: settings apply live */
static int cursor_shown = 1;
static uint32_t mouse_moved_at;
#define CURSOR_IDLE_MS 2000
static int stick_chord, stick_chorded;   /* L3 + R3: the menu */
/* port: --hash-log FILE: per frame, a hash of WRAM and the picture (as
   ct_boot's, smaller), so tests can tell the game didn't change. */
static FILE *hash_log;
/* port: --menu-test FILE: scripted menu use (see menu_test_load). */
#define MAX_MENU_TEST 256
struct mt_ev { int kind, a; char path[200]; };
static struct { long open_frame; int n; struct mt_ev ev[MAX_MENU_TEST]; }
    mtest = {-1, 0, {{0, 0, {0}}}};
static long menu_ticks_total, menu_frames_advanced;

/* port: where the picture is: HD-2D's aspect, or the plain integer scale. */
static touch_rect picture_rect(int w, int h)
{
    if (!hd2d_mode) {   /* port: QOL integer scale or fit */
        touch_rect r;
        qol_picture_rect(w, h, &r.x, &r.y, &r.w, &r.h);
        return r;
    }
    int a = hd2d_aspect();
    return touch_picture_aspect(w, h, a == HD2D_ASPECT_PIXEL ? (SCHED_WIDTH * 8.0 / 7.0) / SCHED_HEIGHT
                                      : a == HD2D_ASPECT_STRETCH ? 0.0 : 4.0 / 3.0);
}

/* port: the plain path: the frame integer-scaled and centered, the touch
   overlay over it. No logical size: SDL would clamp touches to it. */
/* port: co-op: "P2" / "P3" over the picture while the game waits on that
   player (sched_turn_player: the battle menu that player has is open),
   by the command windows at the bottom left, in the 5x7 font at 2x. */
static const uint8_t *turn_tag(const uint8_t *fb)
{
    static uint8_t out[SCHED_WIDTH * SCHED_HEIGHT * 4];
    int p = coop_players > 1 ? sched_turn_player() : -1;
    if (p < 1)
        return fb;
    memcpy(out, fb, sizeof out);
    char text[3] = {'P', (char)('1' + p), 0};
    static const uint8_t col[4][3] = {{0, 0, 0}, {80, 200, 255}, {255, 190, 60}, {120, 255, 120}};
    int x0 = 6, y0 = 136;
    for (int y = y0 - 3; y < y0 + 17; y++)   /* a dark box */
        for (int x = x0 - 3; x < x0 + 26; x++) {
            uint8_t *q = out + ((size_t)y * SCHED_WIDTH + x) * 4;
            q[0] = q[0] / 4, q[1] = q[1] / 4, q[2] = q[2] / 4;
        }
    for (int k = 0; text[k]; k++) {
        const uint8_t *rows = osd_glyph(text[k]);
        for (int r = 0; r < 7; r++)
            for (int b = 0; b < 5; b++)
                if (rows[r] & (0x10 >> b))
                    for (int d = 0; d < 4; d++) {
                        int x = x0 + (k * 6 + b) * 2 + (d & 1), y = y0 + r * 2 + (d >> 1);
                        uint8_t *q = out + ((size_t)y * SCHED_WIDTH + x) * 4;
                        q[0] = col[p][2], q[1] = col[p][1], q[2] = col[p][0];
                    }
    }
    return out;
}

/* port: split screen (--coop-split): this frame's second view, if any. */
static int split_on;
static uint8_t view2_px[SCHED_WIDTH * SCHED_HEIGHT * 4];
static SDL_Texture *tex2;

/* port: --menu-test's "shot": the window as presented next, menu and all. */
static const char *menu_shot_path;

static void take_menu_shot(int w, int h)
{
    if (!menu_shot_path)
        return;
    int ok = !menu_shot(menu_shot_path, w, h);
    fprintf(stderr, "ct_sdl: menu: %s %s (%dx%d)\n", ok ? "wrote" : "cannot write", menu_shot_path,
            w, h);
    menu_shot_path = NULL;
}


static void present_plain(const uint8_t *bgrx)
{
    int w, h;
    SDL_GetRendererOutputSize(ren, &w, &h);
    SDL_UpdateTexture(tex, NULL, bgrx, SCHED_WIDTH * 4);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (split_on && tex2) {
        /* port: two views, side by side on a wide window (player 2's on
           the side it is on), else one above the other */
        int side = (long)w * SCHED_HEIGHT >= (long)h * SCHED_WIDTH;
        int hw = side ? w / 2 : w, hh = side ? h : h / 2;
        touch_rect p = touch_picture_aspect(hw, hh, (double)SCHED_WIDTH / SCHED_HEIGHT);
        int second_first = side ? sched_view2()->dx < 0 : sched_view2()->dy < 0;
        SDL_UpdateTexture(tex2, NULL, view2_px, SCHED_WIDTH * 4);
        for (int k = 0; k < 2; k++) {
            int slot = k ^ second_first;
            SDL_Rect dst = {p.x + (side ? slot * hw : 0), p.y + (side ? 0 : slot * hh), p.w, p.h};
            SDL_RenderCopy(ren, k ? tex2 : tex, NULL, &dst);
        }
        if (menu_is_open()) {
            menu_draw(w, h);
            take_menu_shot(w, h);
        }
        SDL_RenderPresent(ren);
        return;
    }
    qol_plain_draw(ren, tex, w, h);   /* port: QOL integer scale or fit, CRT look */
    if (menu_is_open()) {   /* port: the menu over the picture, no overlay or message */
        menu_draw(w, h);
        take_menu_shot(w, h);
    } else {
        touch_draw_sdl(ren);
        /* port: the message, placed and sized as HD-2D's */
        int px, py, pw, ph;
        qol_picture_rect(w, h, &px, &py, &pw, &ph);
        int s = ph / 180 > 2 ? ph / 180 : 2;
        osd_sdl_draw(ren, px + s * 4, py + s * 4, s, 1);
        padtest_draw(w, h);   /* port: the controller test's checklist */
    }
    SDL_RenderPresent(ren);
}

/* port: the scene the 3D diorama look draws (module.h), captured once per
   frame edge and only while the look uses it. */
static const ct_scene *scene_capture_now(void)
{
    static ct_scene sc;
    static long at = -1;
    if (!ct_scene_available())
        return NULL;
    if (sched_frame_count() != at) {
        at = sched_frame_count();
        ct_scene_capture(&sc);
    }
    return &sc;
}

static const ct_scene *scene_now(void)
{
    return hd2d_wants_scene() ? scene_capture_now() : NULL;
}

/* One screenshot of a frame: HD-2D at shot_w x shot_h, else as is. */
static int write_shot(const char *path, const uint8_t *bgrx, const uint8_t *layers,
                      const uint8_t *behind, const uint8_t *behind_layers,
                      const sched_ppu_state *st)
{
    int ok;
    if (hd2d_mode) {
        uint8_t *buf = malloc((size_t)shot_w * shot_h * 4);
        hd2d_set_scene(st ? scene_now() : NULL);   /* none on the setup screens */
        hd2d_render(bgrx, layers, behind, behind_layers, st, shot_w, shot_h, 1);
        ok = buf && !hd2d_read(buf, shot_w, shot_h) &&
             (!touch_gl_ok || !touch_gl_shot(buf, shot_w, shot_h, picture_rect(shot_w, shot_h))) &&
             !png_write_bgrx(path, buf, shot_w, shot_h);
        free(buf);
    } else if (touch_visible()) {   /* port: with the touch overlay */
        uint8_t *buf = malloc((size_t)shot_w * shot_h * 4);
        SDL_UpdateTexture(tex, NULL, bgrx, SCHED_WIDTH * 4);
        ok = buf && !touch_shot_sdl(ren, tex, shot_w, shot_h, buf) &&
             !png_write_bgrx(path, buf, shot_w, shot_h);
        free(buf);
    } else {
        ok = !png_write_bgrx(path, bgrx, SCHED_WIDTH, SCHED_HEIGHT);
    }
    return ok;
}

static void take_shots(long f)
{
    for (int k = 0; k < n_shots; k++) {
        if (shots[k].frame != f)
            continue;
        int ok = write_shot(shots[k].path, hd_text_now.bgrx ? hd_text_now.bgrx : sched_frame(),
                            hd_text_now.layers ? hd_text_now.layers : sched_layers(),
                            sched_behind(), sched_behind_layers(), sched_ppu_state_get());
        fprintf(stderr, "ct_sdl: frame %ld: %s %s\n", f, ok ? "wrote" : "cannot write",
                shots[k].path);
        if (split_on) {   /* port: the second view beside it */
            char p2[512];
            snprintf(p2, sizeof p2, "%s.view2.png", shots[k].path);
            ok = !png_write_bgrx(p2, view2_px, SCHED_WIDTH, SCHED_HEIGHT);
            fprintf(stderr, "ct_sdl: frame %ld: %s %s\n", f, ok ? "wrote" : "cannot write", p2);
        }
    }
}

static void present_hd2d(void)
{
    int w, h;
    SDL_GL_GetDrawableSize(win, &w, &h);
    hd2d_set_scene(scene_now());
    hd2d_render(hd_text_now.bgrx ? hd_text_now.bgrx : sched_frame(),
                hd_text_now.layers ? hd_text_now.layers : sched_layers(), sched_behind(),
                sched_behind_layers(), sched_ppu_state_get(), w, h, 0);
    if (menu_is_open()) {   /* port: the menu, after the picture, before the swap */
        menu_draw(w, h);
        take_menu_shot(w, h);
    } else {
        if (touch_gl_ok)
            touch_gl_draw(w, h);   /* port: touch overlay */
        padtest_draw(w, h);   /* port: the controller test's checklist */
    }
    SDL_GL_SwapWindow(win);
    if (log_gpu && sched_frame_count() % 60 == 0)
        fprintf(stderr, "ct_sdl: frame %ld hd2d %dx%d %.2f ms\n", sched_frame_count(), w, h,
                hd2d_last_ms());
}

static void settings_save(void);

/* port: the window's remembered state (settings.ini: window=WxH,
   position=X,Y, fullscreen=0|1). The windowed size and place are kept
   apart from fullscreen, so leaving fullscreen goes back to them. */
static struct {
    int w, h, x, y, fs;      /* current */
    int have_size, have_pos; /* known: loaded, or seen */
    int w0, h0, x0, y0, fs0; /* as the window was created */
    int scale_given;         /* --scale beats the saved size */
} wst;

static int wst_places_work(void)   /* a driver that reports real window places */
{
    const char *d = SDL_GetCurrentVideoDriver();
    return d && strcmp(d, "wayland") && strcmp(d, "offscreen") && strcmp(d, "dummy") &&
           strcmp(d, "KMSDRM") && strcmp(d, "kmsdrm");
}

/* Take the window's windowed geometry (not while fullscreen: that is the
   display's). */
static void wst_track(void)
{
    if (!win || (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN))
        return;
    SDL_GetWindowSize(win, &wst.w, &wst.h);
    wst.have_size = 1;
    if (wst_places_work()) {
        SDL_GetWindowPosition(win, &wst.x, &wst.y);
        wst.have_pos = 1;
    }
}

static int wst_dirty(void)
{
    int fs = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) ? 1 : 0;
    wst.fs = fs;
    return fs != wst.fs0 || wst.w != wst.w0 || wst.h != wst.h0 || wst.x != wst.x0 ||
           wst.y != wst.y0;
}

/* Is a place on a display (so a monitor that is gone doesn't strand the window)? */
static int wst_on_screen(int x, int y, int w)
{
    for (int k = 0, n = SDL_GetNumVideoDisplays(); k < n; k++) {
        SDL_Rect r;
        if (!SDL_GetDisplayBounds(k, &r) && x + w / 2 >= r.x && x + w / 2 < r.x + r.w &&
            y + 16 >= r.y && y + 16 < r.y + r.h)
            return 1;
    }
    return 0;
}

/* port: a message on screen (HD-2D or the plain picture), [token]s as
   button glyphs of the device in use (glyphs.h). */
static void osd_show(const char *msg)
{
    if (hd2d_mode)
        hd2d_osd(msg);
    else
        osd_sdl_show(msg);
}

/* ... and on stderr, the tokens as their labels. */
static void osd(const char *msg)
{
    char plain[256];
    osd_text_plain(msg, plain, sizeof plain);
    fprintf(stderr, "ct_sdl: %s\n", plain);
    osd_show(msg);
}

/* port: co-op is on: never when turned off; otherwise while a second
   controller is open, or a --script plays pad 2 with co-op turned on.
   When the second controller goes, its characters go back to player 1
   at once (one player: the game's co-op patches stand aside). */
static int coop_on(void)
{
    if (coop_pref == COOP_OFF)
        return 0;
    return pad2 || (coop_pref == COOP_ON && replay_multi(&script));
}

/* port: players on: 1, 2, or 3 with a third controller and a party slot
   given to player 3. */
static int coop_count(void)
{
    if (!coop_on())
        return 1;
    int p3 = 0;
    for (int k = 0; k < 3; k++)
        p3 |= slot_player[k] == 3;
    return pad3 && p3 ? 3 : 2;
}

/* port: tell the game how many players there are, and who has which
   party slot; say so when it changes. */
static void coop_update(void)
{
    static int said;
    int n = coop_count();
    for (int k = 0; k < 3; k++)
        sched_set_slot_player(k, slot_player[k] - 1);
    if (said && n == coop_players)
        return;
    if (said || n > 1) {
        char msg[200];
        const char *name = pad2 ? SDL_GameControllerName(pad2) : "the script";
        if (n > 1)
            snprintf(msg, sizeof msg, "Co-op: on, player 2 is %s%s%s", name ? name : "?",
                     n > 2 ? ", player 3 is " : "",
                     n > 2 && SDL_GameControllerName(pad3) ? SDL_GameControllerName(pad3) : "");
        else
            snprintf(msg, sizeof msg, "Co-op: off");
        osd(msg);
    }
    said = 1;
    coop_players = n;
    sched_set_players(n);
}

/* port: settings.ini coop_chars / coop_chars3: party slots, "2,3". */
static void parse_chars(const char *v, int player)
{
    if (player == 2)
        for (int k = 0; k < 3; k++)
            if (slot_player[k] == 2)
                slot_player[k] = 1;
    for (; *v; v++)
        if (*v >= '1' && *v <= '3')
            slot_player[*v - '1'] = (uint8_t)player;
}

static void print_chars(FILE *f, const char *key, int player)
{
    fprintf(f, "%s=", key);
    const char *sep = "";
    for (int k = 0; k < 3; k++)
        if (slot_player[k] == player) {
            fprintf(f, "%s%d", sep, k + 1);
            sep = ",";
        }
    fputc('\n', f);
}

/* F1-F3, F11: the HD-2D controls. Returns 1 if the key was one of them. */
static int hd2d_key(SDL_Scancode sc)
{
    if (sc == SDL_SCANCODE_F4) {   /* port: co-op on / off */
        coop_pref = coop_on() ? COOP_OFF : COOP_ON;
        coop_update();
        if (coop_pref == COOP_ON && coop_players < 2)
            osd("Co-op: on once a second controller is connected");
        settings_save();
        return 1;
    }
    if (sc == SDL_SCANCODE_F11) {
        int fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
#ifndef CT_PS5   /* port: always fullscreen (see the Fullscreen setting) */
        SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
#endif
        wst.fs = !fs;
        settings_save();
        return 1;
    }
    if (!hd2d_mode)
        return 0;
    if (sc == SDL_SCANCODE_F5 && n_pack_names) {   /* port: reload the packs */
        int n = packs_reload();
        music_reload();
        packs_upload();
        sched_set_prov(want_prov());
        static char msg[48];
        snprintf(msg, sizeof msg, n < 0 ? "Packs: none readable" : "Packs reloaded: %d", n);
        fprintf(stderr, "ct_sdl: %s\n", msg);
        hd2d_osd(msg);
        return 1;
    }
    if (sc == SDL_SCANCODE_F1) {
        preset_forced = -1;   /* port: the user's choice now */
        hd2d_set_preset((hd2d_preset() + 1) % HD2D_PRESETS);
        fprintf(stderr, "ct_sdl: hd2d preset %s\n", hd2d_preset_name(hd2d_preset()));
        static const char *const msg[HD2D_PRESETS] = {"Look: original", "Look: HD-2D subtle",
                                                      "Look: HD-2D", "Look: HD-2D Steam Deck",
                                                      "Look: HD-2D 3D diorama",
                                                      "Look: 3D diorama, Steam Deck"};
        osd_show(msg[hd2d_preset()]);
        settings_save();
    } else if (sc == SDL_SCANCODE_F2) {
        hd2d_set_debug((hd2d_debug() + 1) % HD2D_DEBUGS);
        fprintf(stderr, "ct_sdl: hd2d debug view %d\n", hd2d_debug());
        static const char *const msg[HD2D_DEBUGS] = {"Debug: off", "Debug: layers", "Debug: depth",
                                                     "Debug: blur", "Debug: compare",
                                                     "Debug: behind"};
        osd_show(msg[hd2d_debug()]);
    } else if (sc == SDL_SCANCODE_F3) {
        hd2d_set_aspect((hd2d_aspect() + 1) % HD2D_ASPECTS);
        settings_save();
        static const char *const msg[HD2D_ASPECTS] = {"Aspect: 4:3", "Aspect: pixel 8:7",
                                                      "Aspect: stretch"};
        osd_show(msg[hd2d_aspect()]);
    } else {
        return 0;
    }
    return 1;
}

/* port: HD-2D settings persist in SDL's per-user preferences folder
   (settings.ini: preset=NAME, aspect=N). */
static char *settings_path(void)
{
    char *dir = SDL_GetPrefPath("ct-recomp", "hd2d");
    if (!dir)
        return NULL;
    size_t n = strlen(dir) + sizeof "settings.ini";
    char *p = malloc(n);
    if (p)
        snprintf(p, n, "%ssettings.ini", dir);
    SDL_free(dir);
    return p;
}

static int parse_preset(const char *v);

/* port: the 3D diorama's quality (hd2d.h): "auto" (the preset's: 0 for
   the render scale, -1 for the others), a render scale in percent
   (50-100, or 0.5-1) or an amount 0-1. Returns 0, or -1 if not one. */
static int parse_quality(const char *v, int render_scale, float *out)
{
    if (!strcmp(v, "auto")) {
        *out = render_scale ? 0.0f : -1.0f;
        return 0;
    }
    char *end;
    double d = strtod(v, &end);
    if (end == v || (*end && *end != '%'))
        return -1;
    if (render_scale) {
        d = d > 1.0 ? d / 100.0 : d;
        if (d < 0.5 || d > 1.0)
            return -1;
    } else if (d < 0.0 || d > 1.0) {
        return -1;
    }
    *out = (float)d;
    return 0;
}

static void settings_load(void)
{
    char *path = settings_path(), line[4352], val[64];
    FILE *f = path ? fopen(path, "r") : NULL;
    while (f && fgets(line, sizeof line, f)) {
        if (settings_parse_line(line))   /* port: the registry's keys (settings.h) */
            continue;
        if (!strncmp(line, "packs=", 6) && !packs_given) {   /* port: asset packs */
            line[strcspn(line, "\r\n")] = 0;
            pack_names_parse(line + 6);
        }
        else if (!strncmp(line, "hd_font=", 8) && !hd_font_given) {   /* port: HD text */
            line[strcspn(line, "\r\n")] = 0;
            snprintf(hd_font, sizeof hd_font, "%.4095s", line + 8);
        }
        else if (sscanf(line, "window=%dx%d", &wst.w, &wst.h) == 2 && wst.w >= 256 &&
                 wst.h >= 224 && wst.w <= 16384 && wst.h <= 16384)
            wst.have_size = 1;
        else if (sscanf(line, "position=%d,%d", &wst.x, &wst.y) == 2)
            wst.have_pos = 1;
        else if (sscanf(line, "coop_chars=%63s", val) == 1)   /* port: co-op */
            parse_chars(val, 2);
        else if (sscanf(line, "renderer=%63s", val) == 1)   /* port: the menu's Renderer */
            renderer_saved = !strcmp(val, "hd2d") ? 1 : !strcmp(val, "plain") ? 0 : -1;
        else if (sscanf(line, "coop_chars3=%63s", val) == 1)
            parse_chars(val, 3);
        else if (bindings_parse_line(bindings_current(), line))   /* port: bind_*, key_* */
            ;
        else {   /* port: QOL settings (qol.h) */
            char key[32];
            if (sscanf(line, "%31[^=]=%63s", key, val) == 2)
                qol_setting(key, val);
        }
    }
    if (f)
        fclose(f);
    free(path);
}

static void settings_save(void)
{
    char *path = settings_path();
    FILE *f = path ? fopen(path, "w") : NULL;
    if (f) {
        /* port: the registry's keys (settings.h): preset, aspect,
           fullscreen, coop, gyro, gyro_strength, steam_hidapi, volume, ... */
        settings_write_lines(f);
        if (n_pack_names) {   /* port: asset packs, in priority order */
            fputs("packs=", f);
            for (int k = 0; k < n_pack_names; k++)
                fprintf(f, "%s%s", k ? "," : "", pack_names[k]);
            fputc('\n', f);
        }
        if (hd_font[0])   /* port: HD text */
            fprintf(f, "hd_font=%s\n", hd_font);
        if (wst.have_size)
            fprintf(f, "window=%dx%d\n", wst.w, wst.h);
        if (wst.have_pos)
            fprintf(f, "position=%d,%d\n", wst.x, wst.y);
        print_chars(f, "coop_chars", 2);
        print_chars(f, "coop_chars3", 3);
        if (renderer_saved >= 0)   /* port: only once chosen in the menu */
            fprintf(f, "renderer=%s\n", renderer_saved ? "hd2d" : "plain");
        bindings_write(bindings_current(), f);   /* port: back buttons, keys */
        qol_settings_write(f);   /* port: QOL */
        fclose(f);
    }
    free(path);
}

static int parse_preset(const char *v)
{
    for (int k = 0; k < HD2D_PRESETS; k++)
        if (!strcmp(v, hd2d_preset_name(k)))
            return k;
    return -1;
}

/* port: the glyphs of a controller, when it is the one used. */
static void use_glyphs_of(SDL_GameController *c)
{
    int set = c ? glyphs_family_of(c) : GLYPHS_KEYBOARD;
    if (set != glyphs_in_use() && log_input)
        fprintf(stderr, "ct_sdl: glyphs: %s\n", glyphs_family_name(set));
    glyphs_use(set);
}

/* port: the menu action (F10, R5, L3 + R3): the menu opens or closes.
   Before the window is up (the first-run screens) it says what the
   buttons do instead. */
static void menu_action(ct_action a, int down, void *user)
{
    (void)a;
    (void)user;
    if (!down)
        return;
    if (!menu_ready)
        osd("Menu: [@quicksave] save  [@quickload] load  [@ffwd] fast-forward");
    else if (menu_is_open())
        menu_close();
    else
        menu_open();
}

/* ---- port: the settings the menu shows (settings.h) ---- */

static void choose_pacing(void);
static int packs_setup(const char *game_id);
static void packs_upload(void);
static const char *menu_game_id;   /* the saves folder's game id, for the packs' folder */

static int on_hd2d(void *u) { (void)u; return hd2d_mode; }
static const char *const renderer_labels[2] = {"Original", "HD-2D"};
static int get_renderer(void *u) { (void)u; return renderer_saved >= 0 ? renderer_saved : hd2d_mode > 0; }
static void set_renderer(void *u, int v) { (void)u; renderer_saved = v; }
static int on_plain(void *u) { (void)u; return !hd2d_mode; }

/* Look */
static const char *const preset_labels[HD2D_PRESETS] = {"Original", "HD-2D subtle", "HD-2D",
                                                        "HD-2D Steam Deck", "HD-2D 3D diorama",
                                                        "3D diorama, Steam Deck"};
static const char *preset_values[HD2D_PRESETS];
static int get_preset(void *u)   /* the user's look, not the battery mode's (QOL) */
{
    (void)u;
    return preset_forced >= 0 ? preset_forced : qol_user_preset(hd2d_preset());
}
static void set_preset(void *u, int v)
{
    (void)u;
    preset_forced = -1;   /* the user's choice now */
    hd2d_set_preset(v);
    preset_saved = 1;     /* beats the device default */
    qol_preset_chosen();  /* and the battery mode's */
    if (frontend_ready && hd2d_mode)
        sched_set_behind(hd2d_wants_behind());
}
static const char *const aspect_labels[HD2D_ASPECTS] = {"4:3", "Pixel 8:7", "Stretch"};
static const char *const dof_labels[3] = {"Off", "Low", "Full"},
                         *const dof_values[3] = {"off", "low", "full"};
static int get_dof(void *u) { (void)u; return hd2d_dof(); }
static void set_dof(void *u, int v) { (void)u; hd2d_set_dof(v); }
static int get_perf(void *u) { (void)u; return hd2d_perf_osd(); }
static void set_perf(void *u, int v) { (void)u; hd2d_set_perf_osd(v); }
static const char *const depth3d_labels[3] = {"Full", "Subtle", "Flat"},
                         *const depth3d_values[3] = {"full", "subtle", "flat"};
static int get_depth3d(void *u) { (void)u; return hd2d_3d_depth(); }
static void set_depth3d(void *u, int v) { (void)u; hd2d_set_3d_depth(v); }
static int get_shadows3d(void *u) { (void)u; return hd2d_3d_shadows(); }
static void set_shadows3d(void *u, int v) { (void)u; hd2d_set_3d_shadows(v); }
static int get_smooth(void *u) { (void)u; return hd2d_smooth(); }
static void set_smooth(void *u, int v) { (void)u; hd2d_set_smooth(v); }
static int get_aspect(void *u) { (void)u; return hd2d_aspect(); }
static void set_aspect(void *u, int v) { (void)u; hd2d_set_aspect(v); }
static int get_cam_tilt(void *u)
{
    (void)u;
    float t, z;
    hd2d_camera(&t, &z);
    return (int)(t + 0.5f);
}
static void set_cam_tilt(void *u, int v)
{
    (void)u;
    float t, z;
    hd2d_camera(&t, &z);
    hd2d_set_camera((float)v, z);
}
static int get_cam_zoom(void *u)   /* percent of the default zoom (0.75) */
{
    (void)u;
    float t, z;
    hd2d_camera(&t, &z);
    return (int)(z / 0.75f * 100.0f + 0.5f);
}
static void set_cam_zoom(void *u, int v)
{
    (void)u;
    float t, z;
    hd2d_camera(&t, &z);
    hd2d_set_camera(t, 0.75f * (float)v / 100.0f);
}
static int get_wcam_tilt(void *u)
{
    (void)u;
    float t, z;
    hd2d_world_camera(&t, &z);
    return (int)(t + 0.5f);
}
static void set_wcam_tilt(void *u, int v)
{
    (void)u;
    float t, z;
    hd2d_world_camera(&t, &z);
    hd2d_set_world_camera((float)v, z);
}
static int get_wcam_zoom(void *u)   /* percent of the default zoom (0.75) */
{
    (void)u;
    float t, z;
    hd2d_world_camera(&t, &z);
    return (int)(z / 0.75f * 100.0f + 0.5f);
}
static void set_wcam_zoom(void *u, int v)
{
    (void)u;
    float t, z;
    hd2d_world_camera(&t, &z);
    hd2d_set_world_camera(t, 0.75f * (float)v / 100.0f);
}
static int on_3d(void *u)
{
    (void)u;
    return hd2d_mode && (hd2d_preset() == HD2D_3D || hd2d_preset() == HD2D_DECK3D);
}
static int get_gyro(void *u) { (void)u; return gyro_on; }
static void set_gyro(void *u, int v)
{
    (void)u;
    gyro_on = v;
    if (!v && frontend_ready && hd2d_mode) {
        gyro.tilt = gyro.yaw = 0.0f;
        hd2d_set_camera_offset(0.0f, 0.0f);
    }
}
static int get_gyro_strength(void *u) { (void)u; return (int)(gyro_strength * 100.0f + 0.5f); }
static void set_gyro_strength(void *u, int v) { (void)u; gyro_strength = (float)v / 100.0f; }
static int on_gyro(void *u) { (void)u; return hd2d_mode && gyro_on; }
static const char *const debug_labels[HD2D_DEBUGS] = {"Off", "Layers", "Depth", "Blur", "Compare",
                                                      "Behind"};
static int get_debug(void *u) { (void)u; return hd2d_debug(); }
static void set_debug(void *u, int v) { (void)u; hd2d_set_debug(v); }

/* The 3D diorama's quality (hd2d.h). The menu shows and settings.ini
   keeps the saved values (ini_*); --render-scale, --sharpen and --ao
   change them for the run only. Each list's first entry is the preset's
   own ("auto"). */
static float ini_render_scale, ini_sharpen = -1.0f, ini_ao = -1.0f;
static int ini_ao_size;
static const char *const rscale_labels[7] = {"Auto", "50%", "60%", "67%", "75%", "85%", "100%"};
static const char *const rscale_values[7] = {"auto", "50", "60", "67", "75", "85", "100"};
static const float scale_of[7] = {0.0f, 0.5f, 0.6f, 0.67f, 0.75f, 0.85f, 1.0f};
static const char *const amount_labels[6] = {"Auto", "Off", "25%", "50%", "75%", "100%"};
static const char *const amount_values[6] = {"auto", "0", "0.25", "0.5", "0.75", "1"};
static const float amount_of[6] = {-1.0f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
static const char *const ao_size_labels[3] = {"Auto", "Half", "Quarter"};
static const char *const ao_size_values[3] = {"auto", "half", "quarter"};
static int nearest_of(const float *of, int n, float v)   /* the list entry nearest v */
{
    int best = 0;
    for (int k = 1; k < n; k++)
        if (fabsf(of[k] - v) < fabsf(of[best] - v))
            best = k;
    return best;
}
static int get_render_scale(void *u) { (void)u; return nearest_of(scale_of, 7, ini_render_scale); }
static void set_render_scale(void *u, int v)
{
    (void)u;
    hd2d_set_render_scale(ini_render_scale = scale_of[v]);
}
static int get_sharpen(void *u) { (void)u; return nearest_of(amount_of, 6, ini_sharpen); }
static void set_sharpen(void *u, int v) { (void)u; hd2d_set_sharpen(ini_sharpen = amount_of[v]); }
static int get_ao(void *u) { (void)u; return nearest_of(amount_of, 6, ini_ao); }
static void set_ao(void *u, int v) { (void)u; hd2d_set_ao(ini_ao = amount_of[v]); }
static int get_ao_size(void *u) { (void)u; return ini_ao_size == 4 ? 2 : ini_ao_size == 2; }
static void set_ao_size(void *u, int v)
{
    (void)u;
    hd2d_set_ao_size(ini_ao_size = v == 2 ? 4 : v == 1 ? 2 : 0);
}

/* Display */
static int get_fullscreen(void *u) { (void)u; return wst.fs; }
static void set_fullscreen(void *u, int v)
{
    (void)u;
    if (win && !game_mode) {
#ifndef CT_PS5   /* port: always fullscreen (see the Fullscreen setting) */
        SDL_SetWindowFullscreen(win, v ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#endif
        wst_track();
    }
    wst.fs = v;
}
static int not_game_mode(void *u) { (void)u; return !game_mode; }
static const char *const fs_values[2] = {"0", "1"};
static const char *const pacing_labels[3] = {"Auto", "Timer", "Vsync"},
                         *const pacing_values[3] = {"auto", "timer", "vsync"};
#ifdef CT_PS5_GL
/* port: the PS5 GPU build's output size (the SDL bridge reads
   CT_PS5_DISPLAY_HEIGHT when the video starts: the next start). */
static int ps5_display = 0;   /* 1080p: the 3D diorama ran at 30 fps at 4K */
static const char *const ps5_display_labels[3] = {"1080p", "1440p", "4K"},
                         *const ps5_display_values[3] = {"1080", "1440", "2160"};
static int get_ps5_display(void *u) { (void)u; return ps5_display; }
static void set_ps5_display(void *u, int v) { (void)u; ps5_display = v < 0 || v > 2 ? 2 : v; }
#endif
static int get_pacing(void *u) { (void)u; return pace_opt; }
static void set_pacing(void *u, int v)
{
    (void)u;
    pace_opt = v;
    if (frontend_ready)
        choose_pacing();
}
static const char *const scale_labels[3] = {"Small", "Normal", "Large"},
                         *const scale_values[3] = {"small", "normal", "large"};
static int menu_scale = 1;
static int get_menu_scale(void *u) { (void)u; return menu_scale; }
static void set_menu_scale(void *u, int v) { (void)u; menu_scale = v; }

/* Controls: the bindable buttons (bindings.h) and the actions' keys */
static const struct { SDL_GameControllerButton b; const char *label; } bind_rows[] = {
    {SDL_CONTROLLER_BUTTON_PADDLE2, "L4 (back button)"}, {SDL_CONTROLLER_BUTTON_PADDLE1, "R4 (back button)"},
    {SDL_CONTROLLER_BUTTON_PADDLE4, "L5 (back button)"}, {SDL_CONTROLLER_BUTTON_PADDLE3, "R5 (back button)"},
    {SDL_CONTROLLER_BUTTON_MISC1, "Extra button"},      {SDL_CONTROLLER_BUTTON_TOUCHPAD, "Touchpad click"},
};
#define N_BIND_ROWS (int)(sizeof bind_rows / sizeof *bind_rows)
static const char *action_labels[ACTION_COUNT];
static char key_labels[ACTION_COUNT][40];
static int get_bind(void *u) { return bindings_current()->button[bind_rows[(intptr_t)u].b]; }
static void set_bind(void *u, int v)
{
    binder_release(&pad_binder[0], bindings_current());   /* nothing held across the change */
    bindings_current()->button[bind_rows[(intptr_t)u].b] = (unsigned char)v;
}
static int get_key(void *u) { return bindings_current()->key[(intptr_t)u]; }
static void set_key(void *u, int v)
{
    bindings_keys_release();
    bindings *b = bindings_current();
    for (int a = 0; a < ACTION_COUNT && v; a++)   /* one action per key */
        if (b->key[a] == (SDL_Scancode)v)
            b->key[a] = SDL_SCANCODE_UNKNOWN;
    b->key[(intptr_t)u] = (SDL_Scancode)v;
}
static const char *key_namer(int sc) { return SDL_GetScancodeName((SDL_Scancode)sc); }
static int get_hidapi(void *u) { (void)u; return steam_hidapi; }
static void set_hidapi(void *u, int v) { (void)u; steam_hidapi = v; }
static const char *const hid_deck_labels[] = {"Auto", "On", "Off"};
static const char *const hid_deck_values[] = {"auto", "on", "off"};
static int get_hid_deck(void *u) { (void)u; return hid_deck_mode(); }
static void set_hid_deck(void *u, int v) { (void)u; hid_deck_set_mode(v); }
/* port: simulated rumble */
static const char *const rumble_labels[RUMBLE_LEVELS] = {"Off", "Light", "Medium", "Strong"};
static const char *const rumble_values[RUMBLE_LEVELS] = {"off", "light", "medium", "strong"};
static int rumble_sounds;
static long rumble_test_at = -1;   /* --rumble-test F */
static void rumble_apply(void)
{
    /* the InputPlumber pad is opened read-write only when it may rumble */
    hid_deck_set_rumble(rumble_out_level() != RUMBLE_OFF || padtest_active());
    rumble_set_detectors(1, rumble_sounds);
}
static int get_rumble(void *u) { (void)u; return rumble_out_level(); }
static void set_rumble(void *u, int v)
{
    (void)u;
    rumble_out_set_level(v);
    rumble_apply();
}
static int get_rumble_sounds(void *u) { (void)u; return rumble_sounds; }
static void set_rumble_sounds(void *u, int v)
{
    (void)u;
    rumble_sounds = v != 0;
    rumble_apply();
}
static int rumble_is_on(void *u) { (void)u; return rumble_out_level() != RUMBLE_OFF; }
static void run_rumble_test(void *u)
{
    (void)u;
    rumble_out_test(SDL_GetTicks());
}
static const char *menu_combo_text(void *u, char *buf, size_t n)
{
    (void)u;
    snprintf(buf, n, "F10, R5, or L3 + R3");
    return buf;
}

/* Co-op */
static const char *const coop_labels[3] = {"Auto", "On", "Off"},
                         *const coop_values[3] = {"auto", "on", "off"};
static int get_coop(void *u) { (void)u; return coop_pref; }
static void set_coop(void *u, int v)
{
    (void)u;
    coop_pref = v;
    if (frontend_ready)
        coop_update();
}
static const char *const player_labels[3] = {"Player 1", "Player 2", "Player 3"};
static int get_slot(void *u) { return slot_player[(intptr_t)u] - 1; }
static void set_slot(void *u, int v)
{
    slot_player[(intptr_t)u] = (uint8_t)(v + 1);
    if (frontend_ready)
        coop_update();
}
static int get_split(void *u) { (void)u; return sched_views() > 1; }
static void set_split(void *u, int v) { (void)u; sched_set_views(v ? 2 : 1); }

/* Asset packs: the folders in <pref path>/packs and the ones loaded. */
#define MAX_PACKS_LISTED 32
static char packs_listed[MAX_PACKS_LISTED][256];
static int n_packs_listed, pack_dump_live;
static char packs_folder[512];

static void pack_list_add(const char *name)
{
    for (int k = 0; k < n_packs_listed; k++)
        if (!strcmp(packs_listed[k], name))
            return;
    if (n_packs_listed < MAX_PACKS_LISTED && strlen(name) < sizeof packs_listed[0])
        snprintf(packs_listed[n_packs_listed++], sizeof packs_listed[0], "%s", name);
}

static int pack_position(const char *name)   /* 1.. in the loaded list, 0: off */
{
    for (int k = 0; k < n_pack_names; k++)
        if (!strcmp(pack_names[k], name))
            return k + 1;
    return 0;
}

static int get_pack(void *u) { return pack_position(packs_listed[(intptr_t)u]); }

/* The loaded packs again, from pack_names, live in the GL renderer. */
static void packs_apply(void)
{
    if (!hd2d_mode) {
        osd("Packs: used from the next start in HD-2D");
        return;
    }
    packs_unload();
    int r = n_pack_names ? packs_setup(menu_game_id) : 0;
    packs_upload();
    sched_set_prov(want_prov());
    char msg[64];
    int n;
    packs_entries(&n);
    snprintf(msg, sizeof msg, r < 0 ? "Packs: none readable" : "Packs: %d replacements", n);
    osd(msg);
}

static void set_pack(void *u, int v)
{
    const char *name = packs_listed[(intptr_t)u];
    char keep[256];
    snprintf(keep, sizeof keep, "%s", name);
    int at = pack_position(keep);
    if (at) {   /* out of the list */
        for (int k = at - 1; k + 1 < n_pack_names; k++)
            memcpy(pack_names[k], pack_names[k + 1], sizeof pack_names[0]);
        n_pack_names--;
    }
    if (v > 0 && n_pack_names < MAX_PACK_NAMES) {   /* in again, at position v */
        int pos = v - 1 > n_pack_names ? n_pack_names : v - 1;
        for (int k = n_pack_names; k > pos; k--)
            memcpy(pack_names[k], pack_names[k - 1], sizeof pack_names[0]);
        snprintf(pack_names[pos], sizeof pack_names[0], "%s", keep);
        n_pack_names++;
    }
    packs_given = 0;   /* the menu's list is the one to keep */
    packs_apply();
}

static const char *pack_option(void *u, int v, char *buf, size_t n)
{
    (void)u;
    static const char *const nth[] = {"1st", "2nd", "3rd"};
    if (!v)
        snprintf(buf, n, "Off");
    else if (v <= 3)
        snprintf(buf, n, "On, %s", nth[v - 1]);
    else
        snprintf(buf, n, "On, %dth", v);
    return buf;
}

static void run_pack_reload(void *u)
{
    (void)u;
    int n = packs_reload();
    music_reload();
    packs_upload();
    sched_set_prov(want_prov());
    char msg[48];
    snprintf(msg, sizeof msg, n < 0 ? "Packs: none readable" : "Packs reloaded: %d", n);
    osd(msg);
}
static int can_reload(void *u) { (void)u; return hd2d_mode && n_pack_names > 0; }

static void pack_dump_live_close(void)
{
    if (pack_dump_live) {
        pack_dump_report();
        pack_dump_live = 0;
    }
}
static int get_dump(void *u) { (void)u; return pack_dump != NULL; }
static void set_dump(void *u, int v)
{
    (void)u;
    if (v && !pack_dump) {
        char *dir = pack_dir(menu_game_id, "dump");
        if (dir && !packs_dump_open(dir, "dump")) {
            static int at_exit;
            if (!at_exit++)
                atexit(pack_dump_live_close);
            pack_dump = "dump";
            pack_dump_live = 1;
            sched_set_prov(1);
            osd("Dumping new art to the \"dump\" pack");
        }
        free(dir);
    } else if (!v && pack_dump_live) {
        pack_dump_live_close();
        pack_dump = NULL;
        sched_set_prov(want_prov());
    }
}
static int can_dump(void *u) { (void)u; return hd2d_mode && (!pack_dump || pack_dump_live); }
static const char *folder_text(void *u, char *buf, size_t n)
{
    (void)u;
    snprintf(buf, n, "%s", packs_folder);
    return buf;
}
static const char *packs_count_text(void *u, char *buf, size_t n)
{
    (void)u;
    int e = 0;
    packs_entries(&e);
    snprintf(buf, n, "%d loaded, %d replacements", n_pack_names, packs_active() ? e : 0);
    return buf;
}

/* The pack rows, rebuilt each time the menu opens (the folder may change). */
#ifdef CT_PS5
static char packs_scan_text[160] = "build 32: not checked";
static const char *packs_scan_get(void *u, char *buf, size_t n)
{
    (void)u;
    snprintf(buf, n, "%s", packs_scan_text);
    return buf;
}
#endif
static void packs_refresh(void)
{
    settings_unregister_category("Packs");
    n_packs_listed = 0;
    for (int k = 0; k < n_pack_names; k++)
        pack_list_add(pack_names[k]);
    {
        char root[400];
        mods_root(menu_game_id ? menu_game_id : "game", root, sizeof root);
        snprintf(packs_folder, sizeof packs_folder, "%spacks", root);
#ifdef CT_PS5
        mkdir(packs_folder, 0777);   /* there to copy into */
#endif
    }
#if defined(__unix__) || defined(__APPLE__) || defined(CT_PS5)
    /* port: a folder is a pack if NAME/pack.toml opens (or NAME/NAME/pack.toml,
       unzipped a level too deep); no stat(), whose struct differs on the PS5 */
    DIR *d = opendir(packs_folder);
#ifdef CT_PS5
    /* the report beside the saves (that folder is known to be writable),
       with what the data folder itself holds, and a notification */
    FILE *scan = fopen("/data/ct-recomp/packs-scan.txt", "w");
    int scan_entries = 0, scan_packs0 = n_packs_listed;
    if (scan) {
        int ds[3];
        ps5_dir_status(ds);
        fprintf(scan, "packs folder %s: %s (open %#x / %#x)\n", packs_folder,
                d ? "opened" : "cannot open", (unsigned)ds[0], (unsigned)ds[1]);
        DIR *r = opendir("/data/ct-recomp");
        struct dirent *re;
        fprintf(scan, "/data/ct-recomp holds:%s\n", r ? "" : " (cannot open)");
        while (r && (re = readdir(r)))
            fprintf(scan, "  \"%s\" (type %d)\n", re->d_name, re->d_type);
        if (r)
            closedir(r);
        {
            int ds3[3];
            ps5_dir_status(ds3);
            const unsigned char *pk = ps5_dir_peek();
            fprintf(scan, "getdents: %d; first bytes:", ds3[2]);
            for (int k = 0; k < 48; k++)
                fprintf(scan, " %02x", pk[k]);
            fprintf(scan, "\n");
        }
        fprintf(scan, "%s holds:\n", packs_folder);
    }
#endif
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "dump"))
            continue;
        char t[1100], n[600];
        snprintf(t, sizeof t, "%s/%s/pack.toml", packs_folder, e->d_name);
        FILE *f = fopen(t, "r");
        const char *how = "no pack.toml";
        if (f) {
            fclose(f);
            pack_list_add(e->d_name);
            how = "pack";
        } else {
            snprintf(t, sizeof t, "%s/%s/%s/pack.toml", packs_folder, e->d_name, e->d_name);
            if ((f = fopen(t, "r"))) {
                fclose(f);
                snprintf(n, sizeof n, "%s/%s", e->d_name, e->d_name);
                pack_list_add(n);
                how = "pack (one level deeper)";
            }
        }
#ifdef CT_PS5
        scan_entries++;
        if (scan)
            fprintf(scan, "  \"%s\" (type %d): %s\n", e->d_name, e->d_type, how);
#else
        (void)how;
#endif
    }
#ifdef CT_PS5
    /* port: names tried without the listing: the lines of packs.txt (in the
       packs folder, or one level down where an unzip put it), and the
       names of the packs this project ships, each as NAME, NAME/NAME and
       NAME-pack/NAME (an unzip into a folder named after the zip) */
    {
        static const char *const known[] = {"crono-remaster", NULL};
        char names[24][200];
        int nn = 0;
        for (int k = 0; known[k]; k++)
            snprintf(names[nn++], sizeof names[0], "%s", known[k]);
        const char *txts[] = {"packs.txt", "crono-remaster-pack/packs.txt", NULL};
        for (int k = 0; txts[k]; k++) {
            char t[700], line[200];
            snprintf(t, sizeof t, "%s/%s", packs_folder, txts[k]);
            FILE *lf = fopen(t, "r");
            while (lf && nn < 24 && fgets(line, sizeof line, lf)) {
                line[strcspn(line, "\r\n")] = 0;
                if (line[0] && line[0] != '#')
                    snprintf(names[nn++], sizeof names[0], "%s", line);
            }
            if (lf)
                fclose(lf);
        }
        for (int k = 0; k < nn; k++) {
            char cand[3][420];
            snprintf(cand[0], sizeof cand[0], "%s", names[k]);
            snprintf(cand[1], sizeof cand[1], "%s/%s", names[k], names[k]);
            snprintf(cand[2], sizeof cand[2], "%s-pack/%s", names[k], names[k]);
            for (int c = 0; c < 3; c++) {
                char pt[1100];
                snprintf(pt, sizeof pt, "%s/%s/pack.toml", packs_folder, cand[c]);
                FILE *f = fopen(pt, "r");
                if (scan)
                    fprintf(scan, "  tried \"%s\": %s\n", cand[c], f ? "pack" : "no");
                if (f) {
                    fclose(f);
                    pack_list_add(cand[c]);
                    break;
                }
            }
        }
    }
    {
        int ds2[3];
        ps5_dir_status(ds2);
        snprintf(packs_scan_text, sizeof packs_scan_text, "build 32: %s, %d seen, %d packs (%x %x %x)",
                 d ? "opened" : "not readable", scan_entries, n_packs_listed - scan_packs0,
                 (unsigned)ds2[0], (unsigned)ds2[1], (unsigned)ds2[2]);
    }
    if (scan)
        fclose(scan);
    {
        static int told;
        int ds[3];
        ps5_dir_status(ds);
        if (!told++)
            ps5_notify("Packs folder: %s, %d entries, %d packs (%x %x %x)",
                       d ? "found" : "not readable", scan_entries, n_packs_listed - scan_packs0,
                       (unsigned)ds[0], (unsigned)ds[1], (unsigned)ds[2]);
    }
#endif
    if (d)
        closedir(d);
#endif
#ifdef CT_PS5
    settings_register(&(setting){
        .label = "Export character poses", .category = "Packs", .key = "pose_export", .type = SETTING_BOOL,
        .get = get_pose_export, .set = set_pose_export, .flags = SETTING_NO_SAVE,
        .help = "Save every character frame drawn on the field into "
                "/data/ct-recomp/sheets/ct5 (one picture per frame, and sheets when "
                "turned off), to make a character pack from."});
#endif
#ifdef CT_PS5
    settings_register(&(setting){.label = "Folder check", .category = "Packs",
                                 .type = SETTING_INFO, .text = packs_scan_get,
                                 .help = "What CT5 found in the packs folder (for a photo if a "
                                         "pack is missing)."});
#endif
    settings_register(&(setting){.label = "Asset packs", .category = "Packs", .type = SETTING_INFO,
                                 .flags = SETTING_HEADING});
    settings_register(&(setting){
        .label = "Loaded", .category = "Packs", .type = SETTING_INFO, .text = packs_count_text,
        .help = "Packs replace the game's art with new pictures. The first one listed wins."});
    for (int k = 0; k < n_packs_listed; k++)
        settings_register(&(setting){
            .label = packs_listed[k], .category = "Packs", .type = SETTING_ENUM,
            .count = n_packs_listed + 1, .get = get_pack, .set = set_pack, .option = pack_option,
            .user = (void *)(intptr_t)k,
            .help = "Off, or on at this place in the order (1st wins over the others)."});
    if (!n_packs_listed)
        settings_register(&(setting){.label = "No packs installed", .category = "Packs",
                                     .type = SETTING_INFO,
                                     .help = "Put each pack in its own folder in the packs folder "
                                             "below (docs/ASSET-PACKS.md)."});
    settings_register(&(setting){.label = "Reload packs", .category = "Packs", .type = SETTING_ACTION,
                                 .run = run_pack_reload, .enabled = can_reload,
                                 .help = "Read the loaded packs from disk again (F5)."});
    settings_register(&(setting){
        .label = "Dump new art", .category = "Packs", .type = SETTING_BOOL, .get = get_dump,
        .set = set_dump, .enabled = can_dump, .flags = SETTING_NO_SAVE,
        .help = "Save each new tile and sprite drawn into the \"dump\" pack, to draw over."});
    settings_register(&(setting){.label = "Folder", .category = "Packs", .type = SETTING_INFO,
                                 .text = folder_text, .help = packs_folder});
}

/* port: HD text */
static const char *hd_font_text(void *u, char *buf, size_t n)
{
    (void)u;
    const char *p = hdtext_font_path();
    if (!p)
        return hd_font[0] && strcmp(hd_font, "none") ? "Not loaded" : "None";
    const char *b = strrchr(p, '/');
    snprintf(buf, n, "%s", b ? b + 1 : p);
    return buf;
}
static int hd_text_on(void *u) { (void)u; return hdtext_ready(); }
static int get_hd_style(void *u) { (void)u; return hd_text_style; }
static void set_hd_style(void *u, int v) { (void)u; hdtext_set_style(hd_text_style = v); }
static int get_hd_size(void *u) { (void)u; return hd_text_size; }
static void set_hd_size(void *u, int v) { (void)u; hdtext_set_size(hd_text_size = v); }

/* Audio */
static int get_volume(void *u) { (void)u; return volume; }
static void set_volume(void *u, int v) { (void)u; volume = v; }
static int get_mute_unfocused(void *u) { (void)u; return mute_unfocused; }
static void set_mute_unfocused(void *u, int v) { (void)u; mute_unfocused = v; }
static int get_audio_interp(void *u) { (void)u; return sched_audio_interp(); }
static void set_audio_interp(void *u, int v) { (void)u; sched_set_audio_interp(v); }

/* About */
static const char *version_text(void *u, char *buf, size_t n)
{
    (void)u;
    snprintf(buf, n, "%s", CT_VERSION);
    return buf;
}
static const char *license_text(void *u, char *buf, size_t n)
{
    (void)buf;
    (void)n;
    return (const char *)u;
}
static void run_quit(void *u)
{
    (void)u;
    set_exit_reason("Quit in the menu");
    running = 0;
    menu_close();
}

static void register_settings(void)
{
    static const char *const cats[] = {"Look", "Display", "Controls", "Co-op", "Packs",
                                       "Save states", "Audio", "About"};
    for (size_t k = 0; k < sizeof cats / sizeof *cats; k++)
        settings_category_add(cats[k]);
    settings_key_namer(key_namer);
    for (int k = 0; k < HD2D_PRESETS; k++)
        preset_values[k] = hd2d_preset_name(k);
    for (int a = 0; a < ACTION_COUNT; a++)
        action_labels[a] = action_title((ct_action)a);

    /* Look */
    settings_register(&(setting){
        .label = "Renderer", .category = "Look", .type = SETTING_ENUM, .options = renderer_labels,
        .count = 2, .get = get_renderer, .set = set_renderer, .flags = SETTING_RESTART,
        .help = "Original: the console's picture, scaled. HD-2D: the OpenGL renderer and its "
                "looks below. --hd2d and --no-hd2d still win."});
    settings_register(&(setting){
        .key = "preset", .label = "Look", .category = "Look", .type = SETTING_ENUM,
        .options = preset_labels, .values = preset_values, .count = HD2D_PRESETS, .get = get_preset,
        .set = set_preset,
        .help = "Original pixels, or the HD-2D renderer: light, depth of field, bloom; the 3D "
                "diorama rebuilds maps in 3D. Used by the HD-2D renderer. (F1, R3)"});
    settings_register(&(setting){
        .key = "dof", .label = "Depth of field", .category = "Look", .type = SETTING_ENUM,
        .options = dof_labels, .values = dof_values, .count = 3, .get = get_dof, .set = set_dof,
        .enabled = on_hd2d,
        .help = "The miniature-model blur towards the top and bottom of the picture: off, a "
                "little, or the look's full amount."});
    settings_register(&(setting){
        .key = "depth_3d", .label = "3D depth", .category = "Look", .type = SETTING_ENUM,
        .options = depth3d_labels, .values = depth3d_values, .count = 3, .get = get_depth3d,
        .set = set_depth3d, .enabled = on_hd2d,
        .help = "How far the 3D diorama's raised layers (treetops, roofs, table tops) move "
                "against the ground as the camera moves. Full: the most depth, with seams "
                "inside tables and tents. Flat: lined up exactly, depth from light and shadow."});
    settings_register(&(setting){
        .key = "shadows_3d", .label = "3D shadows", .category = "Look", .type = SETTING_BOOL,
        .get = get_shadows3d, .set = set_shadows3d, .enabled = on_hd2d,
        .help = "Shadows cast by treetops, roofs and characters in the 3D diorama. Off is "
                "faster."});
    settings_register(&(setting){
        .key = "smooth_3d", .label = "3D texture smoothing", .category = "Look",
        .type = SETTING_BOOL, .get = get_smooth, .set = set_smooth, .enabled = on_hd2d,
        .help = "Pixel-art anti-aliasing in the 3D diorama: tilted and distant tiles stay even "
                "instead of crunchy. Turn off for hard pixels or a little more speed."});
#ifdef CT_PS5
    settings_register(&(setting){
        .key = "perf_readout", .label = "Performance readout", .category = "Look",
        .type = SETTING_BOOL, .get = get_perf, .set = set_perf, .enabled = on_hd2d,
        .help = "Every few seconds, the time each frame takes on the GPU and CPU and its "
                "three slowest steps, at the bottom of the picture. Under 16.7 ms is full "
                "speed."});
#endif
    settings_register(&(setting){.key = "aspect", .label = "Aspect", .category = "Look",
                                 .type = SETTING_ENUM, .options = aspect_labels,
                                 .count = HD2D_ASPECTS, .get = get_aspect, .set = set_aspect,
                                 .help = "The picture's shape: a 4:3 TV, the console's own pixels (8:7), "
                                         "or the whole window, in both renderers (the original "
                                         "one: with Integer scale off). (F3, L3)"});
    settings_register(&(setting){.label = "3D camera", .category = "Look", .type = SETTING_INFO,
                                 .flags = SETTING_HEADING});
    settings_register(&(setting){.key = "cam_tilt", .label = "Camera tilt", .category = "Look",
                                 .type = SETTING_INT, .min = 20, .max = 70, .step = 5,
                                 .format = "%d\xc2\xb0", .get = get_cam_tilt, .set = set_cam_tilt,
                                 .enabled = on_3d,
                                 .help = "How far the 3D diorama's camera leans back from "
                                         "looking straight down (40\xc2\xb0)."});
    settings_register(&(setting){.key = "cam_zoom", .label = "Camera zoom", .category = "Look",
                                 .type = SETTING_INT, .min = 60, .max = 160, .step = 10,
                                 .format = "%d%%", .get = get_cam_zoom, .set = set_cam_zoom,
                                 .enabled = on_3d,
                                 .help = "How close the 3D diorama's camera is (100%: about the "
                                         "console's view)."});
    settings_register(&(setting){.key = "worldmap_tilt", .label = "World map tilt",
                                 .category = "Look", .type = SETTING_INT, .min = 10, .max = 70,
                                 .step = 5, .format = "%d\xc2\xb0", .get = get_wcam_tilt,
                                 .set = set_wcam_tilt, .enabled = on_3d,
                                 .help = "The same for the world map: lower looks down from "
                                         "higher up (40\xc2\xb0)."});
    settings_register(&(setting){.key = "worldmap_zoom", .label = "World map zoom",
                                 .category = "Look", .type = SETTING_INT, .min = 40, .max = 160,
                                 .step = 10, .format = "%d%%", .get = get_wcam_zoom,
                                 .set = set_wcam_zoom, .enabled = on_3d,
                                 .help = "How close the world map's camera is (100%: about the "
                                         "console's view; less shows more of the world)."});
    settings_register(&(setting){
        .key = "render_scale", .label = "3D render scale", .category = "Look",
        .type = SETTING_ENUM, .options = rscale_labels, .values = rscale_values, .count = 7,
        .get = get_render_scale, .set = set_render_scale, .enabled = on_3d,
        .help = "Draw the 3D scene smaller and upscale it with AMD FSR 1: faster on handhelds. "
                "The UI stays sharp. Auto: 100%, 67% in the Steam Deck 3D look."});
    settings_register(&(setting){
        .key = "sharpen", .label = "Sharpening", .category = "Look", .type = SETTING_ENUM,
        .options = amount_labels, .values = amount_values, .count = 6, .get = get_sharpen,
        .set = set_sharpen, .enabled = on_3d,
        .help = "AMD CAS on the 3D scene at 100%, FSR's RCAS below it. Auto: off at 100%, "
                "50% below."});
    settings_register(&(setting){
        .key = "ao", .label = "Ambient occlusion", .category = "Look", .type = SETTING_ENUM,
        .options = amount_labels, .values = amount_values, .count = 6, .get = get_ao,
        .set = set_ao, .enabled = on_3d,
        .help = "Soft contact shadows where walls meet floors, in corners and under eaves. "
                "Auto: 75%."});
    settings_register(&(setting){
        .key = "ao_size", .label = "Occlusion quality", .category = "Look",
        .type = SETTING_ENUM, .options = ao_size_labels, .values = ao_size_values, .count = 3,
        .get = get_ao_size, .set = set_ao_size, .enabled = on_3d,
        .help = "The occlusion's resolution: half or a quarter of the 3D scene's. Auto: half, "
                "a quarter in the Steam Deck 3D look."});
    settings_register(&(setting){.key = "gyro", .label = "Gyro aiming", .category = "Look",
                                 .type = SETTING_BOOL, .get = get_gyro, .set = set_gyro,
                                 .enabled = on_hd2d,
                                 .help = "Turn the controller to turn the 3D camera a little "
                                         "(Steam Deck, Switch and PlayStation pads)."});
    settings_register(&(setting){.key = "gyro_strength", .label = "Gyro strength",
                                 .category = "Look", .type = SETTING_INT, .min = 25, .max = 300,
                                 .step = 25, .format = "%d%%", .ini_div = 100,
                                 .get = get_gyro_strength, .set = set_gyro_strength,
                                 .enabled = on_gyro, .help = "Camera degrees per degree turned."});
    /* port: HD text (hdtext.h) */
    settings_register(&(setting){.label = "HD text font", .category = "Look", .type = SETTING_INFO,
                                 .text = hd_font_text,
                                 .help = "Text boxes drawn with your own font at the screen's "
                                         "resolution: hd_font=FILE in settings.ini (fonts/ in "
                                         "the data folder), or --hd-font. Takes effect on the "
                                         "next start."});
    static const char *const hd_style_labels[] = {"Shadow", "Outline", "Plain"};
    static const char *const hd_style_values[] = {"shadow", "outline", "plain"};
    settings_register(&(setting){
        .key = "hd_text_style", .label = "HD text style", .category = "Look",
        .type = SETTING_ENUM, .options = hd_style_labels, .values = hd_style_values, .count = 3,
        .get = get_hd_style, .set = set_hd_style, .enabled = hd_text_on,
        .help = "A drop shadow like the console's, an outline all round, or none."});
    settings_register(&(setting){.key = "hd_text_size", .label = "HD text size",
                                 .category = "Look", .type = SETTING_INT, .min = 50, .max = 150,
                                 .step = 5, .format = "%d%%", .get = get_hd_size,
                                 .set = set_hd_size, .enabled = hd_text_on,
                                 .help = "The font's size against the console's."});
    settings_register(&(setting){.label = "Debug view", .category = "Look", .type = SETTING_ENUM,
                                 .options = debug_labels, .count = HD2D_DEBUGS, .get = get_debug,
                                 .set = set_debug, .enabled = on_hd2d, .flags = SETTING_NO_SAVE,
                                 .help = "The renderer's inputs, for checking its work. Not "
                                         "saved. (F2)"});

    /* Display */
#ifndef CT_PS5   /* port: the PS5 has one screen, always used whole; switching
                    re-opened its video output (a flicker, the menu stuck) */
    settings_register(&(setting){.key = "fullscreen", .label = "Fullscreen", .category = "Display",
                                 .type = SETTING_BOOL, .values = fs_values, .get = get_fullscreen,
                                 .set = set_fullscreen, .enabled = not_game_mode,
                                 .help = "Always on in Steam's Game Mode. (F11)"});
#endif
#ifdef CT_PS5_GL
    settings_register(&(setting){
        .key = "ps5_resolution", .label = "Resolution", .category = "Display", .type = SETTING_ENUM,
        .options = ps5_display_labels, .values = ps5_display_values, .count = 3,
        .get = get_ps5_display, .set = set_ps5_display, .flags = SETTING_RESTART,
        .help = "The picture the PS5 draws; the console scales it to the TV. 4K is the "
                "sharpest (the 3D diorama is drawn at 1080 lines and upscaled), 1080p the "
                "lightest. Takes effect on the next start."});
#endif
    settings_register(&(setting){
        .key = "pacing", .label = "Frame pacing", .category = "Display", .type = SETTING_ENUM,
        .options = pacing_labels, .values = pacing_values, .count = 3, .get = get_pacing,
        .set = set_pacing,
        .help = "Timer: 60.1 frames a second by the clock (Auto, for now). Vsync: by the display "
                "(60 Hz displays)."});
    settings_register(&(setting){.key = "menu_scale", .label = "Menu text size",
                                 .category = "Display", .type = SETTING_ENUM,
                                 .options = scale_labels, .values = scale_values, .count = 3,
                                 .get = get_menu_scale, .set = set_menu_scale,
                                 .help = "The size of this menu's text."});

    /* Controls */
    settings_register(&(setting){.label = "Open this menu", .category = "Controls",
                                 .type = SETTING_INFO, .text = menu_combo_text,
                                 .help = "On a controller without back buttons, click both "
                                         "sticks together."});
    settings_register(&(setting){.label = "Back buttons", .category = "Controls",
                                 .type = SETTING_INFO, .flags = SETTING_HEADING});
    for (int k = 0; k < N_BIND_ROWS; k++)
        settings_register(&(setting){
            .label = bind_rows[k].label, .category = "Controls", .type = SETTING_ENUM,
            .options = action_labels, .count = ACTION_COUNT, .get = get_bind, .set = set_bind,
            .user = (void *)(intptr_t)k,
            .help = "Steam Deck back buttons, Steam Controller grips, Elite paddles. Under Steam "
                    "Input, Steam's layout decides what they send."});
    settings_register(&(setting){.label = "Keyboard", .category = "Controls", .type = SETTING_INFO,
                                 .flags = SETTING_HEADING});
    for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
        snprintf(key_labels[a], sizeof key_labels[a], "%s key", action_title((ct_action)a));
        settings_register(&(setting){.label = key_labels[a], .category = "Controls",
                                     .type = SETTING_KEY, .get = get_key, .set = set_key,
                                     .user = (void *)(intptr_t)a,
                                     .help = "Pick, then press the key. Backspace: no key."});
    }
    settings_register(&(setting){.label = "Controller", .category = "Controls",
                                 .type = SETTING_INFO, .flags = SETTING_HEADING});
    settings_register(&(setting){
        .key = "steam_hidapi", .label = "Steam Controller driver", .category = "Controls",
        .type = SETTING_BOOL, .get = get_hidapi, .set = set_hidapi, .flags = SETTING_RESTART,
        .help = "SDL's own driver for the Steam Controller, back buttons included, when Steam "
                "Input isn't running."});
    settings_register(&(setting){
        .key = "hid_deck", .label = "Built-in controls (InputPlumber)", .category = "Controls",
        .type = SETTING_ENUM, .options = hid_deck_labels, .values = hid_deck_values, .count = 3,
        .get = get_hid_deck, .set = set_hid_deck,
        .help = "Bazzite's InputPlumber shows an Ally's or Legion Go's controls as a Steam Deck "
                "pad only Steam reads. Auto: read it while no other controller is there."});
    settings_register(&(setting){
        .key = "rumble", .label = "Rumble", .category = "Controls", .type = SETTING_ENUM,
        .options = rumble_labels, .values = rumble_values, .count = RUMBLE_LEVELS,
        .get = get_rumble, .set = set_rumble,
        .help = "Pulses from the game: hits, criticals, enemies going down, earthquakes, the "
                "screen shaking or flashing. Co-op: each player feels their own party member's."});
    settings_register(&(setting){
        .key = "rumble_sounds", .label = "Rumble from low sounds", .category = "Controls",
        .type = SETTING_BOOL, .get = get_rumble_sounds, .set = set_rumble_sounds,
        .enabled = rumble_is_on,
        .help = "Also pulse on explosions and thuds heard in the sound (low booms)."});
    settings_register(&(setting){
        .label = "Test rumble", .category = "Controls", .type = SETTING_ACTION,
        .run = run_rumble_test, .enabled = rumble_is_on,
        .help = "A short pulse on every controller in play, at the strength set."});

    /* Co-op */
    settings_register(&(setting){
        .key = "coop", .label = "Co-op", .category = "Co-op", .type = SETTING_ENUM,
        .options = coop_labels, .values = coop_values, .count = 3, .get = get_coop,
        .set = set_coop,
        .help = "Auto: on while two controllers are connected. A second controller plays pad 2. "
                "(F4)"});
    settings_register(&(setting){.label = "Who plays which party member", .category = "Co-op",
                                 .type = SETTING_INFO, .flags = SETTING_HEADING});
    static const char *const slot_labels[3] = {"Party member 1", "Party member 2",
                                               "Party member 3"};
    for (int k = 0; k < 3; k++)
        settings_register(&(setting){
            .label = slot_labels[k], .category = "Co-op", .type = SETTING_ENUM,
            .options = player_labels, .count = 3, .get = get_slot, .set = set_slot,
            .user = (void *)(intptr_t)k,
            .help = "Player 3 plays only with a third controller; until then player 1 has "
                    "theirs."});
    settings_register(&(setting){.label = "Split screen (experimental)", .category = "Co-op",
                                 .type = SETTING_BOOL, .get = get_split, .set = set_split,
                                 .enabled = on_plain, .flags = SETTING_NO_SAVE,
                                 .help = "A second view when player 2 walks away. Original "
                                         "picture only (--no-hd2d); not saved."});

    /* Packs: packs_refresh, each time the menu opens */
    settings_on_refresh(packs_refresh);

    /* Save states, screenshots, fast-forward, turbo, always run, pausing,
       integer scale, CRT, battery: QOL's own rows (qol.h). */
    qol_register_settings();

    /* Audio */
    settings_register(&(setting){.key = "volume", .label = "Volume", .category = "Audio",
                                 .type = SETTING_INT, .min = 0, .max = 100, .step = 5,
                                 .format = "%d%%", .get = get_volume, .set = set_volume,
                                 .help = "The game's sound."});
    settings_register(&(setting){.key = "mute_unfocused", .label = "Mute in the background",
                                 .category = "Audio", .type = SETTING_BOOL,
                                 .get = get_mute_unfocused, .set = set_mute_unfocused,
                                 .help = "Silent while another window has the focus."});
    static const char *const interp_labels[] = {"Gaussian (console)", "Cubic", "Sinc"};
    static const char *const interp_values[] = {"gaussian", "cubic", "sinc"};
    settings_register(&(setting){
        .key = "audio_interp", .label = "Sample interpolation", .category = "Audio",
        .type = SETTING_ENUM, .options = interp_labels, .values = interp_values, .count = 3,
        .get = get_audio_interp, .set = set_audio_interp,
        .help = "How the sound chip blends its samples. Gaussian is the console's own, slightly "
                "muffled; cubic and sinc are crisper."});

    /* About */
    settings_register(&(setting){.label = APP_NAME, .category = "About", .type = SETTING_INFO,
                                 .text = version_text,
                                 .help = "A static recompilation of a SNES game for PCs and "
                                         "handhelds. Free software: GPL-3.0-or-later."});
    settings_register(&(setting){
        .label = "Bring your own ROM", .category = "About", .type = SETTING_INFO,
        .help = "No game data comes with " APP_NAME ". It runs only from a ROM you dumped from "
                "a cartridge you own, and never downloads or shares one."});
    settings_register(&(setting){.label = "Licenses", .category = "About", .type = SETTING_INFO,
                                 .flags = SETTING_HEADING});
    static const struct { const char *name, *license, *help; } lic[] = {
        {APP_NAME, "GPL-3.0-or-later",
         "This program. Parts from ct-recomp (Akhil Moola) stay MIT; REUSE.toml maps every file."},
        {"ct-recomp", "MIT", "The recompiler and runtime this is built on (Akhil Moola)."},
        {"zelda3 SNES core", "MIT", "PPU, DMA, APU and DSP emulation (snesrev, elzo_d)."},
        {"ares SPC700", "ISC", "The sound CPU core (ares team, Near et al)."},
        {"Dear ImGui", "MIT", "This menu's GUI library (Omar Cornut), v1.92.9b."},
        {"Roboto", "Apache-2.0", "This menu's font (Christian Robertson, Google)."},
        {"cgltf", "MIT", "glTF reader for asset packs' models (Johannes Kuhlmann)."},
        {"Kenney Input Prompts", "CC0-1.0", "The button pictures (Kenney)."},
        {"SDL2", "Zlib", "Windows, input and sound (the SDL developers)."},
    };
    for (size_t k = 0; k < sizeof lic / sizeof *lic; k++)
        settings_register(&(setting){.label = lic[k].name, .category = "About",
                                     .type = SETTING_INFO, .text = license_text,
                                     .user = (void *)lic[k].license, .help = lic[k].help});
    settings_register(&(setting){.label = "", .category = "About", .type = SETTING_INFO,
                                 .flags = SETTING_HEADING});
    settings_register(&(setting){.label = "Quit " APP_NAME, .category = "About",
                                 .type = SETTING_ACTION, .run = run_quit, .flags = SETTING_CONFIRM,
                                 .help = "The game's own saves are kept. Press twice."});
    settings_on_change(settings_save);
}

static void log_action(ct_action a, int down)
{
    if (log_input)
        fprintf(stderr, "ct_sdl: frame %ld action %s %s\n", sched_frame_count(), action_name(a),
                down ? "down" : "up");
}

/* port: a pad was added or removed: let go of what the pads held (the
   binders see the buttons still down as new presses next frame). */
static void pads_changed(void)
{
    for (int k = 0; k < 3; k++) {
        binder_release(&pad_binder[k], bindings_current());
        binder_init(&pad_binder[k]);
    }
}

/* port: gyro aiming, once a frame (settings.ini gyro=on). The 3D
   diorama's camera follows pad 1's gyro, if it has one. */
static void gyro_frame(void)
{
    if (gyro_pad != pad) {
        gyro_pad = NULL;
        gyro.tilt = gyro.yaw = 0.0f;
        if (pad && SDL_GameControllerHasSensor(pad, SDL_SENSOR_GYRO) &&
            !SDL_GameControllerSetSensorEnabled(pad, SDL_SENSOR_GYRO, SDL_TRUE)) {
            gyro_pad = pad;
            fprintf(stderr, "ct_sdl: gyro aiming with \"%s\"\n", SDL_GameControllerName(pad));
        }
        if (!gyro_pad)
            hd2d_set_camera_offset(0.0f, 0.0f);
    }
    float rate[3] = {0};
    if (!gyro_pad || SDL_GameControllerGetSensorData(gyro_pad, SDL_SENSOR_GYRO, rate, 3))
        return;
    gyro_cam_step(&gyro, rate, (float)(FRAME_NS / 1e9), gyro_strength);
    hd2d_set_camera_offset(gyro.tilt, gyro.yaw);
}

/* Window, keyboard and controller events; live_buttons for pad 1. */
static void poll_input(long f)
{
    keys.controller = pad != NULL;
    {   /* port: lay the touch overlay out; scripted touches for frame f+1 */
        int w = 0, h = 0;
        if (hd2d_mode)
            SDL_GL_GetDrawableSize(win, &w, &h);
        else
            SDL_GetRendererOutputSize(ren, &w, &h);
        touch_set_screen(win, w, h, picture_rect(w, h));
        touch_test_inject(f + 1);
    }
    hid_deck_update();   /* port: InputPlumber's Deck pad, before SDL reads it */
    {   /* port: a handheld with no controller at all: say why, once */
        static int polls, hinted;
        if (!hinted && !no_pad_hint && ++polls >= 120) {
            const char *h = device_no_pad_hint(device_kind(), pad != NULL);
            hinted = 1;
            if (h)
                osd(h);
        }
    }
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        padtest_event(&ev);   /* port: the controller test only watches */
        if (!menu_is_open() && touch_event(&ev, f))
            continue;   /* port: a touch, used by the overlay */
        /* port: QOL keys, the pause and the resume prompt; while the menu
           is open, only what it watches of the window and the pads */
        if ((!menu_is_open() || ev.type == SDL_WINDOWEVENT ||
             ev.type == SDL_CONTROLLERDEVICEREMOVED) && qol_event(&ev, f))
            continue;
        if (log_input && (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && !ev.key.repeat)
            fprintf(stderr, "ct_sdl: frame %ld key %s %s (scancode %d)\n", f,
                    SDL_GetScancodeName(ev.key.keysym.scancode),
                    ev.type == SDL_KEYDOWN ? "down" : "up", ev.key.keysym.scancode);
        if (log_input && (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP))
            fprintf(stderr, "ct_sdl: frame %ld controller button %s %s\n", f,
                    SDL_GameControllerGetStringForButton(ev.cbutton.button),
                    ev.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up");
        if (ev.type == SDL_WINDOWEVENT && (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                                           ev.window.event == SDL_WINDOWEVENT_MOVED ||
                                           ev.window.event == SDL_WINDOWEVENT_RESTORED))
            wst_track();
        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            bindings_keys_release();   /* port: no fast-forward stuck on */
            focused = 0;               /* port: mute_unfocused */
        }
        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            focused = 1;
        if (ev.type == SDL_MOUSEMOTION && ev.motion.which != SDL_TOUCH_MOUSEID) {
            mouse_moved_at = SDL_GetTicks();   /* port: the cursor shows while it moves */
            if (!cursor_shown && !game_mode) {
                SDL_ShowCursor(SDL_ENABLE);
                cursor_shown = 1;
            }
        }
        if (ev.type == SDL_CONTROLLERBUTTONDOWN) {   /* port: glyphs of the pad used */
            SDL_GameController *c = SDL_GameControllerFromInstanceID(ev.cbutton.which);
            last_pad_press = f;
            if (c)
                use_glyphs_of(c);
        } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat && f - last_pad_press > 10) {
            use_glyphs_of(NULL);   /* not a key Steam Input sent with a button */
        }
        if (ev.type == SDL_QUIT) {
            set_exit_reason("SDL_QUIT event (window closed, or quit requested by the "
                            "desktop or another program)");
            running = 0;
            continue;
        }
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_Q &&
            (ev.key.keysym.mod & KMOD_CTRL)) {
            set_exit_reason("Ctrl+Q");
            running = 0;
            continue;
        }
        if (menu_is_open()) {
            /* port: the menu has the keys, buttons, stick and mouse. A key
               or button let go still reaches the rest below (so nothing
               held when it opened stays held); device and window events
               too. Its own key (F10) closes it. */
            int up = ev.type == SDL_KEYUP || ev.type == SDL_CONTROLLERBUTTONUP;
            if (menu_event(&ev) && !up)
                continue;
            if (ev.type == SDL_KEYDOWN) {
                if (!ev.key.repeat && !menu_capturing_key() &&
                    bindings_for_key(bindings_current(), ev.key.keysym.scancode) == ACTION_MENU)
                    menu_close();
                continue;
            }
            if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERAXISMOTION)
                continue;
        }
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat && hd2d_key(ev.key.keysym.scancode)) {
            /* port: renderer controls, not game input */
        } else if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) &&
                   bindings_key_event(bindings_current(), ev.key.keysym.scancode,
                                      ev.type == SDL_KEYDOWN, ev.key.repeat)) {
            /* port: a frontend action's key (bindings.h), not game input */
        } else if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && !ev.key.repeat) {
            key_filter_key(&keys, ev.key.keysym.scancode, ev.type == SDL_KEYDOWN, f);
        } else if ((ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP) &&
                   (ev.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK ||
                    ev.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK)) {
            /* port: stick clicks (no SNES button) drive the renderer on
               handhelds without function keys: R3 preset, L3 aspect, on
               release; both together open the menu (any pad without back
               buttons has them) and then neither does its own. */
            int bit = ev.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK ? 1 : 2;
            if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                key_filter_button(&keys, f);
                stick_chord |= bit;
                if (stick_chord == 3 && !stick_chorded) {
                    stick_chorded = 1;
                    actions_emit(ACTION_MENU, 1);
                    actions_emit(ACTION_MENU, 0);
                }
            } else if (stick_chord & bit) {
                stick_chord &= ~bit;
                if (!stick_chorded && hd2d_mode)
                    hd2d_key(bit == 1 ? SDL_SCANCODE_F1 : SDL_SCANCODE_F3);
                if (!stick_chord)
                    stick_chorded = 0;
            }
        } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
            key_filter_button(&keys, f);
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) {
            pad = open_controller();
            pads_changed();   /* port */
            if (pad) {
                fprintf(stderr, "ct_sdl: using \"%s\" on pad 1\n", SDL_GameControllerName(pad));
                use_glyphs_of(pad);
            }
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {   /* port: pads 2 and 3 */
            if (!pad2)
                pad2 = open_controller_except2(pad, pad3);
            else if (!pad3)
                pad3 = open_controller_except2(pad, pad2);
            pads_changed();
        } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            pads_changed();   /* port: before the pads move up */
            rumble_out_stop_all();   /* port: every pad still; the next frame reassigns them */
            if (gyro_pad && !SDL_GameControllerGetAttached(gyro_pad))
                gyro_pad = NULL;
            SDL_GameController **all[3] = {&pad, &pad2, &pad3};
            for (int k = 0; k < 3; k++)
                if (*all[k] && !SDL_GameControllerGetAttached(*all[k])) {
                    SDL_GameControllerClose(*all[k]);
                    *all[k] = NULL;
                }
            if (!pad) {   /* port: the next one takes over pad 1 */
                pad = pad2;
                pad2 = pad3;
                pad3 = NULL;
            }
            if (!pad)
                pad = open_controller();
        }
    }
    {   /* port: back buttons / grips -> frontend actions, on any pad */
        SDL_GameController *all[3] = {pad, pad2, pad3};
        for (int k = 0; k < 3; k++)
            if (all[k])
                binder_update(&pad_binder[k], bindings_current(), pad_button, all[k]);
    }
    if (cursor_shown && SDL_GetTicks() - mouse_moved_at > CURSOR_IDLE_MS) {
        SDL_ShowCursor(SDL_DISABLE);   /* port: hidden while the mouse is still */
        cursor_shown = 0;
    }
    if (menu_is_open())
        return;   /* port: the game waits (menu_run); nothing else to read */
    if (gyro_on && hd2d_mode)   /* port: off by default */
        gyro_frame();
    coop_update();   /* port */
    SDL_GameController *p1 = pad, *p2 = coop_players > 1 ? pad2 : NULL;
    SDL_GameController *p3 = coop_players > 2 ? pad3 : NULL;
    uint16_t buttons = input_keyboard(keys.held);
    live_buttons2 = p2 ? input_controller(pad_button, pad_axis, p2) : 0;   /* port: co-op */
    live_buttons3 = p3 ? input_controller(pad_button, pad_axis, p3) : 0;
    if (p1) {   /* port: the quit combo first: it can keep the pad from the game */
        int q = quit_combo_update(&quitc, f, pad_button, p1);
        if (q == QUIT_ASK) {   /* port: both pictures show the question (osd_text.h) */
            fprintf(stderr, "ct_sdl: View+Menu held 2 s: asking to quit\n");
            quit_asked = f;
        } else if (q == QUIT_YES) {
            fprintf(stderr, "ct_sdl: quit confirmed with A\n");
            set_exit_reason("View+Menu held 2 s, then A");
            running = 0;
        } else if (q == QUIT_CANCEL) {
            fprintf(stderr, "ct_sdl: quit cancelled\n");
            quit_asked = -1;
            osd_show("Back to the game");
        }
        /* The question stays up (a message lasts about 2 s) until answered. */
        if (quit_asked >= 0 && running && (f - quit_asked) % 90 == 0)
            osd_show("Quit? [a] quit  [b] back to the game");
        if (!quit_combo_masks(&quitc))
            buttons |= input_controller(pad_button, pad_axis, p1);
    }
    if (menu_hold_mask) {   /* port: what closed the menu doesn't press anything in the game */
        if (buttons)
            buttons = 0;
        else
            menu_hold_mask = 0;
    }
    buttons |= touch_buttons(f);   /* port */
    live_buttons = buttons;
}

/* Pad 1 for frame f: live input plus the --script, recorded. port: pad 2
   (co-op) and the script's "pN" pads likewise. */
static void set_input(long f)
{
    uint16_t b = qol_buttons(0, live_buttons, f) | replay_buttons(&script, f);   /* port: QOL turbo */
    sched_set_joypad(0, b);
    replay_rec_frame(&rec, f, b);
    for (int p = 1; p < REPLAY_PORTS; p++) {
        uint16_t bp = (uint16_t)(replay_port_buttons(&script, p, f) |
                                 qol_buttons(p, p == 1 ? live_buttons2 : p == 2 ? live_buttons3 : 0, f));
        sched_set_joypad(p, bp);
        if (bp || p == 1)
            replay_rec_port(&rec, p, f, bp);
    }
}

/* port: audio packs (music.h): the game module's music events of this
   frame to the player, and the console's music voices muted while a
   replacement plays (the sound effects' voices stay). The module starts
   watching the APU ports once there is a song to replace or a log to
   write. A jump in the frame count (a save state loaded) stops the
   replacement: the game's song may be another one now. */
static void music_frame(long f)
{
    static int watching;
    static long last = -1;
    if (!watching && (music_count() || log_music) && ct_music_source()) {
        ct_music_watch(1);
        watching = 1;
    }
    if (!watching)
        return;
    long now = sched_frame_count();
    if (last >= 0 && now != last + 1)
        music_stop();
    last = now;
    ct_music_event ev[16];
    int n = ct_music_events(ev, 16);
    for (int k = 0; k < n; k++)
        music_event(&ev[k], f);
    const ct_music_hooks *h = ct_music_source();
    sched_set_voice_mute(music_replacing() ? 0xFF : 0, h ? h->sfx_voices_aram : -1);
}

/* This frame's DSP output through the resampler into the SDL queue, with
   rate control toward AUDIO_TARGET. */
static void queue_audio(long f)
{
    static int16_t in[4096 * 2], out[8192 * 2];
    int n_in = sched_audio_take(in, 4096);
    rumble_frame(f, in, n_in, SCHED_AUDIO_HZ);   /* port: simulated rumble: the frame's events */
    if (!audio || qol_audio_muted())
        return;   /* port: QOL fast-forward drops the sound */
    unsigned level = SDL_GetQueuedAudioSize(audio) / 4;
    if (playing && !level)
        audio_underruns++;
    level_avg += 0.05 * (level - level_avg);
    double adjust = rs_rate_control(level_avg, AUDIO_TARGET, AUDIO_MAX_DEV);
    rs_set_adjust(&rs, adjust);
    int n_out = rs_process(&rs, in, n_in, out, 8192);
    if (n_out > 0)
        music_mix(out, n_out);   /* port: audio packs */
    if (log_audio)
        fprintf(stderr, "ct_sdl: frame %ld audio queued %u avg %.0f rate x%.4f in %d out %d%s\n",
                f, level, level_avg, adjust, n_in, n_out, playing && !level ? " UNDERRUN" : "");
    /* port: the volume, and silence in the background (mute_unfocused):
       the samples are scaled, not dropped, so the queue and its rate
       control carry on as before. */
    int vol = mute_run || (mute_unfocused && !focused) ? 0 : volume;
    if (vol < 100)
        for (int k = 0; k < n_out * 2; k++)
            out[k] = (int16_t)(out[k] * vol / 100);
    if (level > 4 * AUDIO_TARGET)
        audio_dropped++;   /* far ahead (--fast, or the device stalled) */
    else if (n_out > 0)
        SDL_QueueAudio(audio, out, (uint32_t)n_out * 4);
    if (!playing && SDL_GetQueuedAudioSize(audio) / 4 >= AUDIO_TARGET) {
        SDL_PauseAudioDevice(audio, 0);
        playing = 1;
    }
}

/* port: sleep until the performance counter reaches t. SDL_Delay counts
   whole milliseconds, and up to a millisecond of jitter shows on a
   variable-refresh display; nanosleep wakes within tens of microseconds. */
static void sleep_until(uint64_t t)
{
    uint64_t now = SDL_GetPerformanceCounter();
    if (now >= t)
        return;
    uint64_t ns = (t - now) * 1000000000ull / perf_freq;
#if defined(__unix__) || defined(__APPLE__)
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts))
        ;
#else
    SDL_Delay((uint32_t)(ns / 1000000));
#endif
}

static void set_vsync(int on)
{
    if (hd2d_mode)
        SDL_GL_SetSwapInterval(on);
    else if (ren)
        SDL_RenderSetVSync(ren, on);
}

/* port: one frame edge's interval into the statistics; a vsync that
   doesn't hold the frame rate near 60 Hz gives way to the timer. */
static void pace_frame(void)
{
    uint64_t now = SDL_GetPerformanceCounter();
    if (pace.last) {
        double ms = (double)(now - pace.last) * 1000.0 / (double)perf_freq;
        if (!pace.n || ms < pace.min)
            pace.min = ms;
        if (!pace.n || ms > pace.max)
            pace.max = ms;
        if (!pace.wn || ms < pace.wmin)
            pace.wmin = ms;
        if (!pace.wn || ms > pace.wmax)
            pace.wmax = ms;
        pace.n++, pace.wn++;
        pace.sum += ms, pace.wsum += ms;
        if (ms > 25.0)
            pace.late++, pace.wlate++;
        if (pace.wn == 60) {
            if (log_pacing)
                pace_print("60", pace.wn, pace.wsum, pace.wmin, pace.wmax, pace.wlate);
            double mean = pace.wsum / pace.wn;
            static long catchups_seen;
            /* Not on the first window (start-up) or one with catch-up
               frames, which run fast. */
            int caught_up = pace_catchups != catchups_seen || pace.n == 60;
            catchups_seen = pace_catchups;
            if (pace_vsync && !caught_up && (mean < 16.2 || mean > 17.4)) {
                fprintf(stderr, "ct_sdl: pacing: vsync gives %.2f ms frames, not ~16.67: "
                                "using the timer\n", mean);
                set_vsync(0);
                pace_vsync = pace_catchup = 0;
            }
            pace.wn = pace.wlate = 0;
            pace.wsum = 0;
        }
    }
    pace.last = now;
}

/* port: vsync or the timer (--pacing), once the window is open. */
static void choose_pacing(void)
{
    SDL_DisplayMode m = {0};
    int idx = win ? SDL_GetWindowDisplayIndex(win) : -1;
    if (idx < 0 || SDL_GetCurrentDisplayMode(idx, &m))
        m.refresh_rate = 0;
    /* auto is the timer for now: vsync (--pacing vsync) loses a frame for
       every missed refresh and catches up only through the sound queue
       check in on_frame, and it is not yet measured on a real Game Mode
       display. */
    int want = !fast && pace_opt == PACE_VSYNC;
#ifdef CT_PS5_GL
    /* port: the PS5's GPU build tears without vsync (seen on a TV); its
       output is a steady 60 Hz, so auto means vsync there */
    want |= !fast && pace_opt == PACE_AUTO;
#endif
    int ok = 0;
    if (want)
        ok = hd2d_mode ? !SDL_GL_SetSwapInterval(1) : ren && !SDL_RenderSetVSync(ren, 1);
    pace_vsync = ok;
    if (!ok)
        set_vsync(0);
    fprintf(stderr, "ct_sdl: display %dx%d at %d Hz: pacing by %s%s\n", m.w, m.h,
            m.refresh_rate, ok ? "vsync" : "the 60.0988 Hz timer",
            want && !ok ? " (vsync unavailable)" : "");
}

/* Frame edge (sched_set_frame_hook): frame f is complete. Present it and
   its audio, wait for its time slot, then read input for frame f+1. Runs
   at the same point whether the CPU is interpreted or inside a native
   function, so a native call spanning frames can't bunch them up. */
/* port: draw this frame (also while paused: QOL). */
static void present_now(void)
{
    if (hd2d_mode)
        present_hd2d();
    else
        present_plain(turn_tag(sched_frame()));   /* port: with the touch overlay, the co-op tag */
}

/* port: --hash-log: frame f's WRAM and picture, FNV-1a 64. */
static uint64_t fnv64(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t k = 0; k < n; k++)
        h = (h ^ p[k]) * 0x100000001B3ull;
    return h;
}

static void write_hash(long f)
{
    uint64_t h = fnv64(0xCBF29CE484222325ull, bus_wram(), 0x20000);
    h = fnv64(h, sched_frame(), (size_t)SCHED_WIDTH * SCHED_HEIGHT * 4);
    fprintf(hash_log, "%ld %016llx\n", f, (unsigned long long)h);
}

/* port: --menu-test FILE, a script of menu use for tests. One command a
   line ('#' starts a comment):
     open F         the menu action (as F10 or R5) at the end of frame F
     button NAME    a controller button (SDL's name: a, b, dpdown,
                    leftshoulder, start, ...) pressed this tick, let go
                    the next
     key NAME       a key likewise (SDL's name: Return, Escape, Down, F10)
     axis NAME V    a stick axis (leftx, lefty) moved to V
     wait N         N ticks with nothing
     shot PATH      the next tick's picture, menu and all, as PNG
     click X Y      the left mouse button (or a touch) at X, Y in the
                    window, let go the next tick
     glyphs NAME    show the buttons of a controller family (steamdeck,
                    xbox, playstation, switch, keyboard, ...), as if used
   After "open", one command per tick of the open menu; once the script
   runs out (or reaches the next "open") with the menu still open, it
   closes after 30 ticks. */
enum { MT_OPEN, MT_BUTTON, MT_KEY, MT_AXIS, MT_WAIT, MT_SHOT, MT_GLYPHS, MT_CLICK };

/* A glyph set by name, spaces and case aside ("steamdeck", "xbox"). */
static int glyphs_parse(const char *s)
{
    for (int k = 0; k < GLYPHS_SETS; k++) {
        const char *n = glyphs_family_name(k), *p = s;
        for (; *n; n++)
            if (*n != ' ' && (*n | 32) != (*p++ | 32))
                break;
        if (!*n && !*p)
            return k;
    }
    return -1;
}
static int mt_pos, mt_wait, mt_idle;
static int mt_up_button = -1, mt_up_mouse;
static int mt_mouse_x, mt_mouse_y;

/* The left mouse button at (x, y) in window pixels (down), or let go where it was. */
static void push_mouse(int x, int y, int down)
{
    SDL_Event e;
    if (down) {
        mt_mouse_x = x, mt_mouse_y = y;
        memset(&e, 0, sizeof e);
        e.type = SDL_MOUSEMOTION;
        e.motion.x = x, e.motion.y = y;
        e.motion.windowID = SDL_GetWindowID(win);
        SDL_PushEvent(&e);
    }
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.clicks = 1;
    e.button.x = mt_mouse_x, e.button.y = mt_mouse_y;
    e.button.windowID = SDL_GetWindowID(win);
    SDL_PushEvent(&e);
}
static SDL_Scancode mt_up_key;

static int menu_test_load(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "ct_sdl: cannot read --menu-test %s\n", path);
        return -1;
    }
    char line[300], cmd[16], arg[200];
    int n = 0, bad = 0;
    while (fgets(line, sizeof line, fp) && !bad) {
        n++;
        line[strcspn(line, "#\r\n")] = 0;
        int v = 0;
        int got = sscanf(line, "%15s %199s %d", cmd, arg, &v);
        if (got < 1)
            continue;
        if (mtest.n == MAX_MENU_TEST) {
            bad = 1;
            break;
        }
        struct mt_ev *e = &mtest.ev[mtest.n];
        memset(e, 0, sizeof *e);
        if (!strcmp(cmd, "open") && got >= 2)
            e->kind = MT_OPEN, e->a = atoi(arg);
        else if (!strcmp(cmd, "button") && got >= 2 &&
                 (e->a = SDL_GameControllerGetButtonFromString(arg)) != SDL_CONTROLLER_BUTTON_INVALID)
            e->kind = MT_BUTTON;
        else if (!strcmp(cmd, "key") && got >= 2 &&
                 (e->a = SDL_GetScancodeFromName(arg)) != SDL_SCANCODE_UNKNOWN)
            e->kind = MT_KEY;
        else if (!strcmp(cmd, "axis") && got == 3 &&
                 (e->a = SDL_GameControllerGetAxisFromString(arg)) != SDL_CONTROLLER_AXIS_INVALID)
            e->kind = MT_AXIS, snprintf(e->path, sizeof e->path, "%d", v);
        else if (!strcmp(cmd, "wait") && got >= 2)
            e->kind = MT_WAIT, e->a = atoi(arg);
        else if (!strcmp(cmd, "click") && got == 3)
            e->kind = MT_CLICK, e->a = atoi(arg), snprintf(e->path, sizeof e->path, "%d", v);
        else if (!strcmp(cmd, "glyphs") && got >= 2 && (e->a = glyphs_parse(arg)) >= 0)
            e->kind = MT_GLYPHS;
        else if (!strcmp(cmd, "shot") && got >= 2)
            e->kind = MT_SHOT, snprintf(e->path, sizeof e->path, "%s", arg);
        else
            bad = 1;
        if (!bad)
            mtest.n++;
    }
    fclose(fp);
    if (bad)
        fprintf(stderr, "ct_sdl: --menu-test %s: bad line %d\n", path, n);
    return bad ? -1 : 0;
}

static void push_button(int button, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    e.cbutton.which = -2;   /* no real pad */
    e.cbutton.button = (uint8_t)button;
    e.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
    SDL_PushEvent(&e);
}

static void push_key(SDL_Scancode sc, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    SDL_PushEvent(&e);
}

/* At frame f's edge: the script's "open". */
static void menu_test_frame(long f)
{
    if (mt_pos < mtest.n && mtest.ev[mt_pos].kind == MT_OPEN && f >= mtest.ev[mt_pos].a) {
        fprintf(stderr, "ct_sdl: menu-test: frame %ld: menu action\n", f);
        mt_pos++;
        mt_idle = 0;
        actions_emit(ACTION_MENU, 1);
        actions_emit(ACTION_MENU, 0);
    }
}

/* Each tick of the open menu: the next command. */
static void menu_test_tick(void)
{
    if (mt_up_button >= 0)
        push_button(mt_up_button, 0), mt_up_button = -1;
    if (mt_up_mouse)
        push_mouse(0, 0, 0), mt_up_mouse = 0;
    if (mt_up_key)
        push_key(mt_up_key, 0), mt_up_key = 0;
    if (!mtest.n)
        return;
    if (mt_wait > 0) {
        mt_wait--;
        return;
    }
    if (mt_pos >= mtest.n || mtest.ev[mt_pos].kind == MT_OPEN) {
        if (++mt_idle > 30) {
            fprintf(stderr, "ct_sdl: menu-test: the script left the menu open: closing it\n");
            menu_close();
        }
        return;
    }
    struct mt_ev *e = &mtest.ev[mt_pos++];
    switch (e->kind) {
    case MT_BUTTON: push_button(e->a, 1), mt_up_button = e->a; break;
    case MT_KEY: push_key((SDL_Scancode)e->a, 1), mt_up_key = (SDL_Scancode)e->a; break;
    case MT_AXIS: {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = SDL_CONTROLLERAXISMOTION;
        ev.caxis.which = -2;
        ev.caxis.axis = (uint8_t)e->a;
        ev.caxis.value = (int16_t)atoi(e->path);
        SDL_PushEvent(&ev);
        break;
    }
    case MT_WAIT: mt_wait = e->a > 0 ? e->a - 1 : 0; break;
    case MT_SHOT: menu_shot_path = e->path; break;
    case MT_GLYPHS: glyphs_use(e->a); break;
    case MT_CLICK: push_mouse(e->a, atoi(e->path), 1), mt_up_mouse = 1; break;
    default: break;
    }
}

/* port: the menu is open (menu.h): the game waits between two frames
   (main's loop; the frame the menu action came in has had its input set),
   the sound is paused, and the picture is drawn again each tick with the
   menu over it until it closes. No frame runs, so the game's state, its
   frame count and the hashes are as they were, unless a save state is
   loaded from it. Between frames is where QOL takes and loads states:
   qol_after_frame each tick carries out what the menu asked for. */
static void menu_run(long f)
{
    long frames_at = sched_frame_count();
    long ticks = 0;
    int loaded = 0;
    fprintf(stderr, "ct_sdl: frame %ld: menu open\n", f);
    if (audio)
        SDL_PauseAudioDevice(audio, 1);
    bindings_keys_release();   /* fast-forward and the like: not held through it */
    qol_ffwd_set(0);
    uint64_t last = SDL_GetPerformanceCounter();
    while (running && menu_is_open()) {
        menu_test_tick();
        poll_input(sched_frame_count());
        uint64_t now = SDL_GetPerformanceCounter();
        unsigned ms = fast ? 16u : (unsigned)((now - last) * 1000 / perf_freq);
        last = now;
        menu_tick(ms);
        rumble_out_idle(SDL_GetTicks());   /* port: no game rumble in the menu (its test only) */
        long before = sched_frame_count();
        qol_after_frame();   /* QOL: a save, load or screenshot asked for in the menu */
        loaded |= sched_frame_count() != before;
        if (!running || !menu_is_open())
            break;
        present_now();
        ticks++;
        if (!fast) {   /* about 60 ticks a second */
            uint64_t end = last + perf_freq / 60;
            if (SDL_GetPerformanceCounter() < end)
                sleep_until(end);
        }
    }
    menu_close();
    menu_ticks_total += ticks;
    if (!loaded)
        menu_frames_advanced += sched_frame_count() - frames_at;
    fprintf(stderr, "ct_sdl: frame %ld: menu closed after %ld ticks; frame count %ld (was %ld)%s\n",
            f, ticks, sched_frame_count(), frames_at, loaded ? ", a state loaded" : "");
    if (audio && playing)
        SDL_PauseAudioDevice(audio, 0);
    menu_hold_mask = 1;
    next_frame = SDL_GetPerformanceCounter();   /* the frame timer starts over */
    pace.last = 0;
    if (hd2d_mode)   /* a look changed in the menu: the PPU's behind buffer to match */
        sched_set_behind(hd2d_wants_behind());
}

static void menu_report(void)
{
    if (menu_opened_count())
        fprintf(stderr, "ct_sdl: menu: opened %ld times, %ld ticks, %ld frames ran while open\n",
                menu_opened_count(), menu_ticks_total, menu_frames_advanced);
}

static void on_frame(long f)
{
    exit_frame = f;
    if (f == poke_frame)   /* port: --poke-wram, a research aid (as ct_boot's) */
        bus_wram()[poke_addr] ^= poke_val;
    if (f >= set_from && f <= set_to)   /* port: --set-wram */
        bus_wram()[set_addr] = set_val;
    if (hash_log)   /* port */
        write_hash(f);
    int show = qol_frame(f);   /* port: QOL: fast-forward and the battery's 30 Hz skip some */
    music_frame(f);   /* port: audio packs */
    queue_audio(f);
    {   /* port: simulated rumble on the pads in play; none while fast-forwarding */
        SDL_GameController *const pads[RUMBLE_PADS] = {
            pad, coop_players > 1 ? pad2 : NULL, coop_players > 2 ? pad3 : NULL, NULL};
        rumble_out_set_pads(pads);
        rumble_out_frame(SDL_GetTicks(), qol_speed() > 1 || qol_paused() || menu_is_open());
        if (f == rumble_test_at) {
            fprintf(stderr, "ct_sdl: frame %ld: rumble test pulse (pad 1: %s)\n", f,
                    pad ? SDL_GameControllerName(pad) : "none");
            rumble_out_test(SDL_GetTicks());
        }
    }
    if (pace_vsync && playing && audio && !fast) {
        /* port: a frame that missed its refresh is lost under vsync, and the
           sound queue drains by a frame each time: when it runs low, stop
           waiting for vsync until the queue is back near its target. */
        unsigned lvl = SDL_GetQueuedAudioSize(audio) / 4;
        if (!pace_catchup && lvl < 2 * AUDIO_BUFFER) {   /* two device buffers from empty */
            pace_catchup = 1;
            pace_catchups++;
            set_vsync(0);
        } else if (pace_catchup && lvl >= AUDIO_TARGET) {
            pace_catchup = 0;
            set_vsync(1);
        }
    }
    const uint8_t *fb = sched_frame();   /* B G R x = ARGB8888 little-endian */
    {   /* port: split screen, the plain path only */
        int was = split_on;
        split_on = !hd2d_mode && sched_views() > 1 && sched_render_view2(view2_px);
        if (split_on != was)
            fprintf(stderr, "ct_sdl: frame %ld: split screen %s\n", f, split_on ? "on" : "off");
    }
    {   /* port: co-op: whose turn, on screen in HD-2D (the plain picture has the tag) */
        static int last_turn = -1;
        int t = coop_players > 1 ? sched_turn_player() : -1;
        if (t != last_turn && t > 0 && hd2d_mode) {
            char msg[32];
            snprintf(msg, sizeof msg, "P%d's turn", t + 1);
            hd2d_osd(msg);
        }
        last_turn = t;
    }
    /* port: asset packs' poses (pack_scene.h): the objects' at this edge */
    pack_scene_frame(pack_scene_wants_poses() ? scene_capture_now() : NULL);
    if (hd2d_mode && show) {
        /* port: this frame's asset pack replacements */
        if (packs_active() && sched_prov()) {
            uint64_t t0 = SDL_GetPerformanceCounter();
            pack_scene_map_poses();
            pack_map_pixels += packs_map_frame(sched_prov(), sched_prov_mem(), pack_map);
            pack_map_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                           (double)SDL_GetPerformanceFrequency();
            pack_map_frames++;
            hd2d_pack_map(pack_map);
        } else {
            hd2d_pack_map(NULL);
        }
    }
    if (hd2d_mode) {
        /* port: HD text: the text boxes' lines, the console's glyphs out,
           the overlay at the screen's scale; every frame, shown or not, as
           a screenshot may draw it */
        memset(&hd_text_now, 0, sizeof hd_text_now);
        if (hdtext_ready() && sched_prov()) {
            static ct_text txt;
            int ww = 0, wh = 0;
            if (win)
                SDL_GL_GetDrawableSize(win, &ww, &wh);
            wh = wh > shot_h ? wh : shot_h;
            int scale = (wh + SCHED_HEIGHT - 1) / SCHED_HEIGHT;
            const sched_ppu_state *ps = sched_ppu_state_get();
            int bright = ps ? ps->brightness : 15;
            if (!ct_text_capture(&txt))
                hdtext_frame(&txt, sched_prov(), sched_prov_mem(), bright, sched_frame(),
                             sched_layers(), scale, &hd_text_now);
        }
        hd2d_set_text_overlay(hd_text_now.rgba, hd_text_now.w, hd_text_now.h, hd_text_now.x,
                              hd_text_now.y, hd_text_now.cw, hd_text_now.ch, hd_text_now.gen);
    }
    if (show)
        present_now();
    if (hd2d_mode)   /* The PPU's behind G-buffer only while the look uses it. */
        sched_set_behind(hd2d_wants_behind());
    if (n_shots)
        take_shots(f);
    if (pack_dump)   /* port: new tiles and sprites into the pack */
        packs_dump_frame(f, sched_prov(), sched_prov_mem());
    if (dump)
        png_dump_frame(dump, f, fb, SCHED_WIDTH, SCHED_HEIGHT);
    if (!fast) {   /* port: QOL fast-forward: qol_speed() frames per frame time */
        next_frame += perf_freq * FRAME_NS / 1000000000ull / (unsigned)qol_speed();
        uint64_t now = SDL_GetPerformanceCounter();
        if (pace_vsync) {
            /* port: the swap above waited for the display; the timer only
               keeps a vsync that stops waiting (a minimized window, a
               compositor change) from running the game fast. */
            /* Behind (a 60.000 Hz display, or a late frame): no sleep, and
               a resync only past 100 ms, as for the timer, so a vsync that
               doesn't wait still catches up after a slow frame. */
            if (now + perf_freq / 1000 < next_frame - perf_freq * FRAME_NS / 1000000000ull)
                sleep_until(next_frame - perf_freq * FRAME_NS / 1000000000ull);
            else if (now > next_frame + perf_freq / 10)
                next_frame = now;
        } else if (now < next_frame) {
            sleep_until(next_frame);
        } else if (now - next_frame > perf_freq / 10) {
            next_frame = now;   /* more than 100 ms behind: resync, don't sprint */
        }
        pace_frame();
    }
    saves_frame(f);   /* port */
    poll_input(f);
    menu_test_frame(f);   /* port: --menu-test (the menu opens after this frame: main) */
    set_input(f + 1);
}

static void close_record(void)
{
    replay_rec_close(&rec);
}

/* The script's reset button (replay.h "reset F"). */
static int script_reset(long frame)
{
    return replay_reset(&script, frame);
}

/* port: the window, plain or HD-2D (moved out of main so the first-run
   screens can open it before the ROM is known). Returns 0, or 1 with the
   reason set. */
static int open_window(const char *title, int scale, int fullscreen)
{
    /* port: under gamescope (Steam Game Mode, handheld PCs) the
       window is the whole screen anyway: start fullscreen. */
    if (game_mode)
        fullscreen = 1;
#ifdef CT_PS5
    else if (1)
        fullscreen = 1;   /* port: a console: the whole screen, always */
#endif
    else if (!fullscreen && wst.fs)
        fullscreen = 1;   /* port: as it was left */
    int ww = hd2d_mode ? SCHED_WIDTH * scale * 4 / 3 : SCHED_WIDTH * scale, wh = SCHED_HEIGHT * scale;
    int wx = SDL_WINDOWPOS_CENTERED, wy = SDL_WINDOWPOS_CENTERED;
    if (!game_mode && !wst.scale_given && wst.have_size) {
        ww = wst.w;
        wh = wst.h;
        if (wst.have_pos && wst_on_screen(wst.x, wst.y, ww)) {
            wx = wst.x;
            wy = wst.y;
        }
    }
    Uint32 wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                    (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (hd2d_mode) {
        /* OpenGL 3.3 core, else OpenGL ES 3.0. */
        static const struct { int profile, major, minor, gles; } want_gl[] = {
            {SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, 0},
            {SDL_GL_CONTEXT_PROFILE_ES, 3, 0, 1},
        };
        /* port: CT_HD2D_GLES=1 asks for OpenGL ES 3.0 only (tests the ES path
           on a desktop driver). */
        const char *es_only = getenv("CT_HD2D_GLES");
        for (size_t k = es_only && *es_only == '1' ? 1 : 0;
             k < sizeof want_gl / sizeof *want_gl && !glctx; k++) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, want_gl[k].profile);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, want_gl[k].major);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, want_gl[k].minor);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            win = SDL_CreateWindow(title, wx, wy, ww, wh,
                                   wflags | SDL_WINDOW_OPENGL);
            glctx = win ? SDL_GL_CreateContext(win) : NULL;
            if (glctx && hd2d_init(want_gl[k].gles)) {
                SDL_GL_DeleteContext(glctx);
                glctx = NULL;
            }
            if (glctx)
                gl_es = want_gl[k].gles;
            if (glctx)   /* port: the touch overlay's GL drawer */
                touch_gl_ok = !touch_gl_init(want_gl[k].gles);
            if (!glctx && win) {
                SDL_DestroyWindow(win);
                win = NULL;
            }
        }
        if (!glctx) {
            fprintf(stderr, "ct_sdl: no usable OpenGL 3.3 / ES 3.0 context: %s\n", SDL_GetError());
#ifdef CT_PS5
            /* port: the PS5 has OpenGL only through an OSMesa library the
               player adds (docs/PS5.md): without it, the plain picture. */
            fprintf(stderr, "ct_sdl: PS5: falling back to the original renderer\n");
            hd2d_mode = 0;
            return open_window(title, scale, fullscreen);
#else
            set_exit_reason("HD-2D renderer setup failed");
            return 1;
#endif
        }
        SDL_GL_SetSwapInterval(0);   /* paced by the frame timer; choose_pacing may change it */
        /* port: the Steam Deck's APU gets the lighter look, unless the
           user saved or asked for one. */
        const char *gl = NULL;
        int deck = device_gl_is_deck(&gl) || device_kind() == DEVICE_STEAM_DECK;
        fprintf(stderr, "ct_sdl: GL renderer \"%s\", device \"%s\"%s\n", gl,
                device_product(), deck ? ": Steam Deck" : "");
        if (deck && !preset_saved && !preset_given) {
            hd2d_set_preset(HD2D_DECK);
            fprintf(stderr, "ct_sdl: hd2d preset deck (Steam Deck default)\n");
        }
    } else {
        win = SDL_CreateWindow(title, wx, wy, ww, wh, wflags);
#ifdef CT_PS5
        /* port: the PS5's one display (SDL's PS5 driver, 1920x1080), each
           step logged: an installed title can refuse what a payload gets. */
        if (!win) {
            fprintf(stderr, "ct_sdl: PS5: window (flags 0x%x): %s\n", (unsigned)wflags,
                    SDL_GetError());
            win = SDL_CreateWindow(title, 0, 0, 1920, 1080, SDL_WINDOW_SHOWN);
            if (!win)
                fprintf(stderr, "ct_sdl: PS5: window 1920x1080: %s\n", SDL_GetError());
        }
#endif
        ren = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
#ifdef CT_PS5
        if (win && !ren)
            fprintf(stderr, "ct_sdl: PS5: renderer: %s\n", SDL_GetError());
#endif
        if (win && !ren)
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
#ifdef CT_PS5
        if (win && !ren) {   /* the window's own surface, SDL's software renderer on it */
            fprintf(stderr, "ct_sdl: PS5: software renderer: %s\n", SDL_GetError());
            SDL_Surface *ws = SDL_GetWindowSurface(win);
            if (!ws)
                fprintf(stderr, "ct_sdl: PS5: window surface: %s\n", SDL_GetError());
            ren = ws ? SDL_CreateSoftwareRenderer(ws) : NULL;
            if (!ren && ws)
                fprintf(stderr, "ct_sdl: PS5: renderer on the surface: %s\n", SDL_GetError());
        }
#endif
        tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                                   SDL_TEXTUREACCESS_STREAMING, SCHED_WIDTH,
                                                   SCHED_HEIGHT) : NULL;
        tex2 = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,   /* port: split */
                                       SDL_TEXTUREACCESS_STREAMING, SCHED_WIDTH, SCHED_HEIGHT)
                   : NULL;
        if (!tex) {
            fprintf(stderr, "ct_sdl: window: %s (window %s, renderer %s)\n", SDL_GetError(),
                    win ? "ok" : "none", ren ? "ok" : "none");
            set_exit_reason("window or renderer creation failed");
            return 1;
        }
        /* port: integer scaling is done by present_plain (touch.h) */
    }
    if (game_mode)   /* port: nothing to point at on a handheld */
        SDL_ShowCursor(SDL_DISABLE);
    wst_track();
    wst.w0 = wst.w, wst.h0 = wst.h, wst.x0 = wst.x, wst.y0 = wst.y;
    wst.fs0 = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN ? 1 : 0;
    if (game_mode)
        wst.fs0 = 1;
    {
        int w = 0, h = 0;
        if (hd2d_mode)
            SDL_GL_GetDrawableSize(win, &w, &h);
        else
            SDL_GetRendererOutputSize(ren, &w, &h);
        fprintf(stderr, "ct_sdl: window %dx%d%s\n", w, h,
                SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN ? ", fullscreen" : "");
    }
    choose_pacing();
    {   /* port: the menu draws over this window (set up when first opened) */
        menu_host mh = {win, hd2d_mode ? NULL : ren, gl_es, APP_NAME, CT_VERSION};
        menu_init(&mh);
        menu_ready = 1;
        cursor_shown = !game_mode;
        mouse_moved_at = SDL_GetTicks();
    }
    return 0;
}

#ifdef CT_FIRSTRUN
/* port: the first-run screens: frames of text, no layer information. */
static uint8_t status_layers[SCHED_WIDTH * SCHED_HEIGHT];

static void present_status(const uint8_t *bgrx)
{
    if (hd2d_mode) {
        int w, h;
        SDL_GL_GetDrawableSize(win, &w, &h);
        hd2d_set_scene(NULL);
        hd2d_render(bgrx, status_layers, NULL, NULL, NULL, w, h, 0);
        SDL_GL_SwapWindow(win);
    } else {
        present_plain(bgrx);
    }
}

/* PATH may hold one %d: numbered shots, one per change, from 0. */
static void shot_status(const uint8_t *bgrx, const char *path)
{
    static int seq;
    char name[4096];
    const char *pct = strchr(path, '%');
    if (pct && pct[1] == 'd' && !strchr(pct + 1, '%'))
        snprintf(name, sizeof name, "%.*s%d%s", (int)(pct - path), path, seq++, pct + 2);
    else
        snprintf(name, sizeof name, "%s", path);
    path = name;
    int ok = write_shot(path, bgrx, status_layers, NULL, NULL, NULL);
    fprintf(stderr, "ct_sdl: setup screen: %s %s\n", ok ? "wrote" : "cannot write", path);
}
#endif

/* port: the options, and the keys (--help prints both). */
static void usage(FILE *out)
{
    fprintf(out, "usage: ct_sdl [--scale N] [--frames N] [--dump DIR]\n         "
            "[--needed-hw FILE] [--fast] [--require-render] [--log-input]\n         "
            "[--interp-only] [--record FILE] [--script FILE] [--log-audio] [--music-log] [--hd-font FILE]\n         "
            "[--require-no-underrun] [--hd2d] [--no-hd2d]\n         "
            "[--preset off|subtle|hd2d|deck|hd2d3d|deck3d] [--render-scale 50-100|auto]\n         "
            "[--sharpen 0-1|auto] [--ao 0-1|auto]\n         "
            "[--aspect 4:3|pixel|stretch] [--debug-view N] [--fullscreen]\n         "
            "[--log-gpu] [--shot F:PATH]... [--shot-size WxH] [--module FILE]\n         "
            "[--rom FILE] [--touch] [--touch-hide SECS] [--touch-test FILE]\n         "
            "[--save FILE] [--no-save] [--dump-sram FILE]\n         "
            "[--poke-sram F:ADDR=VAL]... [--pacing auto|timer|vsync] [--log-pacing]\n         "
            "[--coop] [--no-coop] [--coop-split]"
            " "
            "[--pack-dump NAME] [--pack NAME|none]...\n         "
            "[--pack-export-sheets NAME] [--pack-export-scale N]\n         "
            "[--mute] [--no-mute] [--pad-test] [--rumble off|light|medium|strong] [--log-rumble] [--rumble-test F]\n         "
            "[--menu-test FILE] [--hash-log FILE]"
#ifdef CT_FIRSTRUN
            " [--data-dir DIR] [--game ID] [--shot status:PATH] [--build-only]"
#endif
            "\n");
    fputs("\n"
          "Keyboard (window)\n"
          "  Arrow keys        d-pad            Enter       Start\n"
          "  X                 A                Right Shift Select\n"
          "  Z                 B                Q / W       L / R\n"
          "  A / S             Y / X\n"
          "  F11               fullscreen on / off\n"
          "  F1                HD-2D look: original, subtle, HD-2D, Steam Deck, 3D\n"
          "  F2                HD-2D debug view      F3  aspect: 4:3, pixel 8:7, stretch\n"
          "  F4                co-op on / off (two players: a second controller is pad 2)\n"
          "  F5                reload the asset packs (--pack)\n"
          "  F6 / F9           quick save / quick load   F8   screenshot\n"
          "  Tab (hold)        fast-forward              F10  menu\n"
          "  Ctrl+Q            quit\n"
          "Back buttons and grips (Steam Deck L4 R4 L5 R5, Steam Controller grips, Elite\n"
          "  paddles): L4 quick save, R4 quick load, L5 fast-forward (hold), R5 menu.\n"
          "  On any controller, L3 + R3 (both sticks clicked) opens the menu too.\n"
          "  settings.ini: bind_paddle1..4=ACTION, key_ACTION=KEY (docs/HANDHELD.md).\n"
          "Controllers: south = B, east = A, west = Y, north = X, shoulders = L / R,\n"
          "  Start, Back = Select, d-pad or left stick. The window's size, place and\n"
          "  fullscreen state, the look and the aspect are remembered between runs.\n"
          "Co-op: on by itself when two controllers are connected (pad 1: keyboard and\n"
          "  the first; pad 2: the second, for party members 2 and 3); F4, --coop or\n"
          "  --no-coop change it. --coop-split (experimental): a second view when\n"
          "  player 2 walks away.\n",
          out);
    qol_usage(out);   /* port: QOL */
}

/* ---- port: what QOL (qol.h) borrows ---- */

static void qol_audio_restart(void)
{
    if (!audio)
        return;
    SDL_PauseAudioDevice(audio, 1);
    SDL_ClearQueuedAudio(audio);
    playing = 0;   /* playback starts again once the queue is back at its target */
    level_avg = AUDIO_TARGET;
    rs_init(&rs, SCHED_AUDIO_HZ, RATE);
}

static void qol_pacing_reset(void)
{
    next_frame = SDL_GetPerformanceCounter();
    pace.last = 0;
}

static void qol_poll(void)
{
    poll_input(sched_frame_count());
}

/* What is on screen, at the window's size. */
static int qol_shot(const char *path)
{
    int w, h;
    if (!hd2d_mode) {
        SDL_GetRendererOutputSize(ren, &w, &h);
        SDL_UpdateTexture(tex, NULL, turn_tag(sched_frame()), SCHED_WIDTH * 4);
        return qol_plain_shot(ren, tex, w, h, path);
    }
    SDL_GL_GetDrawableSize(win, &w, &h);
    int sw = shot_w, sh = shot_h;
    shot_w = w, shot_h = h;
    int ok = write_shot(path, sched_frame(), sched_layers(), sched_behind(),
                        sched_behind_layers(), sched_ppu_state_get());
    shot_w = sw, shot_h = sh;
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    static long frames = -1;           /* static: survive the longjmp */
    static int scale = 3, require_render, require_no_underrun, fullscreen;
    static const char *needed, *module_path, *rom_arg;
#ifdef CT_FIRSTRUN
    static const char *data_dir = CT_FIRSTRUN_DATA_DIR, *game_id = CT_FIRSTRUN_GAME,
                      *status_shot;
    static int interp_only, build_only;
#else
    static const char *game_id = CT_GAME_ID;   /* port: names the saves folder */
#endif
    static const char *save_path;   /* port: --save */
    static int no_save;
    static const char *import_srm, *export_srm;   /* port: QOL */
    static int recording, scripted;
#ifdef CT_PS5
    ps5_init(argc > 0 ? argv[0] : NULL);   /* port: the log file, the splash screen */
#endif
    register_settings();   /* port: the settings registry (settings.h), before reading them */
    settings_load();   /* port: saved HD-2D settings; options below override */
#ifdef CT_PS5_GL
    setenv("CT_PS5_DISPLAY_HEIGHT", ps5_display_values[ps5_display], 1);   /* before SDL_Init */
    /* port: the renderer's per-pass timings, every 30th frame, while the
       3D diorama's speed on the console is being tuned (docs/HD2D.md) */
    setenv("CT_HD2D_PROFILE", "/data/ct-recomp/hd2d-profile.txt", 0);
    fprintf(stderr, "ct_ps5: display %s\n", ps5_display_labels[ps5_display]);
#endif
    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "--scale") && k + 1 < argc)
            scale = atoi(argv[++k]), wst.scale_given = 1;
        else if (!strcmp(argv[k], "--frames") && k + 1 < argc)
            frames = atol(argv[++k]);
        else if (!strcmp(argv[k], "--dump") && k + 1 < argc)
            dump = argv[++k];
        else if (!strcmp(argv[k], "--needed-hw") && k + 1 < argc)
            needed = argv[++k];
        else if (!strcmp(argv[k], "--fast"))
            fast = 1;
        else if (!strcmp(argv[k], "--require-render"))
            require_render = 1;
        else if (!strcmp(argv[k], "--log-input"))
            log_input = 1;
        else if (!strcmp(argv[k], "--log-audio"))
            log_audio = 1;
        else if (!strcmp(argv[k], "--music-log")) {   /* port: audio packs */
            log_music = 1;
            music_set_log(stdout);
        }
        else if (!strcmp(argv[k], "--require-no-underrun"))
            require_no_underrun = 1;
        else if (!strcmp(argv[k], "--interp-only")) {
            sched_set_native(0);
#ifdef CT_FIRSTRUN
            interp_only = 1;
#endif
        }
        else if (!strcmp(argv[k], "--record") && k + 1 < argc) {
            if (replay_rec_open(&rec, argv[++k], "ct_sdl --record")) {
                fprintf(stderr, "ct_sdl: cannot write %s\n", argv[k]);
                return 2;
            }
            atexit(close_record);
            recording = 1;
        } else if (!strcmp(argv[k], "--script") && k + 1 < argc) {
            if (replay_load(&script, argv[++k])) {
                fprintf(stderr, "ct_sdl: bad --script %s\n", argv[k]);
                return 2;
            }
            scripted = 1;
        } else if (!strcmp(argv[k], "--module") && k + 1 < argc) {
            module_path = argv[++k];
        } else if (!strcmp(argv[k], "--rom") && k + 1 < argc) {
            rom_arg = argv[++k];
        } else if (!strcmp(argv[k], "--poke-wram") && k + 1 < argc) {   /* port */
            unsigned a, v;
            if (sscanf(argv[++k], "%ld:%x^%x", &poke_frame, &a, &v) != 3 || poke_frame < 1 ||
                a > 0x1FFFF || !v || v > 0xFF) {
                fprintf(stderr, "ct_sdl: --poke-wram F:ADDR^M\n");
                return 2;
            }
            poke_addr = a;
            poke_val = (uint8_t)v;
        } else if (!strcmp(argv[k], "--set-wram") && k + 1 < argc) {   /* port */
            unsigned a, v;
            if (sscanf(argv[++k], "%ld-%ld:%x=%x", &set_from, &set_to, &a, &v) != 4 ||
                set_from < 1 || set_to < set_from || a > 0x1FFFF || v > 0xFF) {
                fprintf(stderr, "ct_sdl: --set-wram F1-F2:ADDR=V\n");
                return 2;
            }
            set_addr = a;
            set_val = (uint8_t)v;
#ifdef CT_FIRSTRUN
        } else if (!strcmp(argv[k], "--data-dir") && k + 1 < argc) {
            data_dir = argv[++k];
        } else if (!strcmp(argv[k], "--game") && k + 1 < argc) {
            game_id = argv[++k];
        } else if (!strcmp(argv[k], "--shot") && k + 1 < argc &&
                   !strncmp(argv[k + 1], "status:", 7)) {
            status_shot = argv[++k] + 7;
        } else if (!strcmp(argv[k], "--build-only")) {
            build_only = 1;
#endif
        } else if (!strcmp(argv[k], "--save") && k + 1 < argc) {
            save_path = argv[++k];
        } else if (!strcmp(argv[k], "--no-save")) {
            no_save = 1;
        } else if (!strcmp(argv[k], "--mute")) {   /* port: silent this run */
            mute_run = 1;
        } else if (!strcmp(argv[k], "--no-mute")) {
            mute_run = -1;
        } else if (!strcmp(argv[k], "--pad-test")) {   /* port: the controller test */
            padtest_start();
        } else if (!strcmp(argv[k], "--rumble") && k + 1 < argc) {   /* port */
            int l = -1;
            for (int v = 0; v < RUMBLE_LEVELS; v++)
                if (!strcmp(argv[k + 1], rumble_values[v]))
                    l = v;
            if (l < 0) {
                fprintf(stderr, "ct_sdl: --rumble off|light|medium|strong\n");
                return 2;
            }
            k++;
            set_rumble(NULL, l);
        } else if (!strcmp(argv[k], "--log-rumble")) {
            rumble_out_set_log(1);
        } else if (!strcmp(argv[k], "--rumble-test") && k + 1 < argc) {
            rumble_test_at = atol(argv[++k]);
        } else if (!strcmp(argv[k], "--dump-sram") && k + 1 < argc) {
            saves_dump_at_exit(argv[++k]);
        } else if (!strcmp(argv[k], "--poke-sram") && k + 1 < argc) {
            if (saves_poke(argv[++k])) {
                fprintf(stderr, "ct_sdl: --poke-sram FRAME:ADDR=VAL (ADDR, VAL hex)\n");
                return 2;
            }
        } else if (!strcmp(argv[k], "--coop")) {   /* port: co-op */
            coop_pref = COOP_ON;
        } else if (!strcmp(argv[k], "--no-coop")) {
            coop_pref = COOP_OFF;
        } else if (!strcmp(argv[k], "--coop-split")) {   /* port: experimental */
            coop_pref = COOP_ON;
            sched_set_views(2);
        } else if (!strcmp(argv[k], "--pack-dump") && k + 1 < argc) {   /* port */
            pack_dump = argv[++k];
        } else if (!strcmp(argv[k], "--hd-font") && k + 1 < argc) {   /* port: HD text */
            snprintf(hd_font, sizeof hd_font, "%s", argv[++k]);
            hd_font_given = 1;
        } else if (!strcmp(argv[k], "--pack-export-sheets") && k + 1 < argc) {   /* port */
            pack_sheets = argv[++k];
        } else if (!strcmp(argv[k], "--pack-export-scale") && k + 1 < argc) {
            pack_sheets_scale = atoi(argv[++k]);
            if (pack_sheets_scale < 1 || pack_sheets_scale > 16) {
                fprintf(stderr, "ct_sdl: --pack-export-scale: 1 to 16\n");
                return 2;
            }
        } else if (!strcmp(argv[k], "--pack") && k + 1 < argc) {   /* port: asset packs */
            if (!packs_given)
                n_pack_names = 0;   /* the command line replaces the saved list */
            packs_given = 1;
            const char *v = argv[++k];
            if (!strcmp(v, "none"))
                n_pack_names = 0;
            else if (n_pack_names < MAX_PACK_NAMES && strlen(v) < sizeof pack_names[0])
                snprintf(pack_names[n_pack_names++], sizeof pack_names[0], "%s", v);
        } else if (!strcmp(argv[k], "--hd2d")) {
            hd2d_mode = 1;
        } else if (!strcmp(argv[k], "--no-hd2d")) {   /* port: beats the Game Mode default */
            hd2d_mode = -1;
        } else if (!strcmp(argv[k], "--pacing") && k + 1 < argc) {
            const char *v = argv[++k];
            pace_opt = !strcmp(v, "timer") ? PACE_TIMER : !strcmp(v, "vsync") ? PACE_VSYNC
                     : !strcmp(v, "auto") ? PACE_AUTO : -1;
            if (pace_opt < 0) {
                fprintf(stderr, "ct_sdl: --pacing: auto, timer or vsync\n");
                return 2;
            }
        } else if (!strcmp(argv[k], "--log-pacing")) {
            log_pacing = 1;
        } else if (!strcmp(argv[k], "--preset") && k + 1 < argc) {
            int p = parse_preset(argv[++k]);
            if (p < 0) {
                fprintf(stderr, "ct_sdl: --preset: off, subtle, hd2d, deck, hd2d3d or deck3d\n");
                return 2;
            }
            hd2d_set_preset(p);
            preset_given = 1;
        } else if ((!strcmp(argv[k], "--render-scale") || !strcmp(argv[k], "--sharpen") ||
                    !strcmp(argv[k], "--ao")) && k + 1 < argc) {   /* port: 3D quality */
            float v;
            int rs = !strcmp(argv[k], "--render-scale");
            if (parse_quality(argv[k + 1], rs, &v)) {
                fprintf(stderr, "ct_sdl: %s: %s or auto\n", argv[k], rs ? "50-100" : "0-1");
                return 2;
            }
            if (rs)
                hd2d_set_render_scale(v);
            else if (!strcmp(argv[k], "--ao"))
                hd2d_set_ao(v);
            else
                hd2d_set_sharpen(v);
            k++;
        } else if (!strcmp(argv[k], "--aspect") && k + 1 < argc) {
            const char *v = argv[++k];
            hd2d_set_aspect(!strcmp(v, "pixel") ? HD2D_ASPECT_PIXEL
                            : !strcmp(v, "stretch") ? HD2D_ASPECT_STRETCH
                                                    : HD2D_ASPECT_4_3);
        } else if (!strcmp(argv[k], "--debug-view") && k + 1 < argc) {
            hd2d_set_debug(atoi(argv[++k]));
        } else if (!strcmp(argv[k], "--fullscreen")) {
            fullscreen = 1;
        } else if (!strcmp(argv[k], "--log-gpu")) {
            log_gpu = 1;
            hd2d_set_timing(1);
        } else if (!strcmp(argv[k], "--shot") && k + 1 < argc) {
            char *v = argv[++k], *colon = strchr(v, ':');
            if (!colon || n_shots == MAX_SHOTS) {
                fprintf(stderr, "ct_sdl: --shot FRAME:PATH\n");
                return 2;
            }
            shots[n_shots].frame = atol(v);
            shots[n_shots].path = colon + 1;
            n_shots++;
        } else if (!strcmp(argv[k], "--touch")) {   /* port: touch overlay */
            touch_o.force = 1;
        } else if (!strcmp(argv[k], "--touch-hide") && k + 1 < argc) {
            touch_o.hide_seconds = atoi(argv[++k]);
        } else if (!strcmp(argv[k], "--touch-test") && k + 1 < argc) {
            if (touch_test_load(argv[++k]))
                return 2;
        } else if (!strcmp(argv[k], "--menu-test") && k + 1 < argc) {   /* port: menu.h */
            if (menu_test_load(argv[++k]))
                return 2;
        } else if (!strcmp(argv[k], "--hash-log") && k + 1 < argc) {   /* port */
            if (!(hash_log = fopen(argv[++k], "w"))) {
                fprintf(stderr, "ct_sdl: cannot write %s\n", argv[k]);
                return 2;
            }
        } else if (!strcmp(argv[k], "--shot-size") && k + 1 < argc) {
            if (sscanf(argv[++k], "%dx%d", &shot_w, &shot_h) != 2 || shot_w < 16 || shot_h < 16) {
                fprintf(stderr, "ct_sdl: --shot-size WxH\n");
                return 2;
            }
        } else if (!strcmp(argv[k], "--help") || !strcmp(argv[k], "-h")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(argv[k], "--import-srm") && k + 1 < argc) {   /* port: QOL */
            import_srm = argv[++k];
        } else if (!strcmp(argv[k], "--export-srm") && k + 1 < argc) {
            export_srm = argv[++k];
        } else if (qol_option(argc, argv, &k) > 0) {
            /* port: QOL options (qol.h); a bad one is a usage error below */
#ifdef CT_PS5
        } else if (argv[k][0] != '-' && !rom_arg) {
            rom_arg = argv[k];   /* port: a launcher passes the ROM alone (ps5.h) */
#endif
        } else {
            usage(stderr);
            return 2;
        }
    }
    /* port: the controller test is silent unless --no-mute */
    mute_run = mute_run > 0 || (mute_run == 0 && padtest_active());
    rumble_apply();   /* port: the pad test rumbles even with rumble off */
    if (import_srm || export_srm) {   /* port: QOL: a save RAM file in or out, then stop */
        if (import_srm && export_srm) {
            fprintf(stderr, "ct_sdl: --import-srm or --export-srm, not both\n");
            return 2;
        }
        return import_srm ? saves_import(import_srm, save_path, game_id)
                          : saves_export(export_srm, save_path, game_id);
    }
    if (scale < 1)
        scale = 1;
    {   /* port: Steam Game Mode (device.h): fullscreen, HD-2D unless --no-hd2d */
        const char *why;
        game_mode = device_game_mode(&why);
        if (game_mode)
            fprintf(stderr, "ct_sdl: Steam Game Mode (%s): fullscreen, no cursor%s\n", why,
                    hd2d_mode >= 0 ? ", HD-2D" : "");
        if (!hd2d_mode && renderer_saved >= 0)   /* port: the menu's choice (settings.ini renderer=) */
            hd2d_mode = renderer_saved ? 1 : -1;
#ifdef CT_PS5_GL
        if (!hd2d_mode)   /* port: the PS5's GPU build: HD-2D unless chosen otherwise */
            hd2d_mode = 1;
#endif
        if (game_mode && !hd2d_mode)
            hd2d_mode = 1;
        int no_hd2d = hd2d_mode < 0;
        if (hd2d_mode < 0)
            hd2d_mode = 0;
        /* port: asset packs replace art in the GL renderer: with packs and
           the plain look, draw through it with the original look. */
        /* port: HD text needs the GL renderer too */
        if (hd_font[0] && strcmp(hd_font, "none")) {
            if (no_hd2d)
                fprintf(stderr, "ct_sdl: --no-hd2d: no HD text (it needs the GL renderer)\n");
            else if (!hd_font_load(game_id) && !hd2d_mode) {
                hd2d_mode = 1;
                if (!preset_given) {
                    preset_forced = hd2d_preset();
                    hd2d_set_preset(HD2D_OFF);
                }
                fprintf(stderr, "ct_sdl: HD text: drawing through the GL renderer\n");
            }
        }
        if (n_pack_names && no_hd2d) {
            fprintf(stderr, "ct_sdl: --no-hd2d: asset packs' art not loaded (it needs the GL "
                            "renderer); their music is\n");
            packs_setup_ex(game_id, 0);
        } else if (n_pack_names) {
            int r = packs_setup(game_id);
            if (r < 0)
                fprintf(stderr, "ct_sdl: no asset pack could be read\n");
            else if (packs_active() && !hd2d_mode) {
                hd2d_mode = 1;
                if (!preset_given) {
                    preset_forced = hd2d_preset();
                    hd2d_set_preset(HD2D_OFF);
                }
                fprintf(stderr, "ct_sdl: asset packs: drawing through the GL renderer\n");
            }
        }
    }
    quit_combo_init(&quitc);
    touch_init(&touch_o);   /* port */
    atexit(report_exit);

#ifdef CT_FIRSTRUN
    if (build_only) {   /* port: --build-only: no window, no audio */
        if (module_path || interp_only) {
            fprintf(stderr, "ct_sdl: --build-only builds the module: not with --module or "
                            "--interp-only\n");
            return 2;
        }
        if (SDL_Init(SDL_INIT_EVENTS)) {
            fprintf(stderr, "ct_sdl: SDL_Init: %s\n", SDL_GetError());
            set_exit_reason("SDL_Init failed");
            return 1;
        }
        firstrun_opts fo = {rom_arg, data_dir, game_id, 1, NULL, NULL, NULL, 1};
        int r = firstrun(&fo);
        set_exit_reason(r == FIRSTRUN_MODULE ? "--build-only: module ready"
                                             : "--build-only: no module");
        SDL_Quit();
        return r == FIRSTRUN_MODULE ? 0 : 1;
    }
#endif
    input_sdl_hints(steam_hidapi);   /* port: Deck / Steam Controller back buttons */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)) {
        fprintf(stderr, "ct_sdl: SDL_Init: %s\n", SDL_GetError());
        set_exit_reason("SDL_Init failed");
        return 1;
    }
    hid_deck_set_log(log_input);   /* port: InputPlumber's Deck pad (hid_deck.h) */
    hid_deck_update();
    no_pad_hint = scripted || dump;
    static CPU cpu;
    int first_run = 0;
#ifdef CT_FIRSTRUN
    if (!module_path) {   /* port: engine-only first run (firstrun.h) */
        if (open_window("ct-recomp", scale, fullscreen))
            return 1;
        int preset = hd2d_preset(), view = hd2d_debug();
        hd2d_set_preset(HD2D_OFF);   /* the setup screens in the original look */
        hd2d_set_debug(HD2D_DEBUG_NONE);
        firstrun_opts fo = {rom_arg, data_dir, game_id, !interp_only, present_status,
                            shot_status, status_shot, 0};
        int r = firstrun(&fo);
        hd2d_set_preset(preset);
        hd2d_set_debug(view);
        if (r == FIRSTRUN_QUIT) {
            set_exit_reason("quit during setup");
            return 0;
        }
        if (r == FIRSTRUN_ERROR) {
            set_exit_reason("setup failed");
            return 2;
        }
        if (r == FIRSTRUN_INTERP)
            fprintf(stderr, "ct_sdl: no game module: running in the interpreter only\n");
        first_run = 1;
    }
#endif
#ifdef CT_PS5
    /* port: no command line from an ELF loader: the ROM and module from
       the fixed folders (ps5.h). A build with the game compiled in
       (CT_PS5_ENGINE unset) never loads a module. */
#ifdef CT_PS5_ENGINE
    if (ps5_find_files(&rom_arg, &module_path, 1)) {
#else
    if (ps5_find_files(&rom_arg, NULL, 0)) {
#endif
        set_exit_reason("no ROM");
        return 2;
    }
#endif
    if (!first_run)
        bus_init(rom_arg);
    if (module_path) {   /* port: game code from a module (runtime/module.h) */
        const char *err;
        if (ct_module_load(module_path, &err)) {
            fprintf(stderr, "ct_sdl: --module: %s\n", err);
            set_exit_reason("game module failed to load");
            return 2;
        }
        fprintf(stderr, "ct_sdl: game module %s: %u functions\n", module_path, ct_func_count);
    }
    if (!no_save && saves_open(save_path, game_id)) {   /* port: the save RAM from disk (saves.h) */
#ifdef CT_PS5
        ps5_notify("CT5: saving is off this time (see ct-recomp.log)");
#endif
    }
    char title[22];   /* ROM header title, $FFC0, 21 bytes, space padded */
    memcpy(title, bus_rom() + 0xFFC0, 21);
    title[21] = 0;
    for (int k = 20; k >= 0 && (title[k] == ' ' || !title[k]); k--)
        title[k] = 0;
    if (win)
        SDL_SetWindowTitle(win, title);
    else if (open_window(title, scale, fullscreen))
        return 1;
    if (hd2d_mode) {
        /* port: say which look is on and how to change it. */
        static char hello[64];
        snprintf(hello, sizeof hello, "Look: %s - [r3] to change",   /* F1 on a keyboard */
                 hd2d_preset() == HD2D_OFF ? "original" : hd2d_preset() == HD2D_SUBTLE ? "subtle"
                 : hd2d_preset() == HD2D_DECK ? "HD-2D Steam Deck"
                 : hd2d_preset() == HD2D_3D ? "HD-2D 3D diorama"
                 : hd2d_preset() == HD2D_DECK3D ? "3D diorama, Steam Deck" : "HD-2D");
        hd2d_osd(hello);
        /* port: the driver's first-draw work now, not on frame 1 with
           audio playing. */
        int ww, wh;
        SDL_GL_GetDrawableSize(win, &ww, &wh);
        uint64_t tw = SDL_GetPerformanceCounter();
        hd2d_warmup(ww, wh);
        fprintf(stderr, "ct_sdl: hd2d warm-up %dx%d: %.1f ms\n", ww, wh,
                (double)(SDL_GetPerformanceCounter() - tw) * 1000.0 / SDL_GetPerformanceFrequency());
        if (packs_active()) {   /* port: asset packs: the atlas to the GPU */
            packs_upload();
            atexit(pack_map_report);
            static char msg[48];
            int n;
            packs_entries(&n);
#ifdef CT_PS5   /* port: no F5 on the console: the menu's Packs page reloads */
            snprintf(msg, sizeof msg, "Packs: %d replacements", n);
#else
            snprintf(msg, sizeof msg, "Packs: %d replacements - F5 reloads", n);
#endif
            hd2d_osd(msg);
        }
        if (packs_given)
            settings_save();   /* port: remember the packs */
    }

    SDL_AudioSpec want = {0}, have;
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = AUDIO_BUFFER;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!audio)
        fprintf(stderr, "ct_sdl: no audio: %s\n", SDL_GetError());
    rs_init(&rs, SCHED_AUDIO_HZ, RATE);
    music_set_rate(RATE);   /* port: audio packs mix at the output rate */
    print_controllers();
    pad = open_controller();
    if (pad)
        fprintf(stderr, "ct_sdl: using \"%s\" on pad 1\n", SDL_GameControllerName(pad));
    if (sched_views() > 1 && hd2d_mode)
        fprintf(stderr, "ct_sdl: --coop-split: the split screen is drawn by the plain "
                        "picture only (--no-hd2d); HD-2D keeps one view\n");
    pad2 = open_controller_except2(pad, NULL);   /* port: co-op */
    pad3 = open_controller_except2(pad, pad2);
    if (pad2)
        fprintf(stderr, "ct_sdl: second controller \"%s\"\n", SDL_GameControllerName(pad2));
    if (pad3)
        fprintf(stderr, "ct_sdl: third controller \"%s\"\n", SDL_GameControllerName(pad3));
    coop_update();
    /* port: frontend actions (actions.h): stubs say so on screen until
       something implements them; the menu shows what the buttons do. */
    actions_set_message(osd);
    actions_set_trace(log_action);
    actions_set_handler(ACTION_MENU, menu_action, NULL);   /* port: the menu (menu.h) */
    menu_game_id = game_id;
    atexit(menu_report);
    use_glyphs_of(pad);
    fprintf(stderr, "ct_sdl: glyphs: %s (%d pictures built in)\n",
            glyphs_family_name(glyphs_in_use()), glyphs_built_in());
    {
        char plain[200];
        osd_text_plain("actions: [@quicksave] quick save, [@quickload] quick load, [@ffwd] "
                       "fast-forward (hold), [@menu] menu, [@screenshot] screenshot",
                       plain, sizeof plain);
        fprintf(stderr, "ct_sdl: %s\n", plain);
    }

    interp_reset(&cpu);
    sched_init(&cpu);
    sched_set_behind(hd2d_mode && hd2d_wants_behind());   /* port: see on_frame */
    sched_set_prov(want_prov());   /* port: asset packs and HD text read it each frame */
    if (pack_dump) {   /* port: asset pack dump (docs/ASSET-PACKS.md) */
        char *dir = pack_dir(game_id, pack_dump);
        const char *base = strrchr(pack_dump, '/') ? strrchr(pack_dump, '/') + 1 : pack_dump;
        if (!dir || packs_dump_open(dir, base)) {
            set_exit_reason("cannot write the pack dump");
            return 2;
        }
        free(dir);
        atexit(pack_dump_report);
        sched_set_prov(1);
    }
    if (pack_sheets) {   /* port: sprite sheets for artists (docs/ASSET-PACKS.md) */
        char *dir = malloc(4096);
        if (dir && (strchr(pack_sheets, '/') || strchr(pack_sheets, '\\'))) {
            snprintf(dir, 4096, "%s", pack_sheets);
        } else if (dir) {
            char *pref = SDL_GetPrefPath("ct-recomp", game_id);
            snprintf(dir, 4096, "%ssheets/%s", pref ? pref : "./", pack_sheets);
            SDL_free(pref);
        }
        if (!dir || pack_scene_export_open(dir, pack_sheets_scale)) {
            free(dir);
            set_exit_reason("cannot write the sprite sheets");
            return 2;
        }
        free(dir);
        atexit(pack_sheets_report);
    }
    sched_set_frame_hook(on_frame);
    sched_set_reset_hook(script_reset);
    ct_fatal_hook = on_fatal;
    if (setjmp(fatal_jmp)) {
        static char stop[512];
        snprintf(stop, sizeof stop, "frame %ld, line %d, PC $%02X%04X: %s", sched_frame_count(),
                 sched_line(), cpu.PB, cpu.PC, fatal_msg);
        fprintf(stderr, "ct_sdl: stopped in %s\n", stop);
        set_exit_reason("fatal error: %s", stop);
        if (needed && hw_needed_write(needed, "ct_sdl", stop))
            fprintf(stderr, "ct_sdl: cannot write %s\n", needed);
        SDL_Quit();
        return 1;
    }

    {   /* port: QOL (qol.h): save states, resume, fast-forward, ... */
        static const qol_host qh = {qol_shot, qol_audio_restart, present_now, qol_poll,
                                    qol_pacing_reset, settings_save};
        qol_init(&qh, game_id, game_mode, hd2d_mode, !no_save, recording, scripted);
    }
    key_filter_init(&keys);
    frontend_ready = 1;   /* port: settings changed from here on apply live */
    perf_freq = SDL_GetPerformanceFrequency();
    pace.last = 0;   /* port: the setup screens' time is not a frame */
    next_frame = SDL_GetPerformanceCounter();
    poll_input(sched_frame_count());   /* port: a loaded state's frame (QOL), else 0 */
    set_input(sched_frame_count() + 1);
    /* Everything per frame happens in on_frame, at the frame edge; this
       loop only keeps the scheduler running. port: QOL pauses it (qol_idle)
       and saves and loads states between frames. */
    while (running && (frames < 0 || sched_frame_count() < frames)) {
        if (menu_is_open()) {   /* port: the menu (menu.h): the game waits */
            menu_run(sched_frame_count());
            continue;
        }
        if (qol_paused()) {
            rumble_out_idle(SDL_GetTicks());   /* port: paused: still */
            qol_idle();
            continue;
        }
        sched_run_frame();
        qol_after_frame();
        if (padtest_tick(menu_opened_count())) {   /* port: every control checked */
            set_exit_reason("pad test: every control works");
            running = 0;
        }
    }
    qol_quit(!running);   /* port: QOL: the resume point, on a quit */

    if (running)
        set_exit_reason("--frames %ld reached", frames);
    exit_frame = sched_frame_count();
    int rendered = 0;
    const uint8_t *fb = sched_frame();
    for (size_t k = 0; k < (size_t)SCHED_WIDTH * SCHED_HEIGHT * 4 && !rendered; k += 4)
        rendered = fb[k] | fb[k + 1] | fb[k + 2];
    printf("ct_sdl: %ld frames, %ld NMIs, PC $%02X%04X\n", sched_frame_count(), sched_nmi_count(),
           cpu.PB, cpu.PC);
    for (unsigned k = 0; k < hw_note_count(); k++)
        printf("ct_sdl: stubbed: %s\n", hw_note_text(k));
    if (needed && hw_needed_write(needed, "ct_sdl", NULL))
        fprintf(stderr, "ct_sdl: cannot write %s\n", needed);
    if (win && !game_mode) {   /* port: remember the window, if it changed */
        wst_track();
        if (wst_dirty())
            settings_save();
    }
    rumble_out_stop_all();   /* port: simulated rumble: every motor off */
    hid_deck_close();        /* (the InputPlumber pad's motors too) */
    if (pad)
        SDL_GameControllerClose(pad);
    if (pad2)
        SDL_GameControllerClose(pad2);
    if (pad3)
        SDL_GameControllerClose(pad3);
    if (tex2)
        SDL_DestroyTexture(tex2);
    menu_shutdown();      /* port: before the renderer or GL context goes */
    osd_sdl_shutdown();   /* port */
    if (audio)
        SDL_CloseAudioDevice(audio);
    if (glctx) {
        touch_gl_shutdown();   /* port */
        hd2d_shutdown();
        SDL_GL_DeleteContext(glctx);
    }
    SDL_Quit();
    if (require_no_underrun && audio_underruns)
        return 1;
    return require_render && !rendered ? 1 : 0;
}
