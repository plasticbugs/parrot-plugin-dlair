# What in this repository came from elsewhere

The plugin's own code, in `src/`, is under the GNU GPL, version 3
(`LICENSE`). The copy of the cart's SDK in `sdk/` is under the MIT license
(`sdk/LICENSE`). These parts came from elsewhere and keep their own terms.

| Where | What | License |
|---|---|---|
| `src/dlair/core/` | the Z80 and AY-3-8910 cores from [pico-dragonslair](https://github.com/PicoPlus-devel/pico-dragonslair) (its `core/LICENSE.md` says which is which): MAME's Z80 (Juergen Buchmueller, Andrei I. Holub) and pico-dragonslair's own AY-3-8910 | BSD 3-Clause (the Z80, in each file's header) and GPL-3.0 (the rest, and pico-dragonslair as a whole) |
| `src/dlair/converter/lib/third_party/libchdr/` | [libchdr](https://github.com/rtissera/libchdr)'s Huffman, bitstream and FLAC wrappers (Romain Tisserand, Aaron Giles), and dr_flac (David Reid) | BSD 3-Clause (`LICENSE.txt` there); dr_flac public domain or MIT-0 |
| `src/dlair/converter/lib/dlc_chd.c`, `src/dlair/converter/plat/` | from pico-dragonslair's `tools/ld2pico` | GPL-3.0-or-later |
| `sdk/firmware/revc/sdk/tinyusb/` | [TinyUSB](https://github.com/hathach/tinyusb) (Ha Thach and contributors), with the cart's fixes, listed in its `VENDORED.md` | MIT (`sdk/firmware/revc/sdk/tinyusb/LICENSE`) |
| `cmake/pico_sdk_import.cmake` | from the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | BSD 3-Clause |

The built plugin also contains the Raspberry Pi Pico SDK's runtime (BSD
3-Clause) and TinyUSB (MIT).

## MAME

Parts of the code in `src/` are ports of MAME's code, or were written with
MAME's code as the reference. That code is under the BSD 3-Clause license
(its terms are in each file's header and in `src/dlair/core/LICENSE.md`).
It comes from MAME (<https://github.com/mamedev/mame>, tag `mame0288`),
and its copyright holders are these.

| Here | From MAME | Copyright holders |
|---|---|---|
| `src/dlair/dlmachine.c`: the Dragon's Lair board, its memory map, inputs and timing | `src/mame/cinematronics/dlair.cpp` | Aaron Giles |
| `src/dlair/dlmachine.c`: the LD-V1000 laserdisc player | `src/devices/machine/ldv1000.cpp`, `laserdsc.cpp` | Aaron Giles; Ryan Holtz |
| `src/dlair/core/z80.c`, `z80.h`, `z80.lst`, `z80_ops.h` | `src/devices/cpu/z80` | Juergen Buchmueller, Andrei I. Holub |
| `src/dlair/converter/lib/dlc_chd.c`: the laserdisc CHD's hunks | `src/lib/util/avhuff.cpp`, `chd.cpp` | Aaron Giles |

The zip reader in `src/starwars/swzip.c` follows RFC 1951 and the method of
zlib's `contrib/puff` by Mark Adler (zlib license). It was written afresh,
for the cart's Star Wars plugin.

Dragon's Lair is a trademark of Bluth Group, Ltd. Parrot Cart isn't
connected with them. This plugin doesn't include any game ROMs.

The 8x8 screen font's letter shapes come from the 1982 arcade game Mr. Do!
