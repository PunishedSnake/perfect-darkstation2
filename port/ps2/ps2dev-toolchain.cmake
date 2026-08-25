# Local EE toolchain shim for Perfect DarkStation 2.
#
# CURRENT IMPLEMENTATION snapshot: ps2dev/ps2sdk samples/ps2dev.cmake,
# reviewed 2026-08-25 at blob 926ae54f820027f0f4bc720e87791e44acb253ea.
# Keep this small and re-check it against current PS2SDK when the toolchain changes.

cmake_minimum_required(VERSION 3.0...3.12)

if(DEFINED ENV{PS2SDK})
  set(PS2SDK "$ENV{PS2SDK}")
else()
  message(FATAL_ERROR "The environment variable PS2SDK needs to be defined.")
endif()

if(DEFINED ENV{PS2DEV})
  set(PS2DEV "$ENV{PS2DEV}")
else()
  message(FATAL_ERROR "The environment variable PS2DEV needs to be defined.")
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR mips)

set(CMAKE_C_COMPILER mips64r5900el-ps2-elf-gcc)
set(CMAKE_CXX_COMPILER mips64r5900el-ps2-elf-g++)

set(EE_CFLAGS "-I$ENV{PS2SDK}/ee/include -I$ENV{PS2SDK}/common/include -I$ENV{PS2DEV}/gsKit/include -I$ENV{PS2SDK}/ports/include -D_EE -DPS2 -D__PS2__ -O2 -G0" CACHE STRING "EE C compiler flags" FORCE)
set(EE_LDFLAGS "-L$ENV{PS2SDK}/ee/lib -L$ENV{PS2DEV}/gsKit/lib -L$ENV{PS2SDK}/ports/lib -Wl,-zmax-page-size=128 -T$ENV{PS2SDK}/ee/startup/linkfile" CACHE STRING "EE linker flags" FORCE)

set(CMAKE_TARGET_INSTALL_PREFIX "$ENV{PS2DEV}/ports")
set(CMAKE_FIND_ROOT_PATH "$ENV{PS2DEV}" "$ENV{PS2DEV}/ee" "$ENV{PS2DEV}/ee/ee" "$ENV{PS2SDK}" "$ENV{PS2SDK}/ports")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
set(CMAKE_C_FLAGS_INIT "${EE_CFLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${EE_CFLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${EE_LDFLAGS}")
set(CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS "-nostartfiles -Wl,-r -Wl,-d")

find_program(PKG_CONFIG_EXECUTABLE NAMES mips64r5900el-ps2-elf-pkg-config HINTS "$ENV{PS2SDK}/bin")
if(NOT PKG_CONFIG_EXECUTABLE)
  message(FATAL_ERROR "Could not find mips64r5900el-ps2-elf-pkg-config")
endif()

set(PS2 TRUE)
set(PLATFORM_PS2 TRUE)
set(EE TRUE)
