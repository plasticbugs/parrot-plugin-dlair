// dlmachine.h -- the Dragon's Lair arcade board (US rev. F2) with its
// Pioneer LD-V1000 player, as MAME 0.288 runs them: the dlair driver
// (src/mame/cinematronics/dlair.cpp), the player's high-level emulation
// (src/devices/machine/ldv1000hle.cpp) and the laserdisc base it stands on
// (src/devices/machine/laserdsc.cpp), with MAME's own timing.
//
// Time is MAME's, in attoseconds: the screen's frame period comes from its
// 28,636,362 Hz pixel clock as MAME divides it, the IRQ is 16 MHz/8/65536,
// and each of the player's timers fires when MAME's would. MAME's Z80 stops
// in the middle of an instruction for a timer, so a read sees an event from
// the cycle the event falls in; the board does the same, and so the same
// inputs give the same commands, frame numbers and status reads (the Mac
// test, test/compare.sh, checks this against MAME). A polling loop that
// cannot change before the next event is stepped over whole turns at a time,
// the cycles and the R register advanced as running it would have.
//
// Portable C: the plugin and the Mac test share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "core/ay8910.h"
#include "core/z80.h"

#define DL_ROM_SIZE     0x8000          // dl_f2_u1..u4
#define DL_APC          250000000000LL  // attoseconds a CPU cycle (4 MHz)
#define DL_CYCLES_S     4000000

// dl_machine_t.inputs, as test/dltrace.lua takes them
#define DL_IN_UP        0x001
#define DL_IN_DOWN      0x002
#define DL_IN_LEFT      0x004
#define DL_IN_RIGHT     0x008
#define DL_IN_SWORD     0x010
#define DL_IN_START1    0x100
#define DL_IN_START2    0x200
#define DL_IN_COIN1     0x400
#define DL_IN_COIN2     0x800

// MAME's defaults for dlair (its dlaire ports)
#define DL_DSW1_DEFAULT 0x20
#define DL_DSW2_DEFAULT 0x79

typedef struct {
    int64_t c;                          // CPU cycles since the start
    int64_t a;                          // and attoseconds into the next (0 <= a < DL_APC)
} dl_time_t;

enum {
    DLT_VBLANK, DLT_FETCH, DLT_VBI, DLT_ST_ON, DLT_ST_OFF, DLT_CMD_ON, DLT_CMD_OFF, DLT_PARK, DLT_STOP, DLT_COUNT
};

typedef struct {
    dl_time_t at;
    dl_time_t period;                   // 0: once
    uint32_t seq;                       // equal times fire in the order they were set
    int32_t param;
    bool armed;
} dl_timer_t;

enum { DLM_PARK, DLM_PLAY, DLM_SCAN_FORWARD, DLM_SCAN_REVERSE, DLM_SEARCH, DLM_STOP };

typedef struct {
    uint8_t numfields;
    uint32_t code;                      // the line 17/18 code of its first field
} dl_frame_t;

typedef struct dl_machine dl_machine_t;

// what the board did, for a log (test/dlmodel.c) or nothing
typedef void (*dl_log_fn)(void *ctx, char kind, dl_time_t t, uint32_t a, uint32_t b);

struct dl_machine {
    z80_t cpu;
    ay8910_t ay;
    uint8_t ram[0x800];
    const uint8_t *rom;                 // DL_ROM_SIZE bytes
    const uint32_t *vbi;                // the disc's line 17/18 code a hunk
    uint32_t hunks;

    uint16_t inputs;                    // DL_IN_*
    uint8_t dsw1, dsw2;
    uint8_t ay_latch, last_misc, ld_latch;
    bool ay_active;                     // the AY's chip select (an address write with 0 on top)
    uint8_t leds[16];                   // the scoreboard digits (low nibble; 0x0f blank)
    uint32_t led_serial;

    // time
    int64_t given;                      // cycles handed to the CPU
    int64_t irq_next;                   // the next periodic IRQ, in cycles
    int64_t run_stop;                   // where the current run ends
    dl_timer_t t[DLT_COUNT];
    uint32_t seq, events;
    uint64_t fields;                    // vblanks so far

    // the player (ldv1000hle)
    uint8_t cmd_buffer[21], cmd_length;
    uint8_t status, pre_stop_status, mode;
    uint32_t curr_frame, search_frame, stop_frame;
    uint8_t cmd_number[5], cmd_number_length;
    uint16_t user_ram[1024];
    uint8_t curr_register;
    int32_t scan_speed, scan_speed_accum;
    double play_speed;
    bool audio_enable[2];
    bool status_strobe, command_strobe;

    // the laserdisc base (laserdsc)
    int32_t curtrack, maxtrack, chdtracks;
    uint8_t fieldnum;
    uint32_t metadata[2];               // line 17/18 of the field read, each field
    bool videoenable, videosquelch;
    uint8_t audiosquelch;               // bit 0 left, bit 1 right
    int64_t attospertrack;
    dl_time_t sliderupdate;
    dl_frame_t frame[3];
    uint8_t videoindex;
    bool readok;
    int32_t readhunk, lasthunk;         // the hunk read at the last fetch, and the one before (its audio plays now)

    // the AY into the field's samples (dl_run_field)
    int16_t *audio;
    int audio_n, audio_done;
    int64_t audio_c0, audio_len;
    int ay_level;

    // idle skip
    bool dirty;
    uint16_t idle_pc;
    int64_t idle_c;
    uint8_t idle_r;
    uint32_t idle_events;
    uint8_t idle_state[40];
    bool skip;                          // on unless switched off (the Mac test checks both ways)
    uint64_t skipped;                   // cycles stepped over

    dl_log_fn log;
    void *log_ctx;
};

// rom: DL_ROM_SIZE bytes; vbi: hunks codes (the card's table)
void dl_init(dl_machine_t *m, const uint8_t *rom, const uint32_t *vbi, uint32_t hunks);
void dl_reset(dl_machine_t *m);

// runs until the time of the next vblank, then starts that vblank's work
// (the log's F line is at that point, as MAME's frame_done)
void dl_run_to_vblank(dl_machine_t *m);

// what the screen shows now: the picture number (0 if it is black)
uint32_t dl_picture(const dl_machine_t *m);

// One field for the plugin: up to the next vblank, the AY rendered into
// ay[0..n) (the field's samples, at the board's level). Returns the picture
// to show (0: black).
uint32_t dl_run_field(dl_machine_t *m, int16_t *ay, int n);

// the time of vblank k (k >= 1), and a time as seconds.attoseconds
dl_time_t dl_vblank_time(uint64_t k);
void dl_time_str(dl_time_t t, char *buf, int len);
