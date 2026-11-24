# Native PS5 HD-2D and MSU-1 integration

## Source and scope

Based on upstream `ps5/v1.0` at `f2d743cf2270305f45609905f04ddc7f95274f02`,
the source line associated with CT5 1.0. This is an update of the existing
PPSA28361 native title, not a new Linux port. The separate `ps5/port` branch
also stores older build archives and pack-directory diagnostics; those binaries
are not generated or replaced by this change.

Changes carried over from the desktop improvements:

- Compare sampled field-map pixels against the console frame; mismatches use
  the original 2D composition immediately, including after battles.
- Mask the opening dialogue window before its first glyph.
- Snap field sprites to the output pixel grid and quantize field zoom.
- Preserve sharp full-screen menus, Mode 7 and console color-math composition.
- Limit map geometry to visible room components, invalidate it as the viewport
  enters a new cell, and follow console scroll during world-map pans.
- Preserve 2D furniture composition when visible actors occupy blocked cells.
  This is a conservative fallback, not a reconstructed sleeping animation.
- Mark scripted field/world shots, follow their authored scroll without live
  camera offsets, and keep the mixed Mode 7/HDMA cutscene engine exact 2D.
- Ease the 3D depth-of-field aperture over 12 frames after cinematic cuts;
  smooth pans keep focus and scene/map cuts restart the existing crossfade.
- Stream MSU-1 PCM soundtracks with header loop points, one-shot jingles and
  original-audio fallback for missing or invalid tracks.

PS5-specific integration retains layered cards, the two OAM-priority passes,
attached effects, contact shadows and the world-cloud palette selection. Pixel
snapping runs after camera setup, and its mesh counts continue to separate low
sprites, high sprites and shadows. Hidden rooms cannot supply card ground fill.
World camera coordinates now come from the NMI scroll shadows, and party OAM
matching uses that scroll instead of assuming a centered leader. A synthetic
WRAM/OAM test covers independent pans, offscreen leaders and wrapping.
Module ABI 11 adds scripted/exact-2D scene flags (ABI 10 introduced the
full-screen UI mode); external game modules must be rebuilt. Existing
save-state ABI 7 onward remains accepted.
The previous eight-frame stale-3D grace period and fixed post-battle wait are
replaced by the console comparison.

The game-specific importer and its tests live under `game/ct/`, maintaining the
engine's existing game-agnostic boundary. Native PCM I/O uses the existing
64-bit `fseek`/`ftell` imports. Native-title builds now compile the frontend as
checked objects; the packaging scripts no longer suppress all frontend build
errors with `|| true`.

## Validation on 2026-11-24

- Full engine/frontend host build: passed (GCC 14.2, CMake 4.4.5, system SDL2).
- ROM-free CTest suite: **43/43 passed**, including PCM, Ogg/FLAC regression,
  importer, input, runtime, geometry and cutscene-classification tests.
- Music tests with the `CT_PS5` file-offset path selected on the host:
  **44 checks passed**. This is not a PS5 binary or a console test.
- Geometry and cutscene-state tests under AddressSanitizer/UndefinedBehaviorSanitizer:
  passed; leak detection disabled because of the execution environment.
- Both native-title shell scripts: syntax checks passed.
- Native-title object-target build path: configured and built with the host
  compiler; this checks the CMake packaging mechanism, not PS5 machine code.

New geometry checks cover cards excluding disconnected rooms, snapped vertices,
priority ordering without duplicated sprites, attached effects, contact shadows
and clearing shadow counts on an empty scene. The SDL virtual-controller fixture
now explicitly maps its declared touchpad button when SDL's automatic mapping
omits it; this fixes a pre-existing test failure without changing actual controls.
Synthetic cutscene checks cover all known special-engine NMI vectors, field event
control, free and scripted world-map cameras, live-camera suppression and the
12-frame focus transition. See [CUTSCENES.md](CUTSCENES.md).

## Native build is still blocked

No new `eboot.bin` has been produced, and no hardware acceptance is claimed.
The original GPU-title script requires the native-app boilerplate revision
`4f531c4b517f80bcb6b1267135848168d2250047`. On the date above, its documented
`blackbearreloaded/ps5-native-app-boilerplate` URL returned GitHub API 404 and
could not be fetched with Git. The accessible `drakmor` mirror stops at
`470695e0c557f99ff2df0e36e4df713c5e636526` and does not contain that revision.
It has not been substituted silently.

The PS5 payload SDK v0.42 archive was retrieved and its published SHA-256
verified (`8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da`).
That archive does not include the host LLVM compiler. The GPU SDK sources are
available at `OpenAGC/ps5-opengl`, but this session did not have the matching
built GL/SDL SDKs or LLVM toolchain. Recover the pinned kit or validate an
explicit dependency update, build matching GL/SDL libraries, and then run
`packaging/ps5/title/gl/build-title-gl.sh` as documented in [PS5.md](PS5.md).

Keep the current working console installation until a newly built candidate is
available. Follow the scenarios in [MSU1.md](MSU1.md) with the user's own ROM,
soundtrack and saves. Compare original audio and PCM packs, then check the bed,
dialogue opening, room boundaries, world flyover, battles, save states and an
extended session. Hardware frame pacing, visual composition and cutscene music
timing remain unverified.
