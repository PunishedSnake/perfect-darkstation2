cmake_minimum_required(VERSION 3.16)

# The current PS2SDK ships its EE CMake toolchain in samples/ps2dev.cmake.
# Set it before project() so CMake never probes the host compiler first.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{PS2SDK} AND EXISTS "$ENV{PS2SDK}/samples/ps2dev.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{PS2SDK}/samples/ps2dev.cmake"
        CACHE FILEPATH "PS2SDK EE CMake toolchain" FORCE)
  else()
    message(FATAL_ERROR
      "PS2SDK toolchain not found. Set PS2SDK or pass "
      "-DCMAKE_TOOLCHAIN_FILE=$PS2SDK/samples/ps2dev.cmake")
  endif()
endif()

project(pd_ps2_bootstrap C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(PD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(pd_ps2_bootstrap
  bootstrap.c
  system_ps2.c
)

target_include_directories(pd_ps2_bootstrap PRIVATE
  "${PD_ROOT}/include"
  "${PD_ROOT}/include/PR"
  "${PD_ROOT}/port/include"
)

# Bootstrap is a correctness target. The current PS2SDK toolchain injects -O2,
# while the Perfect Dark port itself currently carries known -O2 issues.
# Keep the first EE executable conservative; optimization comes after bring-up.
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
