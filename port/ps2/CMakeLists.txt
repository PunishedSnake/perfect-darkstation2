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

set(PD_PS2_COMMON_INCLUDE_DIRS
  "${PD_ROOT}/include"
  "${PD_ROOT}/include/PR"
  "${PD_ROOT}/src/include"
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

add_executable(pd_ps2_bootstrap
  bootstrap.c
  system_ps2.c
  video_diag.c
  gfx_api_scene.c
  gfx_ps2.cpp
  rom_probe.c
  "${PD_ROOT}/port/src/romsource.c"
  "${PD_ROOT}/port/fast3d/gfx_cc.cpp"
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
target_link_libraries(pd_ps2_bootstrap PRIVATE gskit dmakit z kernel)

set_target_properties(pd_ps2_bootstrap PROPERTIES
  OUTPUT_NAME "pd-ps2-bootstrap"
  SUFFIX ".elf"
)

# Compile the real current Fast3D translator under the EE compiler even before
# we link the complete game runtime. This is an ABI/source-contract gate: it
# exposes desktop-only assumptions in gfx_pc.cpp without dragging unrelated
# subsystems into the hardware bring-up executable.
add_library(pd_ps2_fast3d_contract OBJECT
  "${PD_ROOT}/port/fast3d/gfx_pc.cpp"
)

target_include_directories(pd_ps2_fast3d_contract PRIVATE
  ${PD_PS2_COMMON_INCLUDE_DIRS}
)

target_compile_options(pd_ps2_fast3d_contract PRIVATE
  ${PD_PS2_CORRECTNESS_FLAGS}
  -fno-exceptions
  -fno-rtti
)

target_compile_definitions(pd_ps2_fast3d_contract PRIVATE
  PLATFORM_PS2=1
)

message(STATUS "Perfect Dark PS2 bootstrap root: ${PD_ROOT}")
message(STATUS "Perfect Dark PS2 bootstrap gsKit: ${GSKIT}")
message(STATUS "Perfect Dark PS2 bootstrap commit: ${PD_PS2_GIT_COMMIT}")
