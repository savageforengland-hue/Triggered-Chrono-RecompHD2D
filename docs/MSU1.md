# MSU-1 soundtrack packs

The audio-pack mixer reads standard MSU-1 `.pcm` recordings directly. No WAV
conversion or patched ROM is required. This is soundtrack-file compatibility,
not emulation of MSU-1 registers or the `.msu` data/video device.

## Import an existing pack

Unpack your own recordings into a directory, then run on the development computer:

```sh
python3 tools/import_msu1.py /path/to/chrono-pack
```

If several filename prefixes exist, select one with `--prefix chrono_msu1`.
For example, `chrono_msu1-24.pcm` maps to game song 24 (hexadecimal `0x18`).
The importer writes `pack.toml` next to the recordings, validates the files before
writing, and refuses to overwrite an existing manifest. It does not copy audio.
Enable this directory through the normal asset-pack settings; see
[ASSET-PACKS.md](ASSET-PACKS.md#music-audio-packs).

The default `chrono-trigger` profile maps tracks 1-83 to the same game song IDs.
It disables looping for the one-shot tracks identified by the
[Chrono Trigger MSU music driver](https://github.com/qwertymodo/ChronoTrigger-MSU1/blob/master/chrono_msu1_music.asm).
Track 84 is a patch-specific attract theme, and tracks 100 onward may contain video
audio. They are excluded: the unpatched game's audio events do not implement those
patch-specific triggers. The original game's theme continues to map to track 24.
Custom packs may require manual `[[music]]` mapping. The explicit `--profile identity`
option maps tracks 1-255 directly and loops them all; use it only when those IDs and
loop policies are appropriate for the game.

## Manual mapping and loop overrides

```toml
[[music]]
song = 0x18
file = "chrono_msu1-24.pcm"
loop = true
# Omit loop_start to use the PCM header; an explicit 0 overrides it.
# loop_start = 0
volume = 0.8
```

`loop = false` plays a jingle once. Fades, pack priority, streaming resampling and
music/SFX separation use the existing audio-pack pipeline. Missing or invalid files
leave the console soundtrack enabled. The loader requires the eight-byte `MSU1`
header, complete nonempty stereo frames, and a loop position inside the recording.
Headerless raw PCM is rejected. The on-disk layout follows the
[bsnes implementation](https://github.com/devinacker/bsnes-plus/blob/master/bsnes/snes/chip/msu1/msu1.cpp):
44.1 kHz signed 16-bit little-endian stereo, with a 32-bit loop position in frames.

The existing pack limitations still apply: loading a save state stops replacement
audio until the next game song event; mixed SPC echo is disabled while replacement
music plays; script timing still follows the original game. Long replacement songs
may not align with original ending/cutscene timing. No ending synchronization,
video playback, or patch-specific pause/resume semantics are added here.

## Validation and PS5 follow-up

Synthetic tests cover intro-versus-loop playback, explicit loop override, one-shot
playback, stereo polarity, fades, a one-frame loop, bad headers, truncated data,
invalid loop offsets, missing files, and unchanged original audio on failure.
The importer has independent temporary-directory tests. Audio recordings and ROMs
are not included in the repository.

This branch has no native PS5 build target, toolchain or packaging integration.
Testing under Linux on a PS5 follows the PC build route; a native homebrew build
requires a separate platform integration. Neither hardware route has been tested.
After choosing the route and producing a runnable build, validate:

1. Boot and ordinary gameplay with no pack, then with the same save and a pack.
2. Song IDs in `--music-log`, room changes, battle music, victory jingles and fades.
3. Effects during music playback, repeated loops, pause, load and resume behavior.
4. Missing tracks reverting to original music without losing effects.
5. Intro, bed scene, dialogue opening, room transitions and world flyover with HD-2D.
6. Stable audio and frame pacing during an extended gameplay session.

Real Chrono Trigger recordings, event timing, and PS5 performance remain to be
verified with the user's ROM, pack and device.
