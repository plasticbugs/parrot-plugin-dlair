// dlplay.h -- one arcade field as one list for the kernel (kernel/dlkernel.asm):
// the board (dlmachine.c) runs a field, and its picture and sound become
// 97 packets (194 samples, 11,660 Hz on the console):
//
//   the sound   the laserdisc's field from DLAIR.DAT (its left and right as
//               the player lets them through), the AY's beeps on top, each
//               sample a level of Movies' PSG table (dlpsg.h)
//   the picture the disc's picture from DLAIR.DAT, drawn into VRAM by
//               the cells that differ from what the console has, 95 packets
//               (cells) a field at most, top to bottom. A picture is drawn
//               whole before the newest one is begun: a dithered picture
//               changes nearly every cell, and 95 a field make a whole one
//               in 8 fields, so pictures are skipped rather than torn.
//               Black (the display blanked) while the player shows no
//               picture, as MAME does; during a search the picture being
//               looked for is drawn behind the blank
//   then        KEYS (the controller) and END (the next list)
//
// Portable: the plugin and the Mac test (test/dlhost.c) share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "dlcard.h"
#include "dlmachine.h"

#define DLP_PACKETS     97
#define DLP_VIDEO       (DLP_PACKETS - 2)
#define DLP_LIST_MAX    (DLP_PACKETS * 17)
#define DLP_CELLS       768

// sec: a sector of DLAIR.DAT into buf (false: it couldn't)
typedef bool (*dlp_read_fn)(void *ctx, uint32_t sec, uint8_t *buf);

typedef struct {
    // what the console has (dlp_game keeps it): VRAM's patterns and colours,
    // the display on or off, the backdrop colour (VDP R7)
    uint8_t shadow[2][DLC_PATTERN];
    bool blank_sent;
    uint8_t border_sent;

    dl_machine_t m;
    dlc_header_t h;
    dlp_read_fn read;
    void *read_ctx;

    uint8_t target[2][DLC_PATTERN];     // what VRAM should have
    uint32_t target_pic;                // the picture in target (0: a message, or nothing yet)
    uint32_t want;                      // the picture the player shows (or seeks)
    uint16_t cursor;                    // the next cell to look at
    bool blank;                         // the display off
    uint8_t border;                     // the backdrop colour

    int32_t audio_hunk;                 // the hunk in audio_sec
    uint8_t audio_sec[DLC_SECTOR];
    int16_t ay[DLC_SAMPLES];
    uint8_t level[DLC_SAMPLES];         // each sample's PSG level
    uint16_t keys_in;                   // the board's inputs this field

    // counts, for the menu's note and the Mac test
    uint32_t fields, pictures, drawn, cells, sectors, read_fail;
    bool target_done;
} dlp_t;

// header: DLAIR.DAT's (checked); vbi: its table; rom: the board's ROMs
// (NULL: messages only). The console as the kernel leaves it at its start.
void dlp_init(dlp_t *p, const dlc_header_t *header, const uint32_t *vbi, const uint8_t *rom, dlp_read_fn read, void *ctx);
// The same, after messages: what the console has stays as it is.
void dlp_game(dlp_t *p, const dlc_header_t *header, const uint32_t *vbi, const uint8_t *rom, dlp_read_fn read, void *ctx);

// One field: the board's inputs (DL_IN_*) for it, then the field run and
// its list made into list[0..DLP_LIST_MAX). Returns the list's length.
int dlp_field(dlp_t *p, uint16_t inputs, uint8_t *list);

// A still picture (a message): px[192][256] a colour a pixel as vdp.h's
// canvas has it, where 0 and 1 are both black (C_BLACK is 1); every other
// colour shows as fg on black. Then dlp_still_field() for each field
// (silence).
void dlp_still(dlp_t *p, const uint8_t px[192][256], uint8_t fg);
int dlp_still_field(dlp_t *p, uint8_t *list);

// The backdrop (the border, and the whole screen while it is blanked) in
// colour c (1 black), from the next list on: the plugin's diagnostic build
// shows its stage with it.
static inline void dlp_border(dlp_t *p, uint8_t c) { p->border = c & 15; }

// the list's packets as the console would run them, into a 16K VRAM, the
// VDP's registers (regs[8]) and the PSG level of each sample (the Mac
// tests); false if the list is malformed
bool dlp_run_list(const uint8_t *list, int n, uint8_t *vram, uint8_t *regs, uint8_t *levels, uint8_t *reports);
