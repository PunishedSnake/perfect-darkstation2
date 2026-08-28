cmake_minimum_required(VERSION 3.16)

# Keep the bootstrap self-contained. The official PS2DEV image installs the
# toolchain and PS2SDK, but not PS2SDK's samples/ directory.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/ps2dev-toolchain.cmake"
      CACHE FILEPATH "PS2SDK-compatible EE CMake toolchain" FORCE)
endif()

project(pd_ps2_bootstrap C CXX)

option(PD_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT
  "Enable alpha-trilerp pass graph before real-PS2 channel-blit A/B validation"
  OFF
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(PD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# CI exposes the exact checkout revision directly. Prefer that over invoking
# git from inside the minimal PS2DEV container; local builds fall back to git.
set(PD_PS2_GIT_COMMIT "")
if(DEFINED ENV{GITHUB_SHA} AND NOT "$ENV{GITHUB_SHA}" STREQUAL "")
  string(SUBSTRING "$ENV{GITHUB_SHA}" 0 12 PD_PS2_GIT_COMMIT)
else()
  execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${PD_ROOT}"
    OUTPUT_VARIABLE PD_PS2_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()
if(PD_PS2_GIT_COMMIT STREQUAL "")
  set(PD_PS2_GIT_COMMIT "unknown")
endif()

if(NOT DEFINED ENV{GSKIT})
  message(FATAL_ERROR "GSKIT environment variable is required for the PS2 diagnostic renderer")
endif()
set(GSKIT "$ENV{GSKIT}")

if(NOT EXISTS "${GSKIT}/include/gsKit.h")
  message(FATAL_ERROR "Current gsKit headers not found under ${GSKIT}/include")
endif()

find_program(PD_PS2_PYTHON3 NAMES python3 REQUIRED)

set(PD_PS2_ROMID "ntsc-final")
set(PD_PS2_ASSET_DIR "${PD_ROOT}/src/assets/${PD_PS2_ROMID}")
set(PD_PS2_GENERATED_DIR "${PD_ROOT}/src/generated/${PD_PS2_ROMID}")

set(PD_PS2_COMMON_INCLUDE_DIRS
  "${CMAKE_CURRENT_LIST_DIR}"
  "${PD_ROOT}/include"
  "${PD_ROOT}/include/PR"
  "${PD_ROOT}/src/include"
  "${PD_ROOT}/src/lib/ultra/audio"
  "${PD_ROOT}/port/include"
  "${PD_ROOT}/port/fast3d"
)

set(PD_PS2_CORRECTNESS_FLAGS
  -Og
  -G0
  -fno-strict-aliasing
  -funsigned-char
  -fwrapv
)

set(PD_PS2_GAME_DEFINITIONS
  PLATFORM_PS2=1
  MATCHING=0
  PIRACYCHECKS=0
  ROM_SIZE=32
  AVOID_UB=1
  _LANGUAGE_C=1
  PAL=0
  VERSION=2
)

# The game compile depends on generated enum headers even before any binary
# asset payload is linked. Keep this headers-only: it exercises the real source
# contract without introducing runtime asset transport or copies prematurely.
function(pd_ps2_generate_asset_header json_name tool_name header_name)
  set(input "${PD_PS2_ASSET_DIR}/${json_name}")
  set(output "${PD_PS2_GENERATED_DIR}/${header_name}")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E env "ROMID=${PD_PS2_ROMID}"
      "${PD_PS2_PYTHON3}" "${PD_ROOT}/tools/assetmgr/${tool_name}"
      "${input}" ${ARGN} --headers-only "--romid=${PD_PS2_ROMID}"
    DEPENDS
      "${input}"
      "${PD_ROOT}/tools/assetmgr/${tool_name}"
      "${PD_ROOT}/tools/assetmgr/assetmgr.py"
    WORKING_DIRECTORY "${PD_ROOT}"
    VERBATIM
  )
  set(PD_PS2_GENERATED_HEADERS ${PD_PS2_GENERATED_HEADERS} "${output}" PARENT_SCOPE)
endfunction()

set(PD_PS2_GENERATED_HEADERS "")
pd_ps2_generate_asset_header("animations.json" "mkanims" "animations.h")
pd_ps2_generate_asset_header("sequences.json" "mksequences" "sequences.h")
pd_ps2_generate_asset_header("textures.json" "mktextures" "textures.h")

file(GLOB PD_PS2_LANG_JSON CONFIGURE_DEPENDS "${PD_PS2_ASSET_DIR}/lang/*.json")
foreach(lang_json IN LISTS PD_PS2_LANG_JSON)
  get_filename_component(lang_name "${lang_json}" NAME_WE)
  pd_ps2_generate_asset_header(
    "lang/${lang_name}.json" "mklang" "lang/${lang_name}.h" "en")
endforeach()

file(GLOB PD_PS2_PADS_JSON CONFIGURE_DEPENDS "${PD_PS2_ASSET_DIR}/pads/*.json")
foreach(pads_json IN LISTS PD_PS2_PADS_JSON)
  get_filename_component(pads_name "${pads_json}" NAME_WE)
  pd_ps2_generate_asset_header(
    "pads/${pads_name}.json" "mkpads" "pads/${pads_name}.h")
endforeach()

file(GLOB PD_PS2_TILES_JSON CONFIGURE_DEPENDS "${PD_PS2_ASSET_DIR}/tiles/*.json")
foreach(tiles_json IN LISTS PD_PS2_TILES_JSON)
  get_filename_component(tiles_name "${tiles_json}" NAME_WE)
  pd_ps2_generate_asset_header(
    "tiles/${tiles_name}.json" "mktiles" "tiles/${tiles_name}.h")
endforeach()

add_custom_target(pd_ps2_asset_headers DEPENDS ${PD_PS2_GENERATED_HEADERS})

# Keep shared Fast3D byte-for-byte intact. The PS2 bootstrap generates a narrow
# frontend copy with ordered live-TMEM hooks at the existing gfx_dp_* execution
# points. Exact-match replacement makes upstream drift a build error instead of
# silently compiling a partially instrumented texture path.
set(PD_PS2_FAST3D_LIVE_CPP
  "${CMAKE_CURRENT_BINARY_DIR}/generated/gfx_pc_ps2_live.cpp"
)
add_custom_command(
  OUTPUT "${PD_PS2_FAST3D_LIVE_CPP}"
  COMMAND "${PD_PS2_PYTHON3}"
    "${CMAKE_CURRENT_LIST_DIR}/patch_fast3d_tmem_live.py"
    "${PD_ROOT}/port/fast3d/gfx_pc.cpp"
    "${PD_PS2_FAST3D_LIVE_CPP}"
  DEPENDS
    "${PD_ROOT}/port/fast3d/gfx_pc.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/patch_fast3d_tmem_live.py"
  WORKING_DIRECTORY "${PD_ROOT}"
  COMMENT "Generating PS2 Fast3D frontend with ordered live TMEM hooks"
  VERBATIM
)
set_source_files_properties(
  "${PD_PS2_FAST3D_LIVE_CPP}"
  PROPERTIES
    GENERATED TRUE
    COMPILE_DEFINITIONS
      "gfx_start_frame=gfx_start_frame_ps2_impl;gfx_run=gfx_run_ps2_impl;TEXTURE_CACHE_MAX_SIZE=64"
)

add_executable(pd_ps2_bootstrap
  bootstrap.c
  system_ps2.c
  video_diag.c
  gs_command_budget.cpp
  gs_core.cpp
  gs_native_queue.cpp
  gs_render_target_layout.cpp
  gs_texture_convert.cpp
  gs_vram_allocator.cpp
  gfx_api_scene.c
  gfx_frame_ps2.c
  gfx_run_tmem_ps2.cpp
  gfx_ps2.cpp
  gfx_ps2_combiner.cpp
  gfx_ps2_pass_graph.cpp
  gfx_window_ps2.c
  pad_ps2.c
  controls_ps2.c
  rom_probe.c
  "${PD_ROOT}/port/src/romsource.c"
  "${PD_ROOT}/port/fast3d/gfx_cc.cpp"
  "${PD_PS2_FAST3D_LIVE_CPP}"
  "${PD_ROOT}/port/fast3d/rdp_tmem.cpp"
  "${PD_ROOT}/port/fast3d/rdp_tmem_runtime.cpp"
  "${PD_ROOT}/port/fast3d/rdp_tmem_live.cpp"
  "${PD_ROOT}/port/fast3d/rdp_tmem_trace.cpp"
)

if(PD_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
  target_compile_definitions(pd_ps2_bootstrap PRIVATE
    PERFECT_DARK_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT=1
  )
endif()

target_include_directories(pd_ps2_bootstrap PRIVATE
  ${PD_PS2_COMMON_INCLUDE_DIRS}
  "${GSKIT}/include"
)

# Bootstrap is a correctness target. Current PS2SDK injects -O2 globally,
# while the Perfect Dark port currently carries known -O2 issues.
# The target-local -Og deliberately wins here until correctness is established.
target_compile_options(pd_ps2_bootstrap PRIVATE
  ${PD_PS2_CORRECTNESS_FLAGS}
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

target_compile_definitions(pd_ps2_bootstrap PRIVATE
  PLATFORM_PS2=1
  PD_PS2_GIT_COMMIT="${PD_PS2_GIT_COMMIT}"
)
target_link_directories(pd_ps2_bootstrap PRIVATE "${GSKIT}/lib")

# Current ps2sdk-ports installs EE zlib into PS2SDK/ports. Keep it behind the
# bootstrap only until the full build has a platform dependency layer.
# libpadx matches the ROM XPADMAN module used by the current PS2SDK padx sample.
target_link_libraries(pd_ps2_bootstrap PRIVATE gskit dmakit padx z kernel)

if(PD_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
  set(PD_PS2_BOOTSTRAP_OUTPUT_NAME "pd-ps2-alpha-trilerp-diag")
else()
  set(PD_PS2_BOOTSTRAP_OUTPUT_NAME "pd-ps2-bootstrap")
endif()

set_target_properties(pd_ps2_bootstrap PROPERTIES
  OUTPUT_NAME "${PD_PS2_BOOTSTRAP_OUTPUT_NAME}"
  SUFFIX ".elf"
)

# Compile real game/render producers next to the renderer bootstrap.
#
# This stays an OBJECT target for now: the migration frontier exposes EE
# compiler/platform-contract breakage without hiding it behind unresolved
# audio/filesystem/gameplay symbols. The selected sources span the path from
# player/world/menu render producers through gfxmemory/GBI, VI/RDP scheduling and
# finally videoSubmitCommands -> shared Fast3D -> the PS2 GS backend. The shared
# options menu is also compiled here so console video/input APIs cannot silently
# drift away from the game-facing settings contract.
add_library(pd_ps2_game_bridge OBJECT
  "${PD_ROOT}/port/src/main.c"
  "${PD_ROOT}/port/src/config.c"
  "${PD_ROOT}/port/src/pdmain.c"
  "${PD_ROOT}/port/src/pdsched.c"
  "${PD_ROOT}/port/src/optionsmenu.c"
  "${PD_ROOT}/src/lib/vi.c"
  "${PD_ROOT}/src/lib/rdp.c"
  "${PD_ROOT}/src/lib/model.c"
  "${PD_ROOT}/src/game/gfxmemory.c"
  "${PD_ROOT}/src/game/tex.c"
  "${PD_ROOT}/src/game/bg.c"
  "${PD_ROOT}/src/game/player.c"
  "${PD_ROOT}/src/game/menugfx.c"
  video_ps2.c
  input_ps2.c
  input_ps2_map.c
  gfx_frame_ps2.c
)
add_dependencies(pd_ps2_game_bridge pd_ps2_asset_headers)

target_include_directories(pd_ps2_game_bridge PRIVATE
  ${PD_PS2_COMMON_INCLUDE_DIRS}
  "${PD_PS2_GENERATED_DIR}"
  "${GSKIT}/include"
)

target_compile_options(pd_ps2_game_bridge PRIVATE
  ${PD_PS2_CORRECTNESS_FLAGS}
)

target_compile_definitions(pd_ps2_game_bridge PRIVATE
  ${PD_PS2_GAME_DEFINITIONS}
  PD_PS2_GIT_COMMIT="${PD_PS2_GIT_COMMIT}"
)

# Broad compile frontier: prove that the complete game layer accepts the EE ABI
# before spending time on link/runtime services. This target contains only the
# decompiled game sources, so failures here are genuine source/toolchain issues,
# not missing audio/filesystem/input implementations. It is deliberately
# compile-only and has no runtime meaning yet.
file(GLOB_RECURSE PD_PS2_ALL_GAME_SOURCES CONFIGURE_DEPENDS
  "${PD_ROOT}/src/game/*.c"
)
add_library(pd_ps2_game_compile_all OBJECT ${PD_PS2_ALL_GAME_SOURCES})
add_dependencies(pd_ps2_game_compile_all pd_ps2_asset_headers)
target_include_directories(pd_ps2_game_compile_all PRIVATE
  ${PD_PS2_COMMON_INCLUDE_DIRS}
  "${PD_PS2_GENERATED_DIR}"
  "${GSKIT}/include"
)
target_compile_options(pd_ps2_game_compile_all PRIVATE
  ${PD_PS2_CORRECTNESS_FLAGS}
)
target_compile_definitions(pd_ps2_game_compile_all PRIVATE
  ${PD_PS2_GAME_DEFINITIONS}
  PD_PS2_GIT_COMMIT="${PD_PS2_GIT_COMMIT}"
)

# Mirror the current portable library set from the desktop build. This is a
# compile-only EE ABI/toolchain gate, not an assertion that every subsystem
# should keep its current implementation at runtime. In particular audio and
# storage will still get explicit PS2 service contracts before full linking.
file(GLOB PD_PS2_LIB_MP3 CONFIGURE_DEPENDS "${PD_ROOT}/src/lib/mp3/*.c")
file(GLOB PD_PS2_LIB_NAUDIO CONFIGURE_DEPENDS "${PD_ROOT}/src/lib/naudio/*.c")
file(GLOB PD_PS2_LIB_ULTRA_AUDIO CONFIGURE_DEPENDS "${PD_ROOT}/src/lib/ultra/audio/*.c")
file(GLOB PD_PS2_LIB_GU CONFIGURE_DEPENDS "${PD_ROOT}/src/lib/ultra/gu/*.c")
set(PD_PS2_PORTABLE_LIB_SOURCES
  "${PD_ROOT}/src/lib/ultra/io/vimodentsclan1.c"
  "${PD_ROOT}/src/lib/ultra/io/vimodepallan1.c"
  "${PD_ROOT}/src/lib/ultra/io/vimodempallan1.c"
  "${PD_ROOT}/src/lib/ultra/io/vitbl.c"
  "${PD_ROOT}/src/lib/ailist.c"
  "${PD_ROOT}/src/lib/anim.c"
  "${PD_ROOT}/src/lib/args.c"
  "${PD_ROOT}/src/lib/audiodma.c"
  "${PD_ROOT}/src/lib/audiomgr.c"
  "${PD_ROOT}/src/lib/base.c"
  "${PD_ROOT}/src/lib/collision.c"
  "${PD_ROOT}/src/lib/debughud.c"
  "${PD_ROOT}/src/lib/dma.c"
  "${PD_ROOT}/src/lib/fault.c"
  "${PD_ROOT}/src/lib/joy.c"
  "${PD_ROOT}/src/lib/lib_2f490_c.c"
  "${PD_ROOT}/src/lib/lib_2fc60.c"
  "${PD_ROOT}/src/lib/lib_3e3e0.c"
  "${PD_ROOT}/src/lib/lib_04f60nb.c"
  "${PD_ROOT}/src/lib/lib_17ce0.c"
  "${PD_ROOT}/src/lib/lib_034d0.c"
  "${PD_ROOT}/src/lib/lib_39c80.c"
  "${PD_ROOT}/src/lib/lib_15850.c"
  "${PD_ROOT}/src/lib/mema.c"
  "${PD_ROOT}/src/lib/memp.c"
  "${PD_ROOT}/src/lib/model.c"
  "${PD_ROOT}/src/lib/modelasm_c.c"
  "${PD_ROOT}/src/lib/mp3.c"
  "${PD_ROOT}/src/lib/mtx_c.c"
  "${PD_ROOT}/src/lib/mtx.c"
  "${PD_ROOT}/src/lib/music.c"
  "${PD_ROOT}/src/lib/path.c"
  "${PD_ROOT}/src/lib/profile.c"
  "${PD_ROOT}/src/lib/rdp.c"
  "${PD_ROOT}/src/lib/rmon.c"
  "${PD_ROOT}/src/lib/rng_c.c"
  "${PD_ROOT}/src/lib/rzip_c.c"
  "${PD_ROOT}/src/lib/snd.c"
  "${PD_ROOT}/src/lib/speaker.c"
  "${PD_ROOT}/src/lib/varsinit.c"
  "${PD_ROOT}/src/lib/vi.c"
  "${PD_ROOT}/src/textureconfig.c"
  ${PD_PS2_LIB_MP3}
  ${PD_PS2_LIB_NAUDIO}
  ${PD_PS2_LIB_ULTRA_AUDIO}
  ${PD_PS2_LIB_GU}
)
add_library(pd_ps2_core_compile_all OBJECT ${PD_PS2_PORTABLE_LIB_SOURCES})
add_dependencies(pd_ps2_core_compile_all pd_ps2_asset_headers)
target_include_directories(pd_ps2_core_compile_all PRIVATE
  ${PD_PS2_COMMON_INCLUDE_DIRS}
  "${PD_PS2_GENERATED_DIR}"
  "${GSKIT}/include"
)
target_compile_options(pd_ps2_core_compile_all PRIVATE
  ${PD_PS2_CORRECTNESS_FLAGS}
)
target_compile_definitions(pd_ps2_core_compile_all PRIVATE
  ${PD_PS2_GAME_DEFINITIONS}
  PD_PS2_GIT_COMMIT="${PD_PS2_GIT_COMMIT}"
)

# The real Fast3D translator remains linked as the compatibility frontend.
# gfx_api_scene.c is the current lower-level correctness baseline while GS
# ownership is moved below gfx_ps2 into gs_core. DualShock 2 input is a separate
# physical backend plus semantic shooter mapping. gs_native_queue is the first
# project-owned PATH3 transport layer and is compiled before taking over the
# active renderer so packet correctness can be gated independently.
#
# pd_ps2_game_bridge is the migration seam toward the actual Perfect Dark main
# loop. Once both broad compile gates are green, stop growing compile-only
# coverage and move to a real link probe with explicit PS2 platform services.

message(STATUS "Perfect Dark PS2 bootstrap root: ${PD_ROOT}")
message(STATUS "Perfect Dark PS2 bootstrap gsKit: ${GSKIT}")
message(STATUS "Perfect Dark PS2 bootstrap commit: ${PD_PS2_GIT_COMMIT}")
