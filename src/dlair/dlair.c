// dlair -- Dragon's Lair (Cinematronics, 1983) on the ColecoVision. The cart
// runs the arcade board and its LD-V1000 laserdisc player as MAME 0.288 does
// (../dlmachine.c), from the user's own ROMs (MAME's dlair set in /DLAIR),
// and plays the disc from /DLAIR/DLAIR.DAT, which the converter makes from
// MAME's dlair.chd. RESET goes back to the menu.
//
//   core 1  serves the kernel (../kernel/dlkernel.asm) and a list a field
//           (cvframes.c, with cvfserve.S's bus loop)
//   core 0  this loop: the board for one field, the disc's picture and
//           sound for it (../dlplay.c), as the next list
//
// The disc's line 17/18 codes (the frame numbers the player reads) are kept
// in PSRAM, read once at the start. So is the list of DLAIR.DAT's clusters
// when the card has it in more pieces than fsro maps (dlcardio.h).
//
// The menu's note (after RESET) says how far it got: "DL<stage> ...",
// stages 1-6 below. With DLAIR_DIAG (cmake -DDLAIR_DIAG=ON; 0.1.1 had it)
// the screen's border shows the same, after the kernel's own colours
// (kernel/dlkernel.asm, DIAG):
//
//   dark blue    the kernel runs, no list from the cart yet
//   magenta      the kernel took its first list (a moment)
//   grey         1  the plugin's lists play: the first message
//   light blue   2  the card is mounted: the ROMs are being read
//   cyan         3  the ROMs are in: DLAIR.DAT is being checked
//   light yellow 4  DLAIR.DAT is good: its frame numbers go into PSRAM
//   dark green   5  the game runs (the board takes about 35 seconds to
//                   start its disc: the screen is dark green till then)
//   black        6  the disc's first picture is on the screen
//   red             it stopped, with a message on the screen
//
// The note: "DL<stage> ..." while it starts (stage 4 with the pieces
// DLAIR.DAT is in on the card), the reason if it stopped, and while the
// game runs "DL<stage> <pictures a second> <late lists>L <longest field,
// ms> p<pictures drawn> f<card reads failed> s<bus count>".
#ifndef DLAIR_DIAG
#define DLAIR_DIAG 0
#endif
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include "cvdiag.h"
#include "cvframes.h"
#include "cvpad.h"
#include "cvpsram.h"
#include "cvsd.h"
#include "vdp.h"
#include "handoff.h"
#include "dlkernel.h"
#include "dlcardio.h"
#include "dlpad.h"
#include "dlplay.h"

#define OPEN_US     20000           // serve the console this long before the card's (blocking) start
#define WATCHDOG_MS 4000            // a hang reboots into the menu
#define NOTE_US     1000000         // the menu's note is brought up to date this often
#define FILE_MAX    0xA000          // the largest file looked at for ROMs (the set's zip is 30K)

static dlp_t play;
static uint8_t list[DLP_LIST_MAX];
static size_t list_len;
static bool list_pending;
static fs_t fs;
static fs_file_t dat;
static dl_dat_t dat_map;                    // DLAIR.DAT's sectors on the card
static uint8_t rom[DL_ROM_SIZE];
static union {
    canvas_t canvas;                // a message's
    uint8_t file[FILE_MAX];         // a file looked at for ROMs
} big __attribute__((aligned(4)));

// ---- the console, a list a field -----------------------------------------------------
static void publish(void) {
    if (list_pending && cvf_publish(list, list_len)) list_pending = false;
}

static void service(void) {
    cvpad_task();
    cvf_task(time_us_32());
    watchdog_update();
    publish();
}

// ---- the stages ----------------------------------------------------------------------------
enum { C_DIAG_STOP = 8, C_DIAG_GAME = 12 };
static const uint8_t stage_colour[7] = {1, 14, 5, 7, 11, 12, 1};
static unsigned stage;

static void set_stage(unsigned n, const char *what) {
    stage = n;
    cvd_stage(n);
    if (DLAIR_DIAG) dlp_border(&play, stage_colour[n]);
    char note[33];
    snprintf(note, sizeof note, "DL%u %s", n, what);
    cvf_note(note);
}

// ---- messages -------------------------------------------------------------------------
static void centre(int y, const char *s, uint8_t colour, int scale) {
    int w = (int)strlen(s) * 8 * scale;
    canvas_text(&big.canvas, w < VDP_W ? (VDP_W - w) / 2 : 0, y, s, colour, scale);
}

static void message(const char *head, const char *l1, const char *l2, const char *l3) {
    canvas_init(&big.canvas);
    centre(32, "DRAGON'S LAIR", C_WHITE, 2);
    if (head) centre(80, head, C_WHITE, 1);
    if (l1) centre(100, l1, C_WHITE, 1);
    if (l2) centre(112, l2, C_WHITE, 1);
    if (l3) centre(124, l3, C_WHITE, 1);
    centre(176, "RESET: BACK TO THE MENU", C_WHITE, 1);
    dlp_still(&play, (const uint8_t(*)[VDP_W])big.canvas.px, C_WHITE);
}

// serve the still picture for us microseconds (forever: 0)
static void serve_for(uint32_t us) {
    uint32_t t0 = time_us_32();
    while (!us || time_us_32() - t0 < us) {
        service();
        if (list_pending) continue;
        list_len = (size_t)dlp_still_field(&play, list);
        list_pending = true;
    }
}

static void __attribute__((noreturn)) fail(const char *why, const char *l2, const char *l3, const char *menu_note) {
    message("IT CAN'T START", why, l2, l3);
    if (DLAIR_DIAG) dlp_border(&play, C_DIAG_STOP);
    cvf_note(menu_note);
    serve_for(0);
    for (;;) tight_loop_contents();
}

// between a file's sectors: the console keeps its picture
static void card_tick(void) {
    service();
    if (list_pending) return;
    list_len = (size_t)dlp_still_field(&play, list);
    list_pending = true;
}

static bool dat_read(void *ctx, uint32_t sec, uint8_t *buf) {
    (void)ctx;
    uint32_t lba = dl_dat_lba(&dat_map, sec);
    return lba && cvsd_read(lba, buf);
}

// PSRAM: the frame numbers, then DLAIR.DAT's cluster list if it needs one
#define PSRAM_MAP_AT    0x100000u
#define PSRAM_MAP_MAX   ((0x800000u - PSRAM_MAP_AT) / 4)

// ---- the game ------------------------------------------------------------------------------
static dlpad_t pad;
static uint32_t late, us_max, drawn_at_note, last_frames;

static void update_note(void) {
    static uint32_t t_last, late_last;
    uint32_t t = time_us_32();
    if (t - t_last < NOTE_US) return;
    unsigned per_s = MIN(play.drawn - drawn_at_note, 99u), dl = MIN(late - late_last, 99u), ms = MIN(us_max / 1000, 99u),
             tenths = us_max / 100 % 10;
    uint32_t unsel, early, unhdr;
    cvf_bus_counts(&unsel, &early, &unhdr);
    char n[48];
    snprintf(n, sizeof n, "DL%u %u/s %uL %u.%ums p%u f%u s%u", stage, per_s, dl, ms, tenths, (unsigned)MIN(play.drawn, 9999u),
             (unsigned)MIN(play.read_fail, 99u), (unsigned)MIN(unsel, 999u));
    cvf_note(n);
    t_last = t;
    late_last = late;
    drawn_at_note = play.drawn;
    us_max = 0;
}

int main(void) {
    static const cvf_config_t cfg = {dlkernel_bin, DLKERNEL_BIN_LEN, handoff_bin, HANDOFF_BIN_LEN};
    static const dlc_header_t none;
    cvf_start(&cfg);
    cvd_start();
    dlp_init(&play, &none, NULL, NULL, NULL, NULL);
    message("LOOKING FOR THE DISC", "IN " DL_HOME " ON THE CARD", NULL, NULL);
    set_stage(1, "start");
    list_len = (size_t)dlp_still_field(&play, list);
    list_pending = true;
    publish();                              // the kernel's first list, before it asks
    cvpad_init();
    serve_for(OPEN_US);

    if (!cvsd_card_in() || !cvsd_init()) fail(cvsd_card_in() ? "THE CARD DIDN'T ANSWER" : "NO CARD IN THE SLOT", NULL, NULL, "Dragon's Lair: no card");
    if (fs_mount(&fs, cvsd_read) != FS_OK) fail("NO FAT OR EXFAT ON THE CARD", NULL, NULL, "Dragon's Lair: no FAT");
    set_stage(2, "card");
    watchdog_enable(WATCHDOG_MS, true);

    uint32_t found = dl_load_roms(&fs, rom, big.file, FILE_MAX, card_tick);
    if (found != DL_ROMS_ALL) {
        int missing = 0;
        for (int i = 0; i < DL_ROMFILES; i++) missing += !(found & 1u << i);
        char l2[40], note[33];
        snprintf(l2, sizeof l2, "%d OF ITS 4 ROMS MISSING", missing);
        snprintf(note, sizeof note, "Dragon's Lair: %d ROMs missing", missing);
        fail("PUT MAME'S DLAIR.ZIP IN " DL_HOME, l2, NULL, note);
    }
    set_stage(3, "ROMs");
    if (!cvpsram_init()) fail("THE CART'S PSRAM DIDN'T ANSWER", NULL, NULL, "Dragon's Lair: no PSRAM");
    dlc_header_t h;
    switch (dl_open_dat(&fs, &dat, &h, &dat_map, (uint32_t *)(CVPSRAM_CACHED + PSRAM_MAP_AT), PSRAM_MAP_MAX, card_tick)) {
    case DL_DAT_OK: break;
    case DL_DAT_MISSING: fail("NO DLAIR.DAT IN " DL_HOME, "MAKE IT FROM DLAIR.CHD WITH", "THE DLAIR-CONVERT TOOL", "Dragon's Lair: no DLAIR.DAT");
    case DL_DAT_UNFINISHED: fail("DLAIR.DAT ISN'T FINISHED", "RUN DLAIR-CONVERT AGAIN", "TO FINISH IT", "Dragon's Lair: DAT unfinished");
    case DL_DAT_OTHER_DISC: fail("DLAIR.DAT IS FROM ANOTHER DISC", "IT NEEDS MAME'S DLAIR.CHD", NULL, "Dragon's Lair: wrong disc");
    case DL_DAT_PIECES: fail("DLAIR.DAT IS IN TOO MANY PIECES", "ON THE CARD. COPY IT TO AN", "EMPTY CARD, OR MAKE IT AGAIN", "Dragon's Lair: DAT in pieces");
    default: fail("DLAIR.DAT CAN'T BE READ", "MAKE IT AGAIN WITH", "THE DLAIR-CONVERT TOOL", "Dragon's Lair: bad DLAIR.DAT");
    }
    {
        char what[24];
        snprintf(what, sizeof what, "DAT %lu pieces", (unsigned long)dat_map.pieces);
        set_stage(4, what);
    }
    uint8_t *vbi = CVPSRAM_CACHED;
    for (uint32_t i = 0; i < h.vbi_sectors; i++) {
        if (!dat_read(NULL, h.vbi_sector + i, vbi + i * DLC_SECTOR)) fail("DLAIR.DAT CAN'T BE READ", NULL, NULL, "Dragon's Lair: read failed");
        if (!(i & 7)) card_tick();
    }
    while (list_pending) service();         // the message's last list out
    dlp_game(&play, &h, (const uint32_t *)vbi, rom, dat_read, NULL);    // VRAM keeps the message till the game draws
    set_stage(5, "game");
    last_frames = cvf_frames();

    for (;;) {
        service();
        if (list_pending) continue;
        uint32_t f = cvf_frames();
        if (f - last_frames > 1) late += f - last_frames - 1;   // a list ran twice
        last_frames = f;
        uint32_t t0 = time_us_32();
        uint8_t joy, fire, key;
        cvf_input(&joy, &fire, &key);
        gamepad_t g;
        bool have = cvpad_get(0, &g);
        list_len = (size_t)dlp_field(&play, dlpad_field(&pad, joy, fire, key, have ? &g : NULL), list);
        list_pending = true;
        if (stage == 5 && play.target_pic && !play.blank) set_stage(6, "picture");
        publish();
        uint32_t us = time_us_32() - t0;
        if (us > us_max) us_max = us;
        update_note();
    }
}
