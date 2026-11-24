/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port: audio packs: the [[music]] entries of asset packs, which play the
 * player's own recordings (Ogg Vorbis, FLAC, WAV or MSU-1 PCM) in place of the
 * game's songs (docs/ASSET-PACKS.md). The game module says when a song
 * starts, fades or stops (module.h ct_music_event, by the game's song
 * ids); this streams the file mapped to that song, loops it, follows the
 * fades, and mixes it into the output. While a replacement plays, the
 * caller mutes the console's music voices (sched_set_voice_mute): the
 * sound effects stay.
 *
 * pack.toml:
 *
 *   [[music]]
 *   song = 0x18                 # the game's song id (--music-log prints them)
 *   file = "music/fair.ogg"     # .ogg, .flac, .wav or .pcm, relative to the pack
 *   loop = true                 # play it again from loop_start at its end (default)
 *   loop_start = 441000         # in the file's samples (sample frames), default: PCM header or 0
 *   loop_end = 5292000          # default: the end of the file
 *   volume = 0.8                # 0-4, default 1
 *   fade = "game"               # "game": follow the game's fades (default); "none"
 *
 * Nothing here knows a game: song ids are just numbers. No SDL. */
#ifndef CT_MUSIC_H
#define CT_MUSIC_H

#include <stdint.h>
#include <stdio.h>

#include "module.h"

/* Read the [[music]] entries of these packs, highest priority first (an
   earlier pack wins a song). Replaces what was loaded and stops any
   replacement. Returns how many songs are mapped (0 is not an error);
   problems are printed. */
int music_load(const char *const *dirs, int n);
int music_reload(void);
void music_unload(void);
int music_count(void);
/* The mapped file of a song, or NULL. */
const char *music_file(int song);

/* The rate music_mix makes, Hz (default 48000). */
void music_set_rate(int hz);
/* --music-log: a line on f for every event (NULL: none). */
void music_set_log(FILE *f);

/* An event from the game (ct_music_events), at frame `frame`. */
void music_event(const ct_music_event *e, long frame);
/* Stop at once (a reset, a save state load, packs reloaded). */
void music_stop(void);
/* A replacement is playing (or about to): mute the console's music. */
int music_replacing(void);
/* Add the next n stereo frames (L R interleaved) of the replacement to
   stereo, saturating. Does nothing while none plays. */
void music_mix(int16_t *stereo, int n);
/* The volume a replacement plays at now, 0-4 (the entry's volume times
   the game's fades), for tests and the log. */
double music_gain(void);

#endif
