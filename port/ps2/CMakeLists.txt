cmake_minimum_required(VERSION 3.16)

# Keep the bootstrap self-contained. The official PS2DEV image installs the
# toolchain and PS2SDK, but not PS2SDK's samples/ directory.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/ps2dev-toolchain.cmake"
      CACHE FILEPATH "PS2SDK-compatible EE CMake toolchain" FORCE)
endif()

project(pd_ps2_bootstrap C CXX)

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

add_custom_target(pd_ps2_asset_headers DEPENDS ${PD_PS2_GENERATED_HEADERS})

add_executable(pd_ps2_bootstrap
  bootstrap.c
  system_ps2.c
  video_diag.c
  gs_core.cpp
  gs_native_queue.cpp
  gfx_api_scene.c
  gfx_ps2.cpp
  gfx_window_ps2.c
  pad_ps2.c
  controls_ps2.c
  rom_probe.c
  "${PD_ROOT}/port/src/romsource.c"
  "${PD_ROOT}/port/fast3d/gfx_cc.cpp"
  "${PD_ROOT}/port/fast3d/gfx_pc.cpp"
)

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

set_target_properties(pd_ps2_bootstrap PROPERTIES
  OUTPUT_NAME "pd-ps2-bootstrap"
  SUFFIX ".elf"
)

# Compile the first slice of the real game next to the renderer bootstrap.
#
# This is deliberately an OBJECT target: the first milestone is to expose EE
# compiler/platform-contract breakage in the actual game entry/scheduler code
# without hiding it behind hundreds of unresolved desktop audio/input symbols.
# The bootstrap remains an independent, runnable A/B correctness baseline.
add_library(pd_ps2_game_bridge OBJECT
  "${PD_ROOT}/port/src/main.c"
  "${PD_ROOT}/port/src/config.c"
  "${PD_ROOT}/port/src/pdmain.c"
  "${PD_ROOT}/port/src/pdsched.c"
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

# The real Fast3D translator remains linked as the compatibility frontend.
# gfx_api_scene.c is the current lower-level correctness baseline while GS
# ownership is moved below gfx_ps2 into gs_core. DualShock 2 input is a separate
# physical backend plus semantic shooter mapping. gs_native_queue is the first
# project-owned PATH3 transport layer and is compiled before taking over the
# active renderer so packet correctness can be gated independently.
#
# pd_ps2_game_bridge is the migration seam toward the actual Perfect Dark main
# loop. Grow it only as each platform dependency gets an explicit PS2 contract;
# do not paper over missing APIs with generic POSIX assumptions.

message(STATUS "Perfect Dark PS2 bootstrap root: ${PD_ROOT}")
message(STATUS "Perfect Dark PS2 bootstrap gsKit: ${GSKIT}")
message(STATUS "Perfect Dark PS2 bootstrap commit: ${PD_PS2_GIT_COMMIT}")
