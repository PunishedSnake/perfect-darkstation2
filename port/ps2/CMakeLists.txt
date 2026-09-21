cmake_minimum_required(VERSION 3.16)

# Keep the bootstrap self-contained. The official PS2DEV image installs the
# toolchain and PS2SDK, but not PS2SDK's samples/ directory.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/ps2dev-toolchain.cmake"
      CACHE FILEPATH "PS2SDK-compatible EE CMake toolchain" FORCE)
endif()

project(pd_ps2_bootstrap C CXX)

option(PD_PS2_ALPHA_TRILERP_DIAGNOSTIC
  "Start the PS2 bootstrap on the hardware-validated alpha-trilerp A/B scene"
  OFF
)
option(PD_PS2_VU1_COLOR_DIAGNOSTIC
  "Enable VU1 textured transforms, A+D transport and runtime hardware A/B"
  OFF
)
option(PD_PS2_GAME_VU1
  "Use the VU1/PATH1 transform path in the full game"
  ON
)
option(PD_PS2_NATIVE_INDEXED_TEXTURES
  "Keep exact CI/IA/I textures in native GS PSMT4/PSMT8 residency"
  ON
)
option(PD_PS2_GEOMETRY_BASELINE_DIAGNOSTIC
  "Render game triangles as untextured diagnostic colors"
  OFF
)
option(PD_PS2_OPAQUE_GEOMETRY_DIAGNOSTIC
  "Render only opaque game triangles as untextured diagnostic colors"
  OFF
)
option(PD_PS2_OPAQUE_MATERIAL_DIAGNOSTIC
  "Render only opaque game triangles with direct TEXEL0 materials"
  OFF
)
option(PD_PS2_ALPHA_MATERIAL_DIAGNOSTIC
  "Render direct TEXEL0 materials while preserving alpha visibility state"
  OFF
)
option(PD_PS2_ALPHA_SPARSE_SHUFFLE
  "Omit definitely untouched 8x2 channel-shuffle sprites in alpha pass graphs"
  ON
)
option(PD_PS2_ALPHA_SAME_SAMPLE_FASTPATH
  "Collapse alpha trilerp when both cycles provably sample the same texels"
  ON
)
option(PD_PS2_INDEPENDENT_ALPHA_MASK
  "Use indexed alpha-mask CLUTs for exact one-pass independent-alpha materials"
  ON
)
option(PD_PS2_INDEPENDENT_ALPHA_DIRECT
  "Use destination alpha to avoid tiled INPUT1 RGB / TEXEL0 alpha composites"
  OFF
)
option(PD_PS2_DISABLE_CULLING_DIAGNOSTIC
  "Disable Fast3D triangle culling in the PS2 generated frontend"
  OFF
)
option(PD_PS2_DISABLE_DEPTH_DIAGNOSTIC
  "Disable GS depth testing and writes in the full game"
  OFF
)
option(PD_PS2_FMCB_STARTUP_DIAGNOSTIC
  "Show direct GS BGCOLOR markers through full-game startup"
  OFF
)
option(PD_PS2_CLEAN_IOP_STARTUP_DIAGNOSTIC
  "Discard inherited IOP state before PS2 runtime service bootstrap"
  OFF
)
option(PD_PS2_CURRENT_PAD_IRX_DIAGNOSTIC
  "Use embedded current-PS2SDK sio2man/padman instead of ROM X modules"
  OFF
)
option(PD_PS2_FORCE_LEGACY_USB_ALIAS_DIAGNOSTIC
  "Rewrite argv[0]-derived usb:/usb0:/mass0: paths to legacy mass:"
  OFF
)
if(PD_PS2_CLEAN_IOP_STARTUP_DIAGNOSTIC AND NOT PD_PS2_FMCB_STARTUP_DIAGNOSTIC)
  message(FATAL_ERROR
    "PD_PS2_CLEAN_IOP_STARTUP_DIAGNOSTIC requires PD_PS2_FMCB_STARTUP_DIAGNOSTIC")
endif()
if(PD_PS2_CURRENT_PAD_IRX_DIAGNOSTIC AND NOT PD_PS2_CLEAN_IOP_STARTUP_DIAGNOSTIC)
  message(FATAL_ERROR
    "PD_PS2_CURRENT_PAD_IRX_DIAGNOSTIC requires clean-IOP startup diagnostic")
endif()
if(PD_PS2_FORCE_LEGACY_USB_ALIAS_DIAGNOSTIC AND NOT PD_PS2_CURRENT_PAD_IRX_DIAGNOSTIC)
  message(FATAL_ERROR
    "PD_PS2_FORCE_LEGACY_USB_ALIAS_DIAGNOSTIC requires current-PAD diagnostic")
endif()
if(PD_PS2_ALPHA_TRILERP_DIAGNOSTIC AND PD_PS2_VU1_COLOR_DIAGNOSTIC)
  message(FATAL_ERROR "Select one hardware diagnostic per build directory")
endif()

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
find_program(PD_PS2_BIN2C NAMES bin2c
  HINTS "$ENV{PS2SDK}/bin"
  REQUIRED
)

find_program(PD_PS2_DVP_AS NAMES dvp-as
  HINTS "$ENV{PS2DEV}/bin" "$ENV{PS2SDK}/bin"
  REQUIRED
)
set(PD_PS2_VU1_PROGRAM_OBJECT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/gs_vu1_color_path.o"
)
set(PD_PS2_VU1_TRANSFORM_PROGRAM_OBJECT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/gs_vu1_textured_transform.o"
)
add_custom_command(
  OUTPUT "${PD_PS2_VU1_PROGRAM_OBJECT}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${PD_PS2_DVP_AS}"
    "${CMAKE_CURRENT_LIST_DIR}/gs_vu1_color_path.vsm"
    -o "${PD_PS2_VU1_PROGRAM_OBJECT}"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gs_vu1_color_path.vsm"
  COMMENT "Assembling VU1 PATH1 A+D transport microprogram"
  VERBATIM
)
add_custom_command(
  OUTPUT "${PD_PS2_VU1_TRANSFORM_PROGRAM_OBJECT}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${PD_PS2_DVP_AS}"
    "${CMAKE_CURRENT_LIST_DIR}/gs_vu1_textured_transform.vsm"
    -o "${PD_PS2_VU1_TRANSFORM_PROGRAM_OBJECT}"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gs_vu1_textured_transform.vsm"
  COMMENT "Assembling VU1 textured transform microprogram"
  VERBATIM
)
set_source_files_properties(
  "${PD_PS2_VU1_PROGRAM_OBJECT}"
  "${PD_PS2_VU1_TRANSFORM_PROGRAM_OBJECT}"
  PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE
)

set(PD_PS2_BOOTSTRAP_VU1_OBJECTS "")
if(PD_PS2_VU1_COLOR_DIAGNOSTIC)
  list(APPEND PD_PS2_BOOTSTRAP_VU1_OBJECTS
    "${PD_PS2_VU1_PROGRAM_OBJECT}"
    "${PD_PS2_VU1_TRANSFORM_PROGRAM_OBJECT}"
  )
endif()

set(PD_PS2_AUDSRV_IRX "$ENV{PS2SDK}/iop/irx/audsrv.irx")
if(NOT EXISTS "${PD_PS2_AUDSRV_IRX}")
  message(FATAL_ERROR
    "Current PS2SDK audsrv.irx not found at ${PD_PS2_AUDSRV_IRX}")
endif()

set(PD_PS2_AUDSRV_IRX_C
  "${CMAKE_CURRENT_BINARY_DIR}/generated/audsrv_irx.c"
)

set(PD_PS2_USBD_IRX "$ENV{PS2SDK}/iop/irx/usbd.irx")
set(PD_PS2_USBHDFSD_IRX "$ENV{PS2SDK}/iop/irx/usbhdfsd.irx")
set(PD_PS2_SIO2MAN_IRX "$ENV{PS2SDK}/iop/irx/sio2man.irx")
set(PD_PS2_PADMAN_IRX "$ENV{PS2SDK}/iop/irx/padman.irx")
foreach(PD_PS2_STORAGE_IRX IN ITEMS
    "${PD_PS2_USBD_IRX}" "${PD_PS2_USBHDFSD_IRX}"
    "${PD_PS2_SIO2MAN_IRX}" "${PD_PS2_PADMAN_IRX}")
  if(NOT EXISTS "${PD_PS2_STORAGE_IRX}")
    message(FATAL_ERROR
      "Current PS2SDK storage IRX not found: ${PD_PS2_STORAGE_IRX}")
  endif()
endforeach()

set(PD_PS2_USBD_IRX_C
  "${CMAKE_CURRENT_BINARY_DIR}/generated/usbd_irx.c"
)
set(PD_PS2_USBHDFSD_IRX_C
  "${CMAKE_CURRENT_BINARY_DIR}/generated/usbhdfsd_irx.c"
)
set(PD_PS2_SIO2MAN_IRX_C
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sio2man_irx.c"
)
set(PD_PS2_PADMAN_IRX_C
  "${CMAKE_CURRENT_BINARY_DIR}/generated/padman_irx.c"
)
add_custom_command(
  OUTPUT "${PD_PS2_AUDSRV_IRX_C}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${PD_PS2_BIN2C}"
    "${PD_PS2_AUDSRV_IRX}" "${PD_PS2_AUDSRV_IRX_C}" audsrv_irx
  DEPENDS "${PD_PS2_AUDSRV_IRX}"
  COMMENT "Embedding PS2SDK audsrv.irx for file-independent SPU2 startup"
  VERBATIM
)
set_source_files_properties(
  "${PD_PS2_AUDSRV_IRX_C}"
  PROPERTIES GENERATED TRUE
)

add_custom_command(
  OUTPUT "${PD_PS2_USBD_IRX_C}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${PD_PS2_BIN2C}"
    "${PD_PS2_USBD_IRX}" "${PD_PS2_USBD_IRX_C}" usbd_irx
  DEPENDS "${PD_PS2_USBD_IRX}"
  COMMENT "Embedding PS2SDK usbd.irx for launcher-independent mass storage"
  VERBATIM
)
add_custom_command(
  OUTPUT "${PD_PS2_USBHDFSD_IRX_C}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${PD_PS2_BIN2C}"
    "${PD_PS2_USBHDFSD_IRX}" "${PD_PS2_USBHDFSD_IRX_C}" usbhdfsd_irx
  DEPENDS "${PD_PS2_USBHDFSD_IRX}"
  COMMENT "Embedding PS2SDK usbhdfsd.irx for launcher-independent mass storage"
  VERBATIM
)
add_custom_command(
  OUTPUT "${PD_PS2_SIO2MAN_IRX_C}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${PD_PS2_BIN2C}"
    "${PD_PS2_SIO2MAN_IRX}" "${PD_PS2_SIO2MAN_IRX_C}" sio2man_irx
  DEPENDS "${PD_PS2_SIO2MAN_IRX}"
  COMMENT "Embedding current PS2SDK sio2man.irx for PAD A/B"
  VERBATIM
)
add_custom_command(
  OUTPUT "${PD_PS2_PADMAN_IRX_C}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${PD_PS2_BIN2C}"
    "${PD_PS2_PADMAN_IRX}" "${PD_PS2_PADMAN_IRX_C}" padman_irx
  DEPENDS "${PD_PS2_PADMAN_IRX}"
  COMMENT "Embedding current PS2SDK padman.irx for PAD A/B"
  VERBATIM
)
set_source_files_properties(
  "${PD_PS2_USBD_IRX_C}"
  "${PD_PS2_USBHDFSD_IRX_C}"
  "${PD_PS2_SIO2MAN_IRX_C}"
  "${PD_PS2_PADMAN_IRX_C}"
  PROPERTIES GENERATED TRUE
)

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

set(PD_PS2_OPTIMIZATION "Og" CACHE STRING
  "EE optimization profile for PS2 targets (Og or O2)"
)
set_property(CACHE PD_PS2_OPTIMIZATION PROPERTY STRINGS Og O2)
if(PD_PS2_OPTIMIZATION STREQUAL "Og")
  set(PD_PS2_OPTIMIZATION_FLAG -Og)
elseif(PD_PS2_OPTIMIZATION STREQUAL "O2")
  set(PD_PS2_OPTIMIZATION_FLAG -O2)
else()
  message(FATAL_ERROR
    "PD_PS2_OPTIMIZATION must be Og or O2, got: ${PD_PS2_OPTIMIZATION}")
endif()

option(PD_PS2_FILE_LOG_DEFAULT
  "Enable the blocking mass:/host: file logger by default in the full game" OFF
)
if(PD_PS2_FILE_LOG_DEFAULT)
  set(PD_PS2_FILE_LOG_DEFAULT_VALUE 1)
else()
  set(PD_PS2_FILE_LOG_DEFAULT_VALUE 0)
endif()

set(PD_PS2_CORRECTNESS_FLAGS
  ${PD_PS2_OPTIMIZATION_FLAG}
  -G0
  -ffunction-sections
  -fdata-sections
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
  VERSION_ROMID="${PD_PS2_ROMID}"
  PD_PS2_OPTIMIZATION_PROFILE="${PD_PS2_OPTIMIZATION}"
  PD_PS2_FILE_LOG_DEFAULT=${PD_PS2_FILE_LOG_DEFAULT_VALUE}
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
  audio_ps2.c
  audio_ps2_queue.c
  "${PD_PS2_AUDSRV_IRX_C}"
  video_diag.c
  gs_alpha_equation.cpp
  gs_frame_mask.cpp
  gs_command_budget.cpp
  gs_clip.cpp
  gs_core.cpp
  gs_native_queue.cpp
  gs_render_target_layout.cpp
  gs_state_shadow.cpp
  gs_texture_convert.cpp
  gs_vu1_batch.cpp
  gs_vu1_queue.cpp
  gs_vu1_transform.cpp
  gs_vu1_wait.cpp
  gs_vram_allocator.cpp
  ps2_renderer_stats.cpp
  renderer_trace.cpp
  gfx_api_scene.c
  gfx_frame_ps2.c
  gfx_run_tmem_ps2.cpp
  gfx_ps2.cpp
  gfx_ps2_combiner.cpp
  gfx_ps2_pass_graph.cpp
  gfx_window_ps2.c
  pad_axis_ps2.c
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
  ${PD_PS2_BOOTSTRAP_VU1_OBJECTS}
)

if(PD_PS2_ALPHA_TRILERP_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_bootstrap PRIVATE
    PERFECT_DARK_PS2_ALPHA_TRILERP_DIAGNOSTIC=1
  )
endif()
if(PD_PS2_ALPHA_SPARSE_SHUFFLE)
  target_compile_definitions(pd_ps2_bootstrap PRIVATE
    PERFECT_DARK_PS2_ALPHA_SPARSE_SHUFFLE=1
  )
endif()
if(PD_PS2_ALPHA_SAME_SAMPLE_FASTPATH)
  target_compile_definitions(pd_ps2_bootstrap PRIVATE
    PERFECT_DARK_PS2_ALPHA_SAME_SAMPLE_FASTPATH=1
  )
endif()
if(PD_PS2_INDEPENDENT_ALPHA_MASK)
  target_compile_definitions(pd_ps2_bootstrap PRIVATE
    PERFECT_DARK_PS2_INDEPENDENT_ALPHA_MASK=1
  )
endif()
if(PD_PS2_INDEPENDENT_ALPHA_DIRECT)
  target_compile_definitions(pd_ps2_bootstrap PRIVATE
    PERFECT_DARK_PS2_INDEPENDENT_ALPHA_DIRECT=1
  )
endif()

if(PD_PS2_VU1_COLOR_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_bootstrap PRIVATE
    PERFECT_DARK_PS2_VU1_COLOR_BATCH=1
    PERFECT_DARK_PS2_VU1_AD_TRANSPORT=1
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
target_link_libraries(pd_ps2_bootstrap PRIVATE
  gskit dmakit padx z audsrv -ldebug patches kernel)

# Standalone hardware probe for the EE -> SIF/RPC -> audsrv -> SPU2 path.
# It deliberately has no renderer or game-mixer dependency, so an audible
# 440 Hz tone proves that silence in the full game originates above audsrv.
add_executable(pd_ps2_audio_diag EXCLUDE_FROM_ALL
  audio_diag.c
  audio_ps2.c
  audio_ps2_queue.c
  system_ps2.c
  "${PD_PS2_AUDSRV_IRX_C}"
)
target_include_directories(pd_ps2_audio_diag PRIVATE
  ${PD_PS2_COMMON_INCLUDE_DIRS}
  "${PD_PS2_GENERATED_DIR}"
)
target_compile_options(pd_ps2_audio_diag PRIVATE
  ${PD_PS2_CORRECTNESS_FLAGS}
)
target_compile_definitions(pd_ps2_audio_diag PRIVATE
  PLATFORM_PS2=1
  PD_PS2_GIT_COMMIT="${PD_PS2_GIT_COMMIT}"
)
target_link_libraries(pd_ps2_audio_diag PRIVATE
  audsrv patches kernel)
set_target_properties(pd_ps2_audio_diag PROPERTIES
  OUTPUT_NAME "pd-ps2-audio-diag"
  SUFFIX ".elf"
)

# FMCB/ExecPS2 loader probes. These deliberately avoid the game runtime so a
# black-screen launch can be separated into loader, CRT and load-image-size
# failures on real hardware.
set(PD_PS2_LOADER_PROBE_RAW_OBJ
  "${CMAKE_CURRENT_BINARY_DIR}/loader-probe-raw.o")
set(PD_PS2_LOADER_PROBE_RAW_ELF
  "${CMAKE_CURRENT_BINARY_DIR}/pd-ps2-loader-probe-raw.elf")
set(PD_PS2_LOADER_PROBE_LARGE_OBJ
  "${CMAKE_CURRENT_BINARY_DIR}/loader-probe-large.o")
set(PD_PS2_LOADER_PROBE_LARGE_ELF
  "${CMAKE_CURRENT_BINARY_DIR}/pd-ps2-loader-probe-large.elf")

add_custom_command(
  OUTPUT "${PD_PS2_LOADER_PROBE_RAW_OBJ}"
  COMMAND "${CMAKE_C_COMPILER}" -c -x assembler-with-cpp -G0
    "${CMAKE_CURRENT_LIST_DIR}/loader_probe_raw.S"
    -o "${PD_PS2_LOADER_PROBE_RAW_OBJ}"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/loader_probe_raw.S"
  VERBATIM
)
add_custom_command(
  OUTPUT "${PD_PS2_LOADER_PROBE_RAW_ELF}"
  COMMAND "${CMAKE_C_COMPILER}"
    -nostdlib -nostartfiles -nodefaultlibs -G0
    -Wl,-zmax-page-size=128
    "-Wl,-T,${CMAKE_CURRENT_LIST_DIR}/loader_probe.ld"
    -Wl,-e,_start
    "${PD_PS2_LOADER_PROBE_RAW_OBJ}"
    -o "${PD_PS2_LOADER_PROBE_RAW_ELF}"
  DEPENDS "${PD_PS2_LOADER_PROBE_RAW_OBJ}"
    "${CMAKE_CURRENT_LIST_DIR}/loader_probe.ld"
  VERBATIM
)

add_custom_command(
  OUTPUT "${PD_PS2_LOADER_PROBE_LARGE_OBJ}"
  COMMAND "${CMAKE_C_COMPILER}" -c -x assembler-with-cpp -G0
    "${CMAKE_CURRENT_LIST_DIR}/loader_probe_large.S"
    -o "${PD_PS2_LOADER_PROBE_LARGE_OBJ}"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/loader_probe_large.S"
  VERBATIM
)
add_custom_command(
  OUTPUT "${PD_PS2_LOADER_PROBE_LARGE_ELF}"
  COMMAND "${CMAKE_C_COMPILER}"
    -nostdlib -nostartfiles -nodefaultlibs -G0
    -Wl,-zmax-page-size=128
    "-Wl,-T,${CMAKE_CURRENT_LIST_DIR}/loader_probe.ld"
    -Wl,-e,_start
    "${PD_PS2_LOADER_PROBE_LARGE_OBJ}"
    -o "${PD_PS2_LOADER_PROBE_LARGE_ELF}"
  DEPENDS "${PD_PS2_LOADER_PROBE_LARGE_OBJ}"
    "${CMAKE_CURRENT_LIST_DIR}/loader_probe.ld"
  VERBATIM
)

add_executable(pd_ps2_loader_probe_crt EXCLUDE_FROM_ALL
  loader_probe_crt.c
)
target_compile_options(pd_ps2_loader_probe_crt PRIVATE -Og -G0)
set_target_properties(pd_ps2_loader_probe_crt PROPERTIES
  OUTPUT_NAME "pd-ps2-loader-probe-crt"
  SUFFIX ".elf"
)

add_custom_target(pd_ps2_loader_probes
  DEPENDS
    "${PD_PS2_LOADER_PROBE_RAW_ELF}"
    "${PD_PS2_LOADER_PROBE_LARGE_ELF}"
    pd_ps2_loader_probe_crt
)

if(PD_PS2_ALPHA_TRILERP_DIAGNOSTIC)
  set(PD_PS2_BOOTSTRAP_OUTPUT_NAME "pd-ps2-alpha-trilerp-diag")
elseif(PD_PS2_VU1_COLOR_DIAGNOSTIC)
  set(PD_PS2_BOOTSTRAP_OUTPUT_NAME "pd-ps2-vu1-color-diag")
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
  "${PD_ROOT}/port/src/fs.c"
  "${PD_ROOT}/port/src/libultra.c"
  "${PD_ROOT}/port/src/pdmain.c"
  "${PD_ROOT}/port/src/pdsched.c"
  "${PD_ROOT}/port/src/romdata.c"
  "${PD_ROOT}/port/src/romsource.c"
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
  audio_ps2.c
  audio_ps2_queue.c
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

# Real game runtime link frontier.
#
# The broad OBJECT targets above prove that all decompiled game and portable
# libultra sources accept the EE ABI. This executable closes the next boundary:
# it links those objects to PS2-owned system, storage, input, audio and video
# services plus the same Fast3D -> VU1/GIF/GS renderer validated by the hardware
# diagnostic. It is excluded from the default target so the two diagnostic
# configurations do not rebuild the full game; CI requests it explicitly from
# the normal build directory.
file(GLOB PD_PS2_PREPROCESS_SOURCES CONFIGURE_DEPENDS
  "${PD_ROOT}/port/src/preprocess/*.c"
)

set(PD_PS2_GAME_RUNTIME_SOURCES
  "${PD_ROOT}/port/src/config.c"
  "${PD_ROOT}/port/src/fs.c"
  "${PD_ROOT}/port/src/libultra.c"
  "${PD_ROOT}/port/src/main.c"
  "${PD_ROOT}/port/src/mixer.c"
  "${PD_ROOT}/port/src/mod.c"
  "${PD_ROOT}/port/src/mpsetups.c"
  "${PD_ROOT}/port/src/optionsmenu.c"
  "${PD_ROOT}/port/src/pdmain.c"
  "${PD_ROOT}/port/src/pdsched.c"
  "${PD_ROOT}/port/src/romdata.c"
  "${PD_ROOT}/port/src/romsource.c"
  "${PD_ROOT}/port/src/utils.c"
  ${PD_PS2_PREPROCESS_SOURCES}
  system_ps2.c
  storage_ps2.c
  fast_math_ps2.c
  audio_ps2.c
  audio_ps2_queue.c
  "${PD_PS2_AUDSRV_IRX_C}"
  "${PD_PS2_USBD_IRX_C}"
  "${PD_PS2_USBHDFSD_IRX_C}"
  "${PD_PS2_SIO2MAN_IRX_C}"
  "${PD_PS2_PADMAN_IRX_C}"
  video_ps2.c
  input_ps2.c
  input_ps2_map.c
  pad_axis_ps2.c
  pad_ps2.c
)

set(PD_PS2_GAME_RENDERER_SOURCES
  gs_alpha_equation.cpp
  gs_frame_mask.cpp
  gs_command_budget.cpp
  gs_clip.cpp
  gs_core.cpp
  gs_native_queue.cpp
  gs_render_target_layout.cpp
  gs_state_shadow.cpp
  gs_texture_convert.cpp
  gs_vu1_batch.cpp
  gs_vu1_queue.cpp
  gs_vu1_transform.cpp
  gs_vu1_wait.cpp
  gs_vram_allocator.cpp
  ps2_renderer_stats.cpp
  renderer_trace.cpp
  gfx_frame_ps2.c
  gfx_run_tmem_ps2.cpp
  gfx_ps2.cpp
  gfx_ps2_combiner.cpp
  gfx_ps2_pass_graph.cpp
  gfx_window_ps2.c
  "${PD_ROOT}/port/fast3d/gfx_cc.cpp"
  "${PD_PS2_FAST3D_LIVE_CPP}"
  "${PD_ROOT}/port/fast3d/rdp_tmem.cpp"
  "${PD_ROOT}/port/fast3d/rdp_tmem_runtime.cpp"
  "${PD_ROOT}/port/fast3d/rdp_tmem_live.cpp"
  "${PD_ROOT}/port/fast3d/rdp_tmem_trace.cpp"
  "${PD_PS2_VU1_PROGRAM_OBJECT}"
  "${PD_PS2_VU1_TRANSFORM_PROGRAM_OBJECT}"
)

add_executable(pd_ps2_game EXCLUDE_FROM_ALL
  ${PD_PS2_GAME_RUNTIME_SOURCES}
  ${PD_PS2_GAME_RENDERER_SOURCES}
  $<TARGET_OBJECTS:pd_ps2_game_compile_all>
  $<TARGET_OBJECTS:pd_ps2_core_compile_all>
)
add_dependencies(pd_ps2_game pd_ps2_asset_headers)

target_include_directories(pd_ps2_game PRIVATE
  ${PD_PS2_COMMON_INCLUDE_DIRS}
  "${PD_PS2_GENERATED_DIR}"
  "${GSKIT}/include"
)
target_compile_options(pd_ps2_game PRIVATE
  ${PD_PS2_CORRECTNESS_FLAGS}
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)
target_compile_definitions(pd_ps2_game PRIVATE
  ${PD_PS2_GAME_DEFINITIONS}
  PD_PS2_GIT_COMMIT="${PD_PS2_GIT_COMMIT}"
)
if(PD_PS2_GAME_VU1)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_VU1_COLOR_BATCH=1
  )
endif()
if(PD_PS2_NATIVE_INDEXED_TEXTURES)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_NATIVE_INDEXED_TEXTURES=1
  )
endif()
if(PD_PS2_ALPHA_SPARSE_SHUFFLE)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_ALPHA_SPARSE_SHUFFLE=1
  )
endif()
if(PD_PS2_ALPHA_SAME_SAMPLE_FASTPATH)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_ALPHA_SAME_SAMPLE_FASTPATH=1
  )
endif()
if(PD_PS2_INDEPENDENT_ALPHA_MASK)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_INDEPENDENT_ALPHA_MASK=1
  )
endif()
if(PD_PS2_INDEPENDENT_ALPHA_DIRECT)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_INDEPENDENT_ALPHA_DIRECT=1
  )
endif()
if(PD_PS2_GEOMETRY_BASELINE_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_GEOMETRY_BASELINE=1
  )
endif()
if(PD_PS2_OPAQUE_GEOMETRY_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_GEOMETRY_BASELINE=1
    PERFECT_DARK_PS2_OPAQUE_DIAGNOSTIC=1
  )
endif()
if(PD_PS2_OPAQUE_MATERIAL_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_MATERIAL_BASELINE=1
    PERFECT_DARK_PS2_OPAQUE_DIAGNOSTIC=1
  )
endif()
if(PD_PS2_ALPHA_MATERIAL_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_MATERIAL_BASELINE=1
  )
endif()
if(PD_PS2_DISABLE_CULLING_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_DISABLE_CULLING=1
  )
endif()
if(PD_PS2_DISABLE_DEPTH_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_game PRIVATE
    PERFECT_DARK_PS2_DISABLE_DEPTH=1
  )
endif()
if(PD_PS2_FMCB_STARTUP_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_game PRIVATE
    PD_PS2_FMCB_STARTUP_DIAGNOSTIC=1
  )
endif()
if(PD_PS2_CLEAN_IOP_STARTUP_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_game PRIVATE
    PD_PS2_CLEAN_IOP_STARTUP_DIAGNOSTIC=1
  )
endif()
if(PD_PS2_CURRENT_PAD_IRX_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_game PRIVATE
    PD_PS2_CURRENT_PAD_IRX_DIAGNOSTIC=1
  )
endif()
if(PD_PS2_FORCE_LEGACY_USB_ALIAS_DIAGNOSTIC)
  target_compile_definitions(pd_ps2_game PRIVATE
    PD_PS2_FORCE_LEGACY_USB_ALIAS_DIAGNOSTIC=1
  )
endif()
target_link_directories(pd_ps2_game PRIVATE "${GSKIT}/lib")
set(PD_PS2_GAME_LINK_MAP "${CMAKE_CURRENT_BINARY_DIR}/pd-ps2-game.map")
target_link_options(pd_ps2_game PRIVATE
  -Wl,--gc-sections
  "-Wl,-Map,${PD_PS2_GAME_LINK_MAP}"
)
target_link_libraries(pd_ps2_game PRIVATE
  gskit dmakit padx z audsrv -ldebug patches kernel m)
if(PD_PS2_FORCE_LEGACY_USB_ALIAS_DIAGNOSTIC)
  set_target_properties(pd_ps2_game PROPERTIES
    OUTPUT_NAME "pd-ps2-game-fmcb-usb-alias"
    SUFFIX ".elf"
  )
elseif(PD_PS2_CURRENT_PAD_IRX_DIAGNOSTIC)
  set_target_properties(pd_ps2_game PROPERTIES
    OUTPUT_NAME "pd-ps2-game-fmcb-current-pad"
    SUFFIX ".elf"
  )
elseif(PD_PS2_CLEAN_IOP_STARTUP_DIAGNOSTIC)
  set_target_properties(pd_ps2_game PROPERTIES
    OUTPUT_NAME "pd-ps2-game-fmcb-clean-iop"
    SUFFIX ".elf"
  )
elseif(PD_PS2_FMCB_STARTUP_DIAGNOSTIC)
  set_target_properties(pd_ps2_game PROPERTIES
    OUTPUT_NAME "pd-ps2-game-fmcb-startup"
    SUFFIX ".elf"
  )
else()
  set_target_properties(pd_ps2_game PROPERTIES
    OUTPUT_NAME "pd-ps2-game"
    SUFFIX ".elf"
  )
endif()

# Preserve the symbol-rich ELF for post-mortem work and emit a launcher-facing
# copy stripped only of non-loadable debug/symbol baggage.  CI verifies that
# entry point, PT_LOAD layout and PT_LOAD bytes remain identical.
find_program(PD_PS2_STRIP_TOOL
  NAMES mips64r5900el-ps2-elf-strip
  REQUIRED
)
set(PD_PS2_FMCB_ELF
  "${CMAKE_CURRENT_BINARY_DIR}/pd-ps2-game-fmcb.elf"
)
add_custom_command(TARGET pd_ps2_game POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy
    "$<TARGET_FILE:pd_ps2_game>" "${PD_PS2_FMCB_ELF}"
  COMMAND "${PD_PS2_STRIP_TOOL}" --strip-all "${PD_PS2_FMCB_ELF}"
  COMMENT "Creating FMCB compatibility ELF with identical PT_LOAD image"
  VERBATIM
)

# The real Fast3D translator remains linked as the compatibility frontend.
# gfx_api_scene.c is the current lower-level correctness baseline while GS
# ownership is moved below gfx_ps2 into gs_core. DualShock 2 input is a separate
# physical backend plus semantic shooter mapping. gs_native_queue is the first
# project-owned PATH3 transport layer and is compiled before taking over the
# active renderer so packet correctness can be gated independently.
#
# pd_ps2_game_bridge remains a narrow fast compile gate. pd_ps2_game is now the
# authoritative link/runtime frontier and must never resolve missing services by
# silently discarding or weakening their contracts.

message(STATUS "Perfect Dark PS2 bootstrap root: ${PD_ROOT}")
message(STATUS "Perfect Dark PS2 bootstrap gsKit: ${GSKIT}")
message(STATUS "Perfect Dark PS2 bootstrap commit: ${PD_PS2_GIT_COMMIT}")
message(STATUS "Perfect Dark PS2 EE optimization: ${PD_PS2_OPTIMIZATION}")
message(STATUS "Perfect Dark PS2 default file logger: ${PD_PS2_FILE_LOG_DEFAULT}")
