// dlcard.h -- /DLAIR/DLAIR.DAT: the laserdisc made ready for the cart by
// the converter (converter/), read by the plugin as it plays. One file,
// in 512-byte sectors, all numbers little-endian:
//
//   sector 0             the header (dlc_header_t)
//   vbi_sector ..        a u32 a field (CHD hunk): its line 17/18 code
//                        (24 bits, as MAME's vbi_metadata.line1718; 0 if none)
//   audio_sector ..      a sector a field: DLC_SAMPLES left samples, then
//                        DLC_SAMPLES right (int8, the field's 735.7 samples
//                        at 44.1 kHz filtered and resampled to 194, at a
//                        fixed gain; the rest of the sector is 0)
//   video_sector ..      a record of pic_sectors sectors a picture number,
//                        first_picture on: its 6144 pattern bytes in VRAM
//                        order ($0000-$17FF, names 0..255 three times), then,
//                        if has_colour, its 6144 colour bytes ($2000-$37FF).
//                        A picture number with no fields on the disc is black.
//
// The pattern byte of pixel row y, column cell c is at (y/8)*256 + c*8 + y%8.
// The picture is the picture number's first two fields woven, its 704x480
// active area scaled to 256x192 and encoded as Movies does (the look).
#pragma once
#include <stdint.h>

#define DLC_MAGIC       "DLAIRCV1"
#define DLC_VERSION     1
#define DLC_SECTOR      512
#define DLC_SAMPLES     194             // audio samples a field (each channel)
#define DLC_PATTERN     6144
#define DLC_HUNKS_MAX   80000

enum { DLC_LOOK_BW, DLC_LOOK_GREY, DLC_LOOK_COOL, DLC_LOOK_VIVID, DLC_LOOKS };

typedef struct {
    char magic[8];                      // DLC_MAGIC
    uint32_t version;                   // DLC_VERSION
    uint32_t header_bytes;              // sizeof(dlc_header_t)
    uint8_t sha1[20];                   // the CHD's SHA-1, as MAME lists it
    uint8_t look;                       // DLC_LOOK_*
    uint8_t has_colour;                 // 1: a colour table in every record
    uint8_t colour;                     // else every cell's colour byte
    uint8_t complete;                   // 1 once everything is written (the plugin refuses 0)
    uint32_t hunks;                     // fields on the disc (79,484)
    uint32_t first_picture;             // the lowest picture number
    uint32_t pictures;                  // records: first_picture .. first_picture+pictures-1
    uint32_t vbi_sector, vbi_sectors;
    uint32_t audio_sector;              // hunks sectors
    uint32_t video_sector;              // pictures * pic_sectors sectors
    uint32_t pic_sectors;               // 12, or 24 with colour
    uint32_t total_sectors;             // the file's size / 512
    uint32_t samples_per_field;         // DLC_SAMPLES
    uint32_t reserved[8];
    uint8_t resume[256];                // the converter's own (which parts are done)
} dlc_header_t;
_Static_assert(sizeof(dlc_header_t) <= DLC_SECTOR, "the header is a sector");
