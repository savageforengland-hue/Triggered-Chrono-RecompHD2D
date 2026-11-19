#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 ct-recomp contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# port: PS5 title with GPU OpenGL (docs/PS5.md): the HD-2D renderer, the
# 3D diorama and the menu drawn on the PS5's GPU through the ps5-opengl SDK
# (Mesa/Gallium) and its SDL2 bridge, at 1080p, 1440p or 4K.
#
#   GL_PREFIX=.../ps5-opengl/build/sdk/ps5-opengl-gl46   (make sdk-gl46, then
#             PS5_DYNAMIC_SCANOUT=1 PS5_SCANOUT_HEIGHT=2160 PS5_SCANOUT_FPS=120
#             toolchain/install-ps5-opengl-gl46.sh for runtime display modes)
#   SDL_PREFIX=.../ps5-opengl/build/native-build/sdk    (integration/SDL2/build.py
#             native, with gl/sdl-bridge.patch applied to the ps5-opengl checkout)
#   GL_SRC=.../ps5-opengl                                (for native-app/)
#   KIT=.../ps5-native-app-boilerplate at 4f531c4b       (its .deps set up)
#   packaging/ps5/title/gl/build-title-gl.sh [WORKDIR]   -> WORKDIR/app/dist/<TITLE_ID>/
set -euo pipefail
: "${GL_PREFIX:?}" "${SDL_PREFIX:?}" "${GL_SRC:?}" "${KIT:?}"
here=$(cd "$(dirname "$0")/.." && pwd); src=$(cd "$here/../../.." && pwd)
work=${1:-$src/build-ps5-title-gl}; objs=$work/objs; app=$work/app
TITLE_ID=${TITLE_ID:-PPSA28361}
sdk=$KIT/.deps/native/ps5-payload-sdk
builtins=${BUILTINS:-$(find "$(dirname "$(dirname "$(readlink -f "$(command -v llvm-config-21 || command -v llvm-config)")")")/lib/clang" -name 'libclang_rt.builtins*.a' | grep -m1 x86_64)}
test -s "$builtins"
mkdir -p "$work"

# The engine, against the kit's SDK, the bridge SDL and the GL SDK.
args=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF -DCT_PS5_TITLE=ON
      -DCT_PS5_GL_PREFIX="$GL_PREFIX" -DSDL2_DIR="$SDL_PREFIX/lib/cmake/SDL2"
      -DPS5OpenGLCore33_DIR="$GL_PREFIX/lib/cmake/PS5OpenGLCore33"
      -DCMAKE_FIND_ROOT_PATH="$SDL_PREFIX;$GL_PREFIX")
[ -n "${CT_ROM:-}" ] || args+=(-DCT_ENGINE_ONLY=ON)
"$sdk/bin/prospero-cmake" -S "$src" -B "$objs" "${args[@]}"
libs=(ct_input ct_hd2d ct_packs ct_hdtext ct_menu ct_imgui ct_sched ct_interp ct_runtime
      ct_snes_vendor ct_tables_empty)
cmake --build "$objs" -j "$(nproc)" --target "${libs[@]}"
cmake --build "$objs" -j "$(nproc)" --target ct_sdl_engine >/dev/null 2>&1 || true   # objects only

# The app folder: the kit, with ps5-opengl's native-app link layout.
rm -rf "$app"
mkdir -p "$app"
cp "$KIT/Makefile" "$app/"
for d in runtime sce_sys tooling tools; do mkdir -p "$app/$d"; cp -a "$KIT/$d/." "$app/$d/"; done
mkdir -p "$app/.deps"; cp -a "$KIT/.deps/native" "$app/.deps/native"
asdk=$app/.deps/native/ps5-payload-sdk
cp "$app/tooling/native/ps5-pie.ld" "$app/tooling/native/ps5-pie-base.ld"
cp "$GL_SRC/native-app/ps5-pie.ld" "$GL_SRC/native-app/app-symbols.map" "$app/tooling/native/"
cp "$GL_PREFIX/lib/libSceAgc.so" "$GL_PREFIX/lib/libSceAgcDriver.so" "$asdk/target/lib/"
rm -f "$app/sce_sys/pic0.dds" "$app/sce_sys/pic1.dds" "$app/sce_sys/snd0.at9" \
    "$app/sce_sys/background-source.png" "$app/sce_sys/launch-background-source.png"
cp "$src/packaging/ps5/icon0-hd2d.png" "$app/sce_sys/icon0.png"
mkdir -p "$app/src" "$app/vendor/lib"
cp "$here/title.c" "$app/src/title.c"
cp "$GL_SRC/native-app/runtime_shims.c" "$app/src/runtime_shims.c"

# One relocatable object for the engine; missing weak game hooks are NULL.
cd "$objs"
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
"$sdk/bin/ld.lld" -r -o "$app/vendor/ct_all.tmp.o" $o --whole-archive $a --no-whole-archive
defs=$(readelf -sW "$app/vendor/ct_all.tmp.o" |
       awk '$5=="WEAK" && $7=="UND" && $8 ~ /^ct_/ {print "--defsym=" $8 "=0"}' | sort -u)
"$sdk/bin/ld.lld" -r -o "$app/vendor/ct_all.o" $defs "$app/vendor/ct_all.tmp.o"
rm -f "$app/vendor/ct_all.tmp.o"
"$sdk/bin/llvm-ar" rcs "$app/vendor/lib/libct_title.a" "$app/vendor/ct_all.o"
# Everything else in one linker group: SDL, the GL SDK (its own GROUP
# script), C++ runtime and compiler builtins.
{
    printf 'SEARCH_DIR("%s")\nSEARCH_DIR("%s")\nGROUP (\n' "$asdk/target/lib" "$GL_PREFIX/lib"
    printf '  "%s"\n' "$app/vendor/lib/libct_title.a" "$SDL_PREFIX/lib/libSDL2.a" \
        "$GL_PREFIX/lib/libPS5OpenGLCore33.a" "$asdk/target/lib/libunwind.a" \
        "$asdk/target/lib/libc++abi.a" "$asdk/target/lib/libc++.a" "$builtins"
    printf ')\n'
} > "$app/vendor/libct_group.a"

cd "$app"
make USE_CCACHE=0 init TITLE_ID="$TITLE_ID" APP_NAME="CT5 HD-2D"
python3 "$GL_SRC/tools/native-display-metadata.py" sce_sys/param.json --fps 120
printf '#define CT_IMPORTS(X)\n' > src/imports.gen.h
for pass in 1 2 3 4; do
    make USE_CCACHE=0 APP_STATIC_ARCHIVES=vendor/libct_group.a app
    cp src/imports.gen.h build/imports.prev.h
    python3 "$here/gen_imports.py" build/llvm-pie.elf "$asdk/target/lib" src/imports.gen.h
    cmp -s src/imports.gen.h build/imports.prev.h && break
done
cmp -s src/imports.gen.h build/imports.prev.h || { echo "import list did not settle" >&2; exit 1; }
echo "title folder: $app/dist/$TITLE_ID"
