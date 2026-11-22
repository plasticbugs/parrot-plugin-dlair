# Dragon's Lair

This plugin plays the 1983 Dragon's Lair laserdisc arcade game on the ColecoVision. It needs your own copy of the game's ROMs and its disc, as MAME's `dlair` set and `dlair.chd`.

## Setup

1. Copy `DLAIR.CVP` into the `/PLUGINS` folder on the card.
2. Run the converter on your computer with the card in it: `dlair-convert dlair.chd /Volumes/CARD`. It writes `/DLAIR/DLAIR.DAT`, about 240 MB, in a few minutes. If you stop it, run it again and it carries on.
3. Put `dlair.zip` from MAME's `dlair` set in the `/DLAIR` folder. The four ROMs can go there loose too.
4. Start Dragon's Lair from the Plugins page in the menu.

The converter checks that the CHD is the disc MAME lists for `dlair` before it starts. Only that set works.

## Controls

On the ColecoVision controller:

- The joystick moves Dirk.
- Either fire button swings the sword.
- Keypad * or # adds a coin. The game takes two coins for a credit.
- Keypad 1 starts a one-player game, and keypad 2 a two-player game.
- Press RESET on the console to go back to the menu.

On a USB pad, through a powered hub:

- The D-pad or the left stick moves Dirk.
- Any face button, L, R, ZL or ZR swings the sword.
- Minus adds a coin, and plus starts the game.

## The picture

The console can't show every picture from the disc. It draws each one in full before starting the next, about eight or nine a second during play. The screen goes black while the player looks for a scene, as the arcade machine does.

## Credits

The arcade board and the laserdisc player are ports of MAME's code, which is under the BSD-3-Clause license. Its authors are:

- Aaron Giles, for the Dragon's Lair driver and the laserdisc player.
- Ryan Holtz, for the LD-V1000 player.
- Juergen Buchmueller and Andrei Holub, for the Z80 CPU.

The Z80 and AY-3-8910 code comes from pico-dragonslair (https://github.com/PicoPlus-devel/pico-dragonslair), under the GPL-3.0. The converter reads CHD files with libchdr, under the BSD-3-Clause license, and decodes the disc's sound with dr_flac by David Reid. The zip reader follows Mark Adler's puff, from zlib. USB support comes from TinyUSB by Ha Thach, under the MIT license. You can find MAME at https://www.mamedev.org.

The plugin itself is by Parrot Cart. It's free, and it's under the GPL-3.0. Its source is linked next to its download.

Dragon's Lair is a trademark of its owners. This plugin contains none of the game.
