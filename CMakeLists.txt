cmake_minimum_required(VERSION 3.20)
project(ct_recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ccache, when installed: rebuilding after a branch switch or a clean costs
# only what changed.
find_program(CT_CCACHE ccache)
if(CT_CCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER ${CT_CCACHE})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CT_CCACHE})
endif()

# Investigation builds: -DCMAKE_BUILD_TYPE=Debug (e.g. build-dbg/) builds the
# runtime at -Og -g and the generated code (game/*/out, the bulk of the
# build) at CT_GEN_OPT, -O1 by default: quick to build, fast enough to run a
# replay to where a problem is.
set(CT_GEN_OPT "-O1" CACHE STRING "Optimization for generated code in Debug builds")
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_C_FLAGS_DEBUG MATCHES "-O")
    # -Og: still debuggable, but replays run near Release speed
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Og")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Og")
endif()

# Packaging builds (docs/PACKAGING.md): the engine programs only, with no
# game directory added even when $CT_ROM is set, so nothing generated from a
# ROM can reach the build or `cmake --install`.
option(CT_ENGINE_ONLY "Build only the engine programs, never game code" OFF)
# port: every test runs with its own data and config dirs, never the user's:
# no saved settings (renderer, look, rumble, ...) leak in, and the built-in
# pad reader and rumble stay off, so no test touches real hardware.
set(CT_TEST_XDG ${CMAKE_BINARY_DIR}/test-xdg)
file(MAKE_DIRECTORY ${CT_TEST_XDG}/config)
file(WRITE ${CT_TEST_XDG}/data/ct-recomp/hd2d/settings.ini "hid_deck=off\nrumble=off\n")

# port: Android (docs/ANDROID-BUILD.md), from android/'s Gradle build: the
# engine and frontend as libmain.so (SDL's Java side loads it), SDL2 built
# from source, no Python, tests or install. Implies CT_ENGINE_ONLY.
option(CT_ANDROID "Build libmain.so for the Android app (android/)" OFF)
if(CT_ANDROID)
    set(CT_ENGINE_ONLY ON CACHE BOOL "" FORCE)
else()
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
endif()

set(CT_WARN -Wall -Wextra -Wpedantic)

enable_testing()

# The translator (recomp/), runtime, frontend, and engine tests are
# game-agnostic; each game under game/ supplies its symbols, generated
# code, and ROM-dependent tests. check_agnostic enforces the split.

# ---- vendored SNES core (third_party/snes/, see its README.md) ----
add_library(ct_snes_vendor STATIC
    third_party/snes/ppu.c third_party/snes/dma.c
    third_party/snes/apu.c third_party/snes/spc.c third_party/snes/dsp.c)
target_include_directories(ct_snes_vendor PUBLIC third_party/snes third_party)
# Not our code; don't hold it to our warning flags.

# ---- runtime ----
add_library(ct_runtime STATIC runtime/cpu.c runtime/bus.c
    runtime/snes_adapter.c runtime/png.c runtime/wav.c runtime/hwlog.c
    runtime/cycles.c runtime/replay.c runtime/resample.c
    runtime/spc700_host.cpp
    runtime/func_table.c runtime/module.c   # port: table indirection, game modules
    runtime/rumble.c   # port: simulated rumble
    runtime/sha1.c   # port: ROM identity (first run, save states)
    runtime/savestate_io.c)   # port: save states' serialization
target_include_directories(ct_runtime PUBLIC runtime)
target_link_libraries(ct_runtime PUBLIC ct_snes_vendor m ${CMAKE_DL_LIBS})
# port: our own context switch (runtime/compat/ucontext.h) where the C
# library has no makecontext/swapcontext: Android's bionic, musl. Off on
# glibc, whose build is then exactly as before; ON there tests ours.
include(CheckCSourceCompiles)
check_c_source_compiles([=[
#include <ucontext.h>
static ucontext_t a, b;
static void f(void) { swapcontext(&b, &a); }
int main(void) { static char s[65536]; getcontext(&b); b.uc_stack.ss_sp = s;
    b.uc_stack.ss_size = sizeof s; b.uc_link = 0; makecontext(&b, f, 0);
    return swapcontext(&a, &b); }
]=] CT_HAVE_MAKECONTEXT)
if(ANDROID OR NOT CT_HAVE_MAKECONTEXT)
    set(ct_compat_default ON)
else()
    set(ct_compat_default OFF)
endif()
option(CT_COMPAT_UCONTEXT "Use runtime/compat's context switch, not <ucontext.h>" ${ct_compat_default})
if(CT_COMPAT_UCONTEXT)
    target_sources(ct_runtime PRIVATE runtime/compat/ucontext_aarch64.c
        runtime/compat/ucontext_x86_64.c)
    target_compile_definitions(ct_runtime PUBLIC CT_COMPAT_UCONTEXT)
    message(STATUS "CT_COMPAT_UCONTEXT: runtime/compat context switch")
endif()
# ares's SPC700 core (ISC), compiled inside runtime/spc700_host.cpp.
target_include_directories(ct_runtime PRIVATE third_party/ares/spc700)
target_compile_options(ct_runtime PRIVATE ${CT_WARN})

# Function tables (func_table.h): a game's generated ct_funcs.c, or these
# empty ones. OBJECT libraries, so the tables are always on the link line
# ahead of the interpreter that reads them.
add_library(ct_tables_empty OBJECT runtime/func_table_empty.c runtime/func_table_static.c)
target_compile_definitions(ct_tables_empty PRIVATE CT_TABLES_DEFINE)
target_link_libraries(ct_tables_empty PUBLIC ct_runtime)
target_compile_options(ct_tables_empty PRIVATE ${CT_WARN})

# ---- interpreter: strict test oracle, system mode from reset (interp.h) ----
add_library(ct_interp STATIC runtime/interp.c)
target_link_libraries(ct_interp PUBLIC ct_runtime)
target_compile_options(ct_interp PRIVATE ${CT_WARN})

# ---- frame scheduler (sched.h) ----
add_library(ct_sched STATIC runtime/sched.c runtime/overlay.c
    runtime/savestate.c)   # port: save states (savestate.h)
target_link_libraries(ct_sched PUBLIC ct_interp)
target_compile_options(ct_sched PRIVATE ${CT_WARN})

# ---- SDL2 input mapping (optional: skipped when SDL2 isn't installed) ----
if(CT_ANDROID)
    # port: SDL2 2.32.10 built here as libSDL2.so. android/app/build.gradle
    # unpacks the release tarball (SHA-256 checked) for its Java sources and
    # passes CT_SDL2_SOURCE_DIR; without it CMake fetches the same tarball.
    set(CT_SDL2_SOURCE_DIR "" CACHE PATH "Unpacked SDL2-2.32.10 source (Android)")
    if(NOT EXISTS "${CT_SDL2_SOURCE_DIR}/CMakeLists.txt")   # e.g. an IDE sync before fetchSdl
        include(FetchContent)
        FetchContent_Declare(sdl2_src
            URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
            URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_GetProperties(sdl2_src)
        if(NOT sdl2_src_POPULATED)
            FetchContent_Populate(sdl2_src)
        endif()
        set(CT_SDL2_SOURCE_DIR ${sdl2_src_SOURCE_DIR})
    endif()
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    add_subdirectory(${CT_SDL2_SOURCE_DIR} ${CMAKE_BINARY_DIR}/sdl2 EXCLUDE_FROM_ALL)
    set(SDL2_FOUND TRUE)
else()
    find_package(SDL2 CONFIG QUIET)
endif()
# Hosts with the SDL2 runtime but no development package (immutable distros
# such as Bazzite): take the headers from the official release tarball and
# link the system library. Packaging builds (Flatpak, AppImage) have a full
# SDK and use find_package or a source build instead.
option(CT_SDL2_SYSTEM_LIB "Use the system libSDL2 with release-tarball headers" ON)
if(NOT SDL2_FOUND AND CT_SDL2_SYSTEM_LIB)
    find_library(CT_SDL2_LIB NAMES SDL2-2.0 libSDL2-2.0.so.0)
    if(CT_SDL2_LIB)
        include(FetchContent)
        FetchContent_Declare(sdl2_headers
            URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
            URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_GetProperties(sdl2_headers)
        if(NOT sdl2_headers_POPULATED)
            FetchContent_Populate(sdl2_headers)   # headers only: never built
        endif()
        add_library(SDL2::SDL2 UNKNOWN IMPORTED)
        set_target_properties(SDL2::SDL2 PROPERTIES
            IMPORTED_LOCATION ${CT_SDL2_LIB}
            INTERFACE_INCLUDE_DIRECTORIES ${sdl2_headers_SOURCE_DIR}/include)
        set(SDL2_FOUND TRUE)
        message(STATUS "SDL2: system ${CT_SDL2_LIB}, headers from 2.32.10")
    endif()
endif()
# port: asset packs (frontend/packs, docs/ASSET-PACKS.md): content keys,
# the dump, PNG reading, the atlas. No SDL: unit-tested on its own.
# music.c: audio packs ([[music]], music.h), with the decoders' vendored
# implementations in codecs.c (third_party/stb, third_party/dr_libs),
# built without our warnings.
add_library(ct_packs STATIC frontend/packs/packs.c frontend/packs/pngio.c
    frontend/packs/packs_model.c frontend/packs/music.c frontend/packs/codecs.c)
target_include_directories(ct_packs PUBLIC frontend/packs runtime
    PRIVATE third_party/cgltf third_party/stb third_party/dr_libs)
target_compile_options(ct_packs PRIVATE ${CT_WARN})
set_source_files_properties(frontend/packs/codecs.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(ct_packs PUBLIC m)
# port: HD text (frontend/hdtext.h): the player's font over the game's text
# boxes; stb_truetype's implementation in hdtext_ttf.c, without our warnings.
add_library(ct_hdtext STATIC frontend/hdtext.c frontend/hdtext_ttf.c)
target_include_directories(ct_hdtext PUBLIC frontend runtime PRIVATE third_party/stb)
target_compile_options(ct_hdtext PRIVATE ${CT_WARN})
set_source_files_properties(frontend/hdtext_ttf.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(ct_hdtext PUBLIC m)
if(SDL2_FOUND)
    # port: the on-screen touch controller (touch.h) and its GL drawer.
    add_library(ct_input STATIC frontend/input.c frontend/touch.c frontend/touch_gl.c
        frontend/device.c   # port: device.c: Steam Game Mode, Steam Deck (HANDHELD.md)
        frontend/hid_deck.c # port: Deck-format hidraw pads SDL misses (InputPlumber)
        frontend/rumble_out.c # port: simulated rumble on the controllers
        # port: frontend actions, their bindings, button glyphs in messages
        frontend/actions.c frontend/bindings.c frontend/glyphs.c frontend/osd_text.c
        frontend/settings.c    # port: the settings registry (settings.h)
        frontend/overlays.c    # port: the corner overlays' data (overlays.h; needs qol.c)
        frontend/timectl.c frontend/timectl_ring.c)   # port: rewind, slow motion, frame step (timectl.h)
    target_include_directories(ct_input PUBLIC frontend)
    target_include_directories(ct_input PRIVATE runtime)   # port: rumble.h (rumble_out.c)
    target_link_libraries(ct_input PUBLIC SDL2::SDL2 m)
    target_compile_options(ct_input PRIVATE ${CT_WARN})
    # port: the button glyphs (third_party/glyphs, Kenney Input Prompts,
    # CC0) as coverage masks in a generated header. The Android build has
    # no Python: its messages name the buttons in text.
    if(NOT CT_ANDROID)
        file(GLOB CT_GLYPH_PNGS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/glyphs/*.png)
        set(CT_GLYPHS_GEN ${CMAKE_BINARY_DIR}/glyphs_gen/glyphs_data.h)
        add_custom_command(
            OUTPUT ${CT_GLYPHS_GEN}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/glyphs_gen
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/embed_glyphs.py
                    ${CT_GLYPHS_GEN} 44 ${CT_GLYPH_PNGS}
            DEPENDS ${CT_GLYPH_PNGS} ${CMAKE_SOURCE_DIR}/tools/embed_glyphs.py
            COMMENT "button glyphs"
            VERBATIM)
        target_sources(ct_input PRIVATE ${CT_GLYPHS_GEN})
        target_include_directories(ct_input PRIVATE ${CMAKE_BINARY_DIR}/glyphs_gen)
        target_compile_definitions(ct_input PRIVATE CT_GLYPHS_DATA)
    endif()

    # chrono-recomp: HD-2D renderer (frontend/hd2d), OpenGL 3.3 / ES 3.0
    # through SDL; shaders are embedded as strings at build time.
    set(CT_HD2D_SHADERS fullscreen.vert emit.frag light.frag upscale.frag coc.frag dofblur.frag
        bright.frag gauss.frag final.frag
        scene3d.vert scene3d.frag scene3d_pic.frag ui3d.frag   # the 3D diorama (scene3d.h)
        aoprep.frag ssao.frag aoblur.frag fsr_easu.frag)   # its ambient occlusion and FSR 1 upscale
    list(TRANSFORM CT_HD2D_SHADERS PREPEND ${CMAKE_SOURCE_DIR}/frontend/hd2d/shaders/)
    # AMD FidelityFX FSR 1 and CAS, ported to GLSL 3.30 / ES 3.00 (MIT).
    list(APPEND CT_HD2D_SHADERS ${CMAKE_SOURCE_DIR}/third_party/fidelityfx/ffx_fsr1_cas.glsl)
    set(CT_HD2D_GEN ${CMAKE_BINARY_DIR}/hd2d_gen/hd2d_shaders.h)
    if(CT_ANDROID)   # port: no Python: the same header from a CMake script
        add_custom_command(
            OUTPUT ${CT_HD2D_GEN}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/hd2d_gen
            COMMAND ${CMAKE_COMMAND} -DOUT=${CT_HD2D_GEN} -DPREFIX=hd2d_src
                    "-DFILES=${CT_HD2D_SHADERS}" -P ${CMAKE_SOURCE_DIR}/tools/embed_text.cmake
            DEPENDS ${CT_HD2D_SHADERS} ${CMAKE_SOURCE_DIR}/tools/embed_text.cmake
            COMMENT "hd2d shaders"
            VERBATIM)
    else()
    add_custom_command(
        OUTPUT ${CT_HD2D_GEN}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/hd2d_gen
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed_text.py ${CT_HD2D_GEN}
                hd2d_src ${CT_HD2D_SHADERS}
        DEPENDS ${CT_HD2D_SHADERS} ${CMAKE_SOURCE_DIR}/tools/embed_text.py
        COMMENT "hd2d shaders"
        VERBATIM)
    endif()
    add_library(ct_hd2d STATIC frontend/hd2d/hd2d.c frontend/hd2d/scene3d.c
        frontend/hd2d/scene3d_world.c ${CT_HD2D_GEN})
    target_include_directories(ct_hd2d PUBLIC frontend PRIVATE ${CMAKE_BINARY_DIR}/hd2d_gen)
    target_link_libraries(ct_hd2d PUBLIC SDL2::SDL2 ct_runtime ct_input m)   # port: ct_input: osd_text.h
    target_compile_options(ct_hd2d PRIVATE ${CT_WARN} -Wno-overlength-strings)

    # port: the in-game settings menu (frontend/menu.h) and Dear ImGui
    # (third_party/imgui, MIT, vendored unmodified), with its SDL2,
    # OpenGL 3 / ES 3 and SDL_Renderer backends.
    set(CT_IMGUI ${CMAKE_SOURCE_DIR}/third_party/imgui)
    add_library(ct_imgui STATIC ${CT_IMGUI}/imgui.cpp ${CT_IMGUI}/imgui_draw.cpp
        ${CT_IMGUI}/imgui_tables.cpp ${CT_IMGUI}/imgui_widgets.cpp
        ${CT_IMGUI}/backends/imgui_impl_sdl2.cpp ${CT_IMGUI}/backends/imgui_impl_opengl3.cpp
        ${CT_IMGUI}/backends/imgui_impl_sdlrenderer2.cpp)
    target_include_directories(ct_imgui PUBLIC ${CT_IMGUI} ${CT_IMGUI}/backends)
    target_compile_definitions(ct_imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS
        IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(ct_imgui PUBLIC SDL2::SDL2 ${CMAKE_DL_LIBS})
    if(CT_ANDROID)
        target_compile_definitions(ct_imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
        target_link_libraries(ct_imgui PUBLIC GLESv3)
    endif()
    # The menu's font (Roboto Medium, Apache-2.0) as a byte array.
    set(CT_MENU_FONT_GEN ${CMAKE_BINARY_DIR}/menu_gen/menu_font.h)
    add_custom_command(
        OUTPUT ${CT_MENU_FONT_GEN}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/menu_gen
        COMMAND ${CMAKE_COMMAND} -DIN=${CT_IMGUI}/fonts/Roboto-Medium.ttf -DOUT=${CT_MENU_FONT_GEN}
                -DNAME=menu_font -P ${CMAKE_SOURCE_DIR}/tools/embed_binary.cmake
        DEPENDS ${CT_IMGUI}/fonts/Roboto-Medium.ttf ${CMAKE_SOURCE_DIR}/tools/embed_binary.cmake
        COMMENT "menu font"
        VERBATIM)
    add_library(ct_menu STATIC frontend/menu.cpp frontend/ui_backend.cpp frontend/quickmenu.cpp
        frontend/overlays_draw.cpp ${CT_MENU_FONT_GEN})
    target_include_directories(ct_menu PUBLIC frontend PRIVATE ${CMAKE_BINARY_DIR}/menu_gen)
    target_compile_definitions(ct_menu PRIVATE CT_MENU_FONT)
    # The version the menu's About tab shows (the release in
    # packaging/*.metainfo.xml), with the commit the build was configured at.
    set(CT_VERSION_STRING "0.1.0")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE ct_git_rev
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(ct_git_rev)
            string(APPEND CT_VERSION_STRING " (${ct_git_rev})")
        endif()
    endif()
    target_compile_definitions(ct_menu PUBLIC CT_VERSION="${CT_VERSION_STRING}")
    target_link_libraries(ct_menu PUBLIC ct_imgui ct_input ct_runtime)
    target_compile_options(ct_menu PRIVATE ${CT_WARN})
else()
    if(CT_ENGINE_ONLY)
        message(FATAL_ERROR "CT_ENGINE_ONLY needs SDL2 for ct_sdl_engine")
    endif()
    message(STATUS "SDL2 not found: frontends not built")
endif()

# ---- engine-only programs (docs/MODULES.md) ----
# The runtime and the empty function tables, no generated code; the game
# code comes from a module loaded with --module.
function(ct_engine_exe name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/test)
    # Whole archives: the module calls into parts of the engine the
    # program itself may not, so every engine object must be present and
    # exported (ENABLE_EXPORTS: -rdynamic).
    target_link_libraries(${name} ct_tables_empty
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,ct_sched,ct_interp,ct_runtime>")
    target_compile_options(${name} PRIVATE ${CT_WARN})
    set_target_properties(${name} PROPERTIES ENABLE_EXPORTS ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endfunction()
if(CT_ANDROID)
    # port: the engine-only SDL frontend as libmain.so, which SDLActivity
    # loads (it calls SDL_main: SDL.h renames main). No first run (no
    # Python or compiler on a phone): the app passes --rom and --module
    # (android/, ImportActivity). Every engine object is linked in and
    # exported, as for ct_sdl_engine, for the module to bind to.
    add_library(main SHARED ${CMAKE_SOURCE_DIR}/frontend/main.c
        ${CMAKE_SOURCE_DIR}/frontend/qol.c   # port: quality of life (qol.h)
        ${CMAKE_SOURCE_DIR}/frontend/padtest.c   # port: the controller test (padtest.h)
        ${CMAKE_SOURCE_DIR}/frontend/pack_scene.c)   # port: asset packs in the scene
    target_include_directories(main PRIVATE ${CMAKE_SOURCE_DIR}/test)
    target_link_libraries(main ct_tables_empty
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,ct_sched,ct_interp,ct_runtime>" ct_input ct_hd2d
        ct_packs ct_hdtext ct_menu SDL2::SDL2 android log)
    target_compile_options(main PRIVATE ${CT_WARN})
    return()   # no tests, games or install() rules on Android
endif()
ct_engine_exe(ct_boot_engine ${CMAKE_SOURCE_DIR}/tools/ct_boot.c ${CMAKE_SOURCE_DIR}/tools/tas_resync.c)
target_link_libraries(ct_boot_engine ct_packs)   # port: --music-pack (music.h)
if(SDL2_FOUND)
    # port: without --module it runs the first-run flow (frontend/firstrun.h):
    # find the ROM, build the module with tools/build_module.py, load it.
    # Packagers point CT_ENGINE_DATA_DIR at the installed data dir
    # (<prefix>/share/ct-recomp); the launcher also passes --data-dir.
    set(CT_ENGINE_DATA_DIR ${CMAKE_SOURCE_DIR} CACHE PATH
        "Where ct_sdl_engine finds game/ and tools/ by default (--data-dir)")
    # The game it builds by default: the first game/<id> with a game.toml.
    if(NOT CT_GAME_ID)
        file(GLOB ct_game_tomls ${CMAKE_SOURCE_DIR}/game/*/game.toml)
        list(GET ct_game_tomls 0 ct_first_toml)
        get_filename_component(ct_first_game ${ct_first_toml} DIRECTORY)
        get_filename_component(ct_first_game ${ct_first_game} NAME)
        set(CT_GAME_ID ${ct_first_game} CACHE STRING "Default game for ct_sdl_engine (--game)")
    endif()
    ct_engine_exe(ct_sdl_engine ${CMAKE_SOURCE_DIR}/frontend/main.c
        ${CMAKE_SOURCE_DIR}/frontend/firstrun.c ${CMAKE_SOURCE_DIR}/frontend/saves.c
        ${CMAKE_SOURCE_DIR}/frontend/qol.c   # port: quality of life (qol.h)
        ${CMAKE_SOURCE_DIR}/frontend/padtest.c   # port: the controller test (padtest.h)
        ${CMAKE_SOURCE_DIR}/frontend/pack_scene.c)   # port: asset packs in the scene
    target_link_libraries(ct_sdl_engine ct_input ct_hd2d ct_packs ct_hdtext ct_menu)
    target_compile_definitions(ct_sdl_engine PRIVATE CT_FIRSTRUN
        CT_FIRSTRUN_GAME="${CT_GAME_ID}" CT_FIRSTRUN_DATA_DIR="${CT_ENGINE_DATA_DIR}")
endif()

# ---- engine tests: no game, no ROM (synthetic image, test/harness.h) ----
set(CT_TEST_ROM ${CMAKE_BINARY_DIR}/test_rom.sfc)
add_custom_command(
    OUTPUT ${CT_TEST_ROM}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/make_test_rom.py ${CT_TEST_ROM}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/make_test_rom.py
    COMMENT "synthetic test ROM"
    VERBATIM)
add_custom_target(ct_test_rom DEPENDS ${CT_TEST_ROM})

function(ct_engine_test name src)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE test)
    target_link_libraries(${name} ct_sched ct_tables_empty)
    target_compile_options(${name} PRIVATE ${CT_WARN})
    target_compile_definitions(${name} PRIVATE CT_TEST_ROM="${CT_TEST_ROM}")
    add_dependencies(${name} ct_test_rom)
endfunction()

ct_engine_test(test_runtime test/test_runtime.c)
add_test(NAME runtime COMMAND test_runtime)
foreach(f open_bus rom_write unhooked entry)
    add_test(NAME runtime_fatal_${f} COMMAND test_runtime fatal_${f})
endforeach()
set_tests_properties(runtime_fatal_open_bus PROPERTIES PASS_REGULAR_EXPRESSION "ct: read8 \\$006000: open bus")
set_tests_properties(runtime_fatal_rom_write PROPERTIES PASS_REGULAR_EXPRESSION "ct: write8 \\$E50000: write to ROM")
set_tests_properties(runtime_fatal_unhooked PROPERTIES PASS_REGULAR_EXPRESSION "ct: read8 \\$004016: unhooked")
set_tests_properties(runtime_fatal_entry PROPERTIES PASS_REGULAR_EXPRESSION "ct: \\$C08100: entry state m0 x0 e0, expected m1 x0")

foreach(t snes_bus hw_map interp_modes interp_system sched stack_guard replay resample dsp
        decimal rumble)   # port: decimal (test/test_decimal.c), rumble (test/test_rumble.c)
    ct_engine_test(test_${t} test/test_${t}.c)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

# port: the coroutine context calls (runtime/compat/ucontext.h), ours or libc's.
add_executable(test_ucontext test/test_ucontext.c)
target_link_libraries(test_ucontext ct_runtime)
target_compile_options(test_ucontext PRIVATE ${CT_WARN})
add_test(NAME ucontext COMMAND test_ucontext)
if(CT_COMPAT_UCONTEXT)
    add_test(NAME ucontext_returns COMMAND test_ucontext returns)
    set_tests_properties(ucontext_returns PROPERTIES
        PASS_REGULAR_EXPRESSION "makecontext coroutine returned with no uc_link")
endif()

# Native dispatch: provides its own function table instead of the empty one.
add_executable(test_native test/test_native.c runtime/func_table_static.c)
target_compile_definitions(test_native PRIVATE CT_TABLES_DEFINE)
target_include_directories(test_native PRIVATE test)
target_link_libraries(test_native ct_sched)
target_compile_options(test_native PRIVATE ${CT_WARN})
target_compile_definitions(test_native PRIVATE CT_TEST_ROM="${CT_TEST_ROM}")
add_dependencies(test_native ct_test_rom)
add_test(NAME native COMMAND test_native)

# tools/lockstep.py's divergence report, against a fake probe.
add_test(NAME lockstep_report COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_lockstep.py)
add_test(NAME ref_compare_report COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_ref_compare.py)
add_test(NAME ctest_cache_report COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_ctest_cache.py)

# TAS movie converter on synthetic .bk2/.lsmv movies.
add_test(NAME tas_convert COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_tas_convert.py)
add_executable(test_packs test/test_packs.c)   # port: asset packs
target_link_libraries(test_packs ct_packs ct_runtime)
target_compile_options(test_packs PRIVATE ${CT_WARN} -Wno-format-truncation)
add_test(NAME packs COMMAND test_packs ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})
# port: the installed data dir can build a module (every header the game's C
# includes is installed); a gap here breaks every packaged first run.
add_test(NAME install_layout COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_install_layout.py
         ${CMAKE_COMMAND} ${CMAKE_BINARY_DIR})
# port: audio packs (music.h): synthetic tones in a test pack; the
# decoders on tones ffmpeg makes (skipped without it).
add_executable(test_music test/test_music.c)
target_link_libraries(test_music ct_packs ct_runtime)
target_compile_options(test_music PRIVATE ${CT_WARN})
add_test(NAME music COMMAND test_music ${CMAKE_BINARY_DIR}/test_music_tmp)
add_test(NAME import_msu1 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_import_msu1.py)
add_test(NAME music_codecs COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_music_codecs.py
    $<TARGET_FILE:test_music> ${CMAKE_BINARY_DIR}/test_music_codecs_tmp)
set_tests_properties(music_codecs PROPERTIES SKIP_RETURN_CODE 77)
# port: HD text (hdtext.h) on a made-up text box, with a font found on the
# system at run time (skipped without one).
add_executable(test_hdtext test/test_hdtext.c)
target_link_libraries(test_hdtext ct_hdtext)
target_compile_options(test_hdtext PRIVATE ${CT_WARN})
add_test(NAME hdtext COMMAND test_hdtext)
set_tests_properties(hdtext PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME pack_upscale COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_pack_upscale.py)   # port: asset packs
add_test(NAME decode_calls COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_decode_calls.py)
add_test(NAME funcs_meta COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_funcs_meta.py)
add_test(NAME replay_gen COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_replay_gen.py)   # port: tools/replay_gen.py

if(SDL2_FOUND)
    add_executable(test_input test/test_input.c)
    target_link_libraries(test_input ct_input)
    target_compile_options(test_input PRIVATE ${CT_WARN})
    add_test(NAME input COMMAND test_input)
    # port: the touch overlay's input logic (frontend/touch.h)
    add_executable(test_touch test/test_touch.c)
    target_link_libraries(test_touch ct_input)
    target_compile_options(test_touch PRIVATE ${CT_WARN})
    add_test(NAME touch COMMAND test_touch)
    # port: handheld controls through an SDL virtual game controller
    add_executable(test_handheld test/test_handheld.c)
    target_link_libraries(test_handheld ct_input)
    target_compile_options(test_handheld PRIVATE ${CT_WARN})
    add_test(NAME handheld COMMAND test_handheld)
    # port: the Deck-format hidraw reader (frontend/hid_deck.h): reports
    # made from the documented layout, through a fake sysfs and a FIFO
    add_executable(test_hid_deck test/test_hid_deck.c)
    target_link_libraries(test_hid_deck ct_input ct_runtime)   # port: ct_runtime: rumble.h
    target_compile_options(test_hid_deck PRIVATE ${CT_WARN})
    add_test(NAME hid_deck COMMAND test_hid_deck ${CMAKE_BINARY_DIR}/test_hid_deck_tmp)
    # port: back buttons -> frontend actions, glyphs, gyro, through SDL
    # virtual controllers with paddles
    add_executable(test_actions test/test_actions.c)
    target_link_libraries(test_actions ct_input ct_runtime)
    target_compile_options(test_actions PRIVATE ${CT_WARN})
    add_test(NAME actions COMMAND test_actions)
    # port: the quality-of-life helpers (frontend/qol.h): battery, pictures
    # folder, settings, turbo, the plain picture's rectangle
    add_executable(test_qol test/test_qol.c frontend/qol.c frontend/saves.c)
    target_include_directories(test_qol PRIVATE test)
    target_link_libraries(test_qol ct_input ct_hd2d ct_sched ct_tables_empty ct_packs)   # port: ct_packs: the menu's thumbnails
    target_compile_options(test_qol PRIVATE ${CT_WARN})
    add_test(NAME qol COMMAND test_qol)
    # port: time controls (frontend/timectl.h): the rewind ring's delta and
    # caps, the speed steps and pacing schedule, the actions, no ROM
    add_executable(test_timectl test/test_timectl.c)
    target_link_libraries(test_timectl ct_input ct_sched ct_tables_empty)
    target_compile_options(test_timectl PRIVATE ${CT_WARN})
    add_test(NAME timectl COMMAND test_timectl)
    # port: the settings registry the menu shows (frontend/settings.h)
    # port: overlays' data, backups of the save file, touch / controls rows
    add_executable(test_q3 test/test_q3.c frontend/qol.c frontend/saves.c)
    target_include_directories(test_q3 PRIVATE test)
    target_link_libraries(test_q3 ct_input ct_hd2d ct_sched ct_tables_empty ct_packs)
    target_compile_options(test_q3 PRIVATE ${CT_WARN})
    add_test(NAME q3 COMMAND test_q3)

    add_executable(test_settings test/test_settings.c)
    target_link_libraries(test_settings ct_input)
    target_compile_options(test_settings PRIVATE ${CT_WARN})
    add_test(NAME settings COMMAND test_settings)
    # port: the F1 quick menu (frontend/quickmenu.h): focus, pad, key, mouse and
    # touch, the tiles' effects and the sheet drawn at 1280x800 and 800x1280, on
    # SDL's dummy video driver with a software renderer; no ROM.
    add_executable(test_quickmenu test/test_quickmenu.c)
    target_link_libraries(test_quickmenu ct_menu SDL2::SDL2)
    target_compile_options(test_quickmenu PRIVATE ${CT_WARN})
    add_test(NAME quickmenu COMMAND test_quickmenu ${CMAKE_BINARY_DIR})
    set_tests_properties(quickmenu PROPERTIES SKIP_RETURN_CODE 77
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    # The 3D diorama's geometry from a scene (frontend/hd2d/scene3d.h)
    add_executable(test_scene3d test/test_scene3d.c)
    target_link_libraries(test_scene3d ct_hd2d)
    target_compile_options(test_scene3d PRIVATE ${CT_WARN})
    add_test(NAME scene3d COMMAND test_scene3d)
endif()

# No game-specific names, addresses, or words outside game/.
add_test(NAME check_agnostic
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_agnostic.py ${CMAKE_SOURCE_DIR})

# ---- games (need $CT_ROM; their tests are LABELS rom) ----
if(CT_ENGINE_ONLY)
    message(STATUS "CT_ENGINE_ONLY: game builds and ROM tests skipped")
elseif(DEFINED ENV{CT_ROM})
    set(CT_ROM $ENV{CT_ROM})
    add_subdirectory(game/ct)
    # port: rewind on the real game (frontend/timectl_ring.h): the delta
    # chain is lossless, and a rewind then play forward on the same input
    # matches the original run's state hashes. LABELS rom (set below).
    add_executable(test_timectl_rom test/test_timectl_rom.c frontend/timectl_ring.c)
    target_include_directories(test_timectl_rom PRIVATE test frontend)
    target_link_libraries(test_timectl_rom ct_sched ct_gen_tables ct_gen)
    target_compile_options(test_timectl_rom PRIVATE ${CT_WARN})
    add_test(NAME timectl_rewind_replay COMMAND test_timectl_rom
        ${CMAKE_SOURCE_DIR}/game/ct/test/replay/leene_square.txt 8500 900 20)
    set_tests_properties(timectl_rewind_replay PROPERTIES LABELS rom ENVIRONMENT
        "CT_ROM=${CT_ROM};SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
else()
    message(STATUS "CT_ROM not set: game builds and ROM tests skipped")
endif()

# ---- install (docs/PACKAGING.md): the engine and its data, nothing else ----
# Never ct_game.so, ct_sdl, ct_boot or anything under a build tree: those hold
# code generated from a ROM. The data dir is what tools/build_module.py
# needs to build a module on the user's machine.
if(TARGET ct_sdl_engine)
    include(GNUInstallDirs)
    set(CT_DATA_DIR ${CMAKE_INSTALL_DATADIR}/ct-recomp)
    install(TARGETS ct_sdl_engine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(PROGRAMS packaging/ct-recomp.sh DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME ct-recomp)
    install(PROGRAMS packaging/zig-cc.sh DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/ct-recomp
        RENAME zig-cc)
    file(GLOB ct_inst_tools ${CMAKE_SOURCE_DIR}/tools/*.py)
    file(GLOB ct_inst_recomp ${CMAKE_SOURCE_DIR}/recomp/*.py)
    file(GLOB ct_inst_hdrs ${CMAKE_SOURCE_DIR}/runtime/*.h)
    install(FILES ${ct_inst_tools} DESTINATION ${CT_DATA_DIR}/tools)
    install(FILES ${ct_inst_recomp} DESTINATION ${CT_DATA_DIR}/recomp)
    install(FILES ${ct_inst_hdrs} runtime/module_entry.c DESTINATION ${CT_DATA_DIR}/runtime)
    # Each game: its symbol and config files, extern hooks, and the build
    # scripts build_module.py runs (overlay decoding). No tests or replays.
    file(GLOB ct_games LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/game/*)
    foreach(g ${ct_games})
        if(EXISTS ${g}/game.toml)
            get_filename_component(gid ${g} NAME)
            file(GLOB ct_inst_game ${g}/*.toml ${g}/*.c ${g}/*.h)
            file(GLOB ct_inst_game_tools ${g}/tools/*.py)
            install(FILES ${ct_inst_game} DESTINATION ${CT_DATA_DIR}/game/${gid})
            install(FILES ${ct_inst_game_tools} DESTINATION ${CT_DATA_DIR}/game/${gid}/tools)
        endif()
    endforeach()
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/ct-recomp)
    install(DIRECTORY LICENSES/ DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/ct-recomp/LICENSES)
    install(FILES THIRD_PARTY.md DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/ct-recomp)
    set(CT_APP_ID io.github.dittobyte.CtRecomp)
    install(FILES packaging/${CT_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    # The icon (packaging/icons/make_icons.py writes the SVG and the PNGs).
    install(FILES packaging/icons/ct-recomp.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME ${CT_APP_ID}.svg)
    foreach(sz 16 24 32 48 64 128 256 512)
        install(FILES packaging/icons/${sz}x${sz}/ct-recomp.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${sz}x${sz}/apps
            RENAME ${CT_APP_ID}.png)
    endforeach()
    install(FILES packaging/${CT_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
endif()

# ---- test environment (keep last) ----
# SDL dummy drivers: the frontend test runs headless.
get_property(ct_engine_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${ct_engine_tests} PROPERTIES ENVIRONMENT
    "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;XDG_DATA_HOME=${CT_TEST_XDG}/data;XDG_CONFIG_HOME=${CT_TEST_XDG}/config")
