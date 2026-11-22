# dlair-convert: Dragon's Lair's laserdisc (MAME's dlair.chd) made into
# DLAIR/DLAIR.DAT for the Parrot Cart's Dragon's Lair plugin.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release
#
# Plain C11 and the system's own threads: macOS, Linux and Windows (MSVC or
# MinGW). The work is in lib/, which reads no files (a WebAssembly build can
# use it as it is); main.c and plat/ are the command line, files and threads.
#
# Third-party code:
#   lib/third_party/libchdr   Huffman, bitstream and FLAC wrappers (BSD-3-Clause, Romain Tisserand,
#                             Aaron Giles), dr_flac (public domain or MIT-0, David Reid)
#   lib/dlc_chd.c, plat/      from pico-dragonslair's tools/ld2pico (GPL-3.0-or-later); its hunk
#                             format follows MAME's avhuff.cpp and chd.cpp (BSD-3-Clause, Aaron Giles)
#   lib/dlc_video.c, lib/dlc_audio.c  ports of the Parrot Cart movie converter (app/engine)
# The whole tool is under the GPL-3.0-or-later.
cmake_minimum_required(VERSION 3.13)
project(dlair_convert C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(LIBCHDR ${CMAKE_CURRENT_LIST_DIR}/lib/third_party/libchdr)
add_library(dlc STATIC
  lib/dlc_chd.c lib/dlc_video.c lib/dlc_audio.c lib/dlc_png.c lib/dlc_card.c
  ${LIBCHDR}/src/libchdr_bitstream.c ${LIBCHDR}/src/libchdr_huffman.c ${LIBCHDR}/src/libchdr_flac.c)
target_include_directories(dlc PUBLIC ${CMAKE_CURRENT_LIST_DIR}/lib ${CMAKE_CURRENT_LIST_DIR}/.. ${LIBCHDR}/include)
# the same bytes as the movie converter: no fused multiply-adds
if(MSVC)
  target_compile_options(dlc PRIVATE /fp:precise)
  target_compile_definitions(dlc PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(dlc PRIVATE -ffp-contract=off)
  set_source_files_properties(lib/dlc_chd.c lib/dlc_video.c lib/dlc_audio.c lib/dlc_png.c lib/dlc_card.c
                              PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
endif()
find_library(MATH m)
if(MATH)
  target_link_libraries(dlc PUBLIC ${MATH})
endif()

if(WIN32)
  set(PLAT plat/ldp_win32.c)
else()
  set(PLAT plat/ldp_posix.c)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
endif()

add_executable(dlair-convert main.c ${PLAT})
target_include_directories(dlair-convert PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/plat)
target_link_libraries(dlair-convert PRIVATE dlc)
if(WIN32)
  target_link_libraries(dlair-convert PRIVATE shell32)
  if(MSVC)
    target_compile_definitions(dlair-convert PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_link_options(dlair-convert PRIVATE -static)
  endif()
else()
  target_link_libraries(dlair-convert PRIVATE Threads::Threads)
endif()

# dlair-check: reads a DLAIR.DAT back (a test, not for users)
add_executable(dlair-check check.c)
target_link_libraries(dlair-check PRIVATE dlc)
if(MSVC)
  target_compile_definitions(dlair-check PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
