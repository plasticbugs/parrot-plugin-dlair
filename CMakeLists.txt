# dlair (DLAIR.CVP): the Dragon's Lair laserdisc arcade game on the
# ColecoVision. The cart runs the arcade board and its LD-V1000 player
# (src/dlair/dlmachine.c) from the user's own ROMs on the card (/DLAIR), and plays
# the disc from /DLAIR/DLAIR.DAT, which src/dlair/converter makes from MAME's
# dlair.chd. Its own kernel (src/dlair/kernel/dlkernel.asm) takes a list a field:
# the sound as PSG samples, the picture as VRAM writes. RESET goes back to
# the menu.
#
#   cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
#   ninja -C build                      -> build/DLAIR.CVP
#
# Copy DLAIR.CVP into the card's /PLUGINS folder.
cmake_minimum_required(VERSION 3.13)

# the cart's SDK: the copy in sdk/ (sdk/README.md)
set(PARROTCART_SDK ${CMAKE_CURRENT_LIST_DIR}/sdk/firmware/revc)
set(PICO_BOARD_HEADER_DIRS ${PARROTCART_SDK}/cvcart_revc/boards)
set(PICO_BOARD cvcart_revc)
set(PICO_PLATFORM rp2350-arm-s)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# the USB host: sdk/tinyusb (TinyUSB 0.21 and its fixes since), not the pico-sdk's 0.18
include(${PARROTCART_SDK}/sdk/cvpad_tinyusb.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(dlair C CXX ASM)
pico_sdk_init()
include(${PARROTCART_SDK}/sdk/cvplugin.cmake)

# The same source makes the same file, whenever it's built: the compiler's
# __DATE__ and __TIME__ (the build date in the binary info) come from
# SOURCE_DATE_EPOCH, fixed here at the tested build's day, 2026-11-21.
set(BUILD_EPOCH 1795262400)
set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})
set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})

# the plugin's code (the cart's firmware/revc/dlair/, with plugin/ beside it)
set(S ${CMAKE_CURRENT_LIST_DIR}/src/dlair)

# DLAIR_DIAG: the border's colour shows how far it got, as the menu's note
# always does (dlair.c's header); 0.1.1 had it on
option(DLAIR_DIAG "the stages in the border's colour too" OFF)
if (DLAIR_DIAG)
  set(KDIAG -DDIAG)
  set(DIAG_VAL 1)
else()
  set(KDIAG "")
  set(DIAG_VAL 0)
endif()

# the kernel, and Movies' PSG levels from the measured ladder
set(DL ${S})
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/dlgen)
file(MAKE_DIRECTORY ${GEN})
add_custom_command(
    OUTPUT ${GEN}/dlkernel.bin
    COMMAND sjasmplus --nologo ${KDIAG} ${DL}/kernel/dlkernel.asm
    WORKING_DIRECTORY ${GEN}                # SAVEBIN writes to the cwd
    DEPENDS ${DL}/kernel/dlkernel.asm)
add_custom_command(
    OUTPUT ${GEN}/dlkernel.h
    COMMAND python3 ${CVSDK_REVC}/sdplayer/bin2c.py ${GEN}/dlkernel.bin dlkernel_bin ${GEN}/dlkernel.h
    DEPENDS ${GEN}/dlkernel.bin ${CVSDK_REVC}/sdplayer/bin2c.py)
add_custom_command(
    OUTPUT ${GEN}/dlpsg.h
    COMMAND python3 ${DL}/tools/mkpsgtab.py ${S}/ladder.json ${GEN}/dlpsg.h
    DEPENDS ${DL}/tools/mkpsgtab.py ${S}/ladder.json)

set(OURS ${S}/dlair.c ${S}/dlplay.c ${S}/dlmachine.c ${S}/dlcardio.c ${S}/core/z80.c ${S}/core/ay8910.c ${CMAKE_CURRENT_LIST_DIR}/src/starwars/swzip.c)
set_source_files_properties(${S}/dlair.c ${S}/dlplay.c ${S}/dlmachine.c ${S}/dlcardio.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
set_source_files_properties(${S}/core/z80.c ${S}/core/ay8910.c PROPERTIES COMPILE_OPTIONS "-O3")

add_executable(dlair ${OURS} ${PARROTCART_SDK}/midihello/vdp.c ${GEN}/dlkernel.h ${GEN}/dlpsg.h)
target_include_directories(dlair PRIVATE ${DL} ${DL}/core ${GEN} ${PARROTCART_SDK}/midihello)
target_compile_definitions(dlair PRIVATE PICO_PRINTF_SUPPORT_FLOAT=0 PICO_PRINTF_SUPPORT_EXPONENTIAL=0 DLAIR_DIAG=${DIAG_VAL})
cvframes_use(dlair)
cvframes_serve2(dlair)      # core 1's bus loop acts on two samples of the pins that agree (sdk/cvfserve.S)
cvsd_use(dlair)
cvpad_use(dlair)                            # USB game pads: the port is a host
cvpsram_use(dlair)                          # the disc's frame numbers
pico_enable_stdio_usb(dlair 0)
pico_enable_stdio_uart(dlair 0)
cvcart_plugin(dlair ID parrotcart.dlair NAME "Dragon's Lair" VERSION 0.1.6 BY "Parrot Cart"
              HOME /DLAIR USES usb-host ICON src/dlair/dlair_icon.txt FILE DLAIR.CVP README CART-README.md)
