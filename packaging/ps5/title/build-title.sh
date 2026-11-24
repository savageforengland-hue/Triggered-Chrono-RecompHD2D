#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 ct-recomp contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# port: PS5 title (docs/PS5.md): eboot.bin + sce_sys/param.json +
# sce_module/libc.prx, an app folder ShadowMountPlus puts on the home screen.
# Built with drakmor/ps5-native-app-boilerplate (pinned below, patched by
# boilerplate.patch) over its pinned ps5-payload-sdk and PacBrew's SDL2.
#
#   packaging/ps5/title/build-title.sh [WORKDIR]   -> WORKDIR/kit/dist/PPSA28360/
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); src=$(cd "$here/../../.." && pwd)
work=${1:-$src/build-ps5-title}; kit=$work/kit; objs=$work/objs
TITLE_ID=${TITLE_ID:-PPSA28360}
BOILERPLATE_REV=470695e0c557f99ff2df0e36e4df713c5e636526
mkdir -p "$work"
if [ ! -d "$kit/.git" ]; then
    git clone https://github.com/drakmor/ps5-native-app-boilerplate "$kit"
    git -C "$kit" checkout -q "$BOILERPLATE_REV"
    git -C "$kit" apply "$here/boilerplate.patch"
fi
# zlib.net may be unreachable: the same tarball (same SHA-256) from GitHub.
z=$kit/.deps/native/zlib/zlib-1.3.2.tar.gz
[ -f "$z" ] || { mkdir -p "${z%/*}"; curl -fsSL -o "$z" \
    https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz; }
bash "$kit/tools/setup-native-dependencies.sh" >/dev/null
bash "$kit/tools/setup-pacbrew-dependencies.sh" --all >/dev/null
sdk=$kit/.deps/native/ps5-payload-sdk
pb=$(ls -d "$kit"/.deps/pacbrew/v*/sysroot | head -1)/user/homebrew

# The engine, compiled against the pinned SDK and PacBrew's SDL2.
args=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF -DCT_PS5_TITLE=ON
      -DSDL2_DIR="$pb/lib/cmake/SDL2" -DCMAKE_FIND_ROOT_PATH="$pb/../..")
[ -n "${CT_ROM:-}" ] || args+=(-DCT_ENGINE_ONLY=ON)
PATH=$(dirname "$(command -v cmake)"):$PATH "$sdk/bin/prospero-cmake" -S "$src" -B "$objs" "${args[@]}"
libs=(ct_input ct_hd2d ct_packs ct_hdtext ct_menu ct_imgui ct_sched ct_interp ct_runtime
      ct_snes_vendor ct_tables_empty)
cmake --build "$objs" -j "$(nproc)" --target "${libs[@]}"
cmake --build "$objs" -j "$(nproc)" --target ct_sdl_engine   # checked frontend objects

# One relocatable object; missing weak game hooks become NULL.
cd "$objs"
mkdir -p "$kit/vendor/lib"
o=$(find CMakeFiles/ct_sdl_engine.dir CMakeFiles/ct_tables_empty.dir -name '*.o')
# The game's own hand-written C that only reads its RAM (no code made from
# the ROM): the scene for the 3D diorama, the world map's, poses, music,
# text boxes and rumble. Without them the diorama has nothing to build
# (the camera settings did nothing).
mkdir -p "$objs/game_hooks"
for f in scene world_scene sprites audio text rumble; do
    "$sdk/bin/prospero-clang" -std=c11 -O2 -fPIC -DCT_PS5 -I"$src/runtime" -I"$src/game/ct" \
        -c "$src/game/ct/$f.c" -o "$objs/game_hooks/$f.o"
    o="$o $objs/game_hooks/$f.o"
done
a=$(printf 'lib%s.a ' "${libs[@]:0:10}")
"$sdk/bin/ld.lld" -r -o "$kit/vendor/ct_all.tmp.o" $o --whole-archive $a --no-whole-archive
defs=$(readelf -sW "$kit/vendor/ct_all.tmp.o" |
       awk '$5=="WEAK" && $7=="UND" && $8 ~ /^ct_/ {print "--defsym=" $8 "=0"}' | sort -u)
"$sdk/bin/ld.lld" -r -o "$kit/vendor/ct_all.o" $defs "$kit/vendor/ct_all.tmp.o"
rm -f "$kit/vendor/ct_all.tmp.o" "$kit/vendor/lib/libct_title.a"
"$sdk/bin/llvm-ar" rcs "$kit/vendor/lib/libct_title.a" "$kit/vendor/ct_all.o"

# The title: identity, icon, no boilerplate demo art.
cd "$kit"
rm -rf src assets sce_sys/pic0.dds sce_sys/pic1.dds sce_sys/snd0.at9 \
    sce_sys/background-source.png sce_sys/launch-background-source.png
mkdir -p src && cp "$here/title.c" src/title.c
cp "$src/packaging/ps5/icon0.png" sce_sys/icon0.png
make init TITLE_ID="$TITLE_ID" APP_NAME="CT5"
# Until the import list stops changing: the check lists what the previous
# build imported, and the check itself imports a few functions once it has
# a non-empty list.
printf '#define CT_IMPORTS(X)\n' > src/imports.gen.h
for pass in 1 2 3 4; do
    make APP_STATIC_ARCHIVES=vendor/lib/libct_title.a PACBREW_PACKAGES=sdl2 build
    cp src/imports.gen.h build/imports.prev.h
    python3 "$here/gen_imports.py" build/llvm-pie.elf "$sdk/target/lib" src/imports.gen.h
    cmp -s src/imports.gen.h build/imports.prev.h && break
done
cmp -s src/imports.gen.h build/imports.prev.h || { echo "import list did not settle" >&2; exit 1; }
echo "title folder: $kit/dist/$TITLE_ID"
