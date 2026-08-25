cmake_minimum_required(VERSION 3.16)

# Keep the bootstrap self-contained. The official PS2DEV image installs the
# toolchain and PS2SDK, but not PS2SDK's samples/ directory.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/ps2dev-toolchain.cmake"
      CACHE FILEPATH "PS2SDK-compatible EE CMake toolchain" FORCE)
endif()

project(pd_ps2_bootstrap C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(PD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(pd_ps2_bootstrap
  bootstrap.c
  system_ps2.c
  "${PD_ROOT}/port/src/romsource.c"
)

target_include_directories(pd_ps2_bootstrap PRIVATE
  "${PD_ROOT}/include"
  "${PD_ROOT}/include/PR"
  "${PD_ROOT}/port/include"
)

# Bootstrap is a correctness target. Current PS2SDK injects -O2 globally,
# while the Perfect Dark port currently carries known -O2 issues.
# The target-local -Og deliberately wins here until correctness is established.
target_compile_options(pd_ps2_bootstrap PRIVATE
  -Og
  -G0
  -fno-strict-aliasing
  -funsigned-char
  -fwrapv
)

target_compile_definitions(pd_ps2_bootstrap PRIVATE PLATFORM_PS2=1)
target_link_libraries(pd_ps2_bootstrap PRIVATE kernel)

set_target_properties(pd_ps2_bootstrap PROPERTIES
  OUTPUT_NAME "pd-ps2-bootstrap"
  SUFFIX ".elf"
)

message(STATUS "Perfect Dark PS2 bootstrap root: ${PD_ROOT}")
