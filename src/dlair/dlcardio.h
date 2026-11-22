// dlcardio.h -- the plugin's files on the card, through fsro: the ROMs
// (MAME's dlair set in /DLAIR, loose or as its zip, each known by its CRC-32)
// and the disc the converter made (/DLAIR/DLAIR.DAT). Portable.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../sdplayer/fsro.h"
#include "dlcard.h"

#define DL_HOME         "/DLAIR"
#define DL_DAT          "DLAIR.DAT"
#define DL_ROMFILES     4
#define DL_ROMSIZE      0x2000
#define DL_ROMS_ALL     0xFu

typedef struct {
    const char *name;
    uint32_t crc;
    uint32_t at;                        // its place in the 32K
} dl_romfile_t;
extern const dl_romfile_t dl_romfiles[DL_ROMFILES];
extern const uint8_t dl_chd_sha1[20];

uint32_t dl_crc32(const uint8_t *p, uint32_t n);

// Every file in /DLAIR up to max bytes, read into buf: a ROM of the set goes
// to its place in rom (32K), a zip's ROMs likewise. tick: between sectors,
// or NULL. Returns the found bits (DL_ROMS_ALL: the whole set).
uint32_t dl_load_roms(fs_t *fs, uint8_t *rom, uint8_t *buf, uint32_t max, void (*tick)(void));

// /DLAIR/DLAIR.DAT opened and its header checked; dl_dat_lba then maps its
// sectors. A file in more pieces than fsro maps (FS_MAX_EXT, as when a card's
// free space is broken up) gets every cluster listed in map (map_max
// entries; the plugin's is in PSRAM), from the FAT, tick between FAT
// sectors. DL_DAT_PIECES: more clusters than map holds.
enum { DL_DAT_OK, DL_DAT_MISSING, DL_DAT_UNREADABLE, DL_DAT_NOT_ONE, DL_DAT_OTHER_DISC, DL_DAT_UNFINISHED, DL_DAT_PIECES };
typedef struct {
    fs_t *fs;
    uint32_t *map;                      // NULL: fsro's own extents (fs_lba)
    uint32_t clusters, pieces;          // the map's length; the runs in it
} dl_dat_t;
int dl_open_dat(fs_t *fs, fs_file_t *f, dlc_header_t *h, dl_dat_t *d, uint32_t *map, uint32_t map_max, void (*tick)(void));
uint32_t dl_dat_lba(dl_dat_t *d, uint32_t sec);     // 0: past the end
