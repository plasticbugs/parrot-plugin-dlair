// dlplay.c -- see dlplay.h.
#include "dlplay.h"

#include <stddef.h>
#include <math.h>
#include <string.h>

#include "dlpsg.h"

#define R1_ON   0xC0                    // 16K, display on (no interrupts)
#define R1_OFF  0x80                    // the display blanked
#define SILENT  128                     // the middle level

enum { OP_ADDR, OP_IDLE, OP_CONT, OP_KEYS, OP_REG, OP_END };

// The sound's level. The disc's soundtrack (nearly all on its right channel) sits about 20 dB under
// the cart's full level, its loudest moments at three quarters of it, so the TV had to be turned
// right up. The disc and the board's AY together play louder, the balance between them as before,
// with a soft knee from 3/4 of full scale: the loudest moments round off, never clip. 0.1.3 made it
// 2.5 times as loud, still some 7 dB under an ordinary ColecoVision cartridge (a game's square tones
// swing twice as far as the parked tones' levels can); 0.1.6 makes it 5.5 times (+6.8 dB over
// 0.1.3), and 5.8% of the scripted game's samples go into the knee (0.7% at 2.5).
#ifndef LOUD_GAIN
#define LOUD_GAIN 5.5f
#endif
#define LOUD_KNEE 96.0f
static int8_t loud[768];                // [v + 384], v = left + right + the AY's top byte
static bool loud_ready;

static void loud_init(void) {
    if (loud_ready) return;
    for (int i = 0; i < 768; i++) {
        const float x = (float)(i - 384) * LOUD_GAIN;
        float a = fabsf(x);
        if (a > LOUD_KNEE) a = LOUD_KNEE + (127.0f - LOUD_KNEE) * (1.0f - expf(-(a - LOUD_KNEE) / (127.0f - LOUD_KNEE)));
        int q = (int)lroundf(a);
        if (q > 127) q = 127;
        loud[i] = (int8_t)(x < 0.0f ? -q : q);
    }
    loud_ready = true;
}

static void setup(dlp_t *p, const dlc_header_t *header, const uint32_t *vbi, const uint8_t *rom, dlp_read_fn read, void *ctx) {
    loud_init();
    dl_log_fn log = p->m.log;
    void *log_ctx = p->m.log_ctx;
    memset(&p->m, 0, sizeof *p - offsetof(dlp_t, m));
    p->h = *header;
    p->read = read;
    p->read_ctx = ctx;
    p->m.log = log;
    p->m.log_ctx = log_ctx;
    p->m.skip = true;
    if (rom) dl_init(&p->m, rom, vbi, header->hunks);
    p->m.ay_level = 4096;               // a beep's channel at 16 of the sound's +-127
    p->audio_hunk = -1;
    if (!p->h.has_colour) memset(p->target[1], p->h.colour, DLC_PATTERN);
    p->blank = p->blank_sent;
    p->border = p->border_sent;
}

void dlp_init(dlp_t *p, const dlc_header_t *header, const uint32_t *vbi, const uint8_t *rom, dlp_read_fn read, void *ctx) {
    // the kernel starts with VRAM cleared, the display on, a black backdrop
    memset(p->shadow, 0, sizeof p->shadow);
    p->blank_sent = false;
    p->border_sent = 1;
    setup(p, header, vbi, rom, read, ctx);
}

void dlp_game(dlp_t *p, const dlc_header_t *header, const uint32_t *vbi, const uint8_t *rom, dlp_read_fn read, void *ctx) {
    setup(p, header, vbi, rom, read, ctx);
}

static bool read_secs(dlp_t *p, uint32_t sec, uint8_t *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++, buf += DLC_SECTOR) {
        p->sectors++;
        if (!p->read(p->read_ctx, sec + i, buf)) {
            p->read_fail++;
            return false;
        }
    }
    return true;
}

static void load_picture(dlp_t *p, uint32_t pic) {
    if (pic < p->h.first_picture || pic >= p->h.first_picture + p->h.pictures) return;
    uint32_t sec = p->h.video_sector + (pic - p->h.first_picture) * p->h.pic_sectors;
    if (!read_secs(p, sec, p->target[0], DLC_PATTERN / DLC_SECTOR)) return;
    if (p->h.has_colour && !read_secs(p, sec + DLC_PATTERN / DLC_SECTOR, p->target[1], DLC_PATTERN / DLC_SECTOR)) return;
    p->target_pic = pic;
    p->target_done = false;
    p->cursor = 0;
    p->pictures++;
}

// ---- the list ------------------------------------------------------------------------------
typedef struct {
    uint8_t *o;
    int pk;                             // packets so far
    int next_addr;                      // the VDP's address after the last data write (-1: not known)
    const uint8_t *level;
} out_t;

static inline void sample(out_t *w, int s) {
    const uint8_t *b = dl_psg[w->level[s]];
    w->o[0] = b[0];
    w->o[1] = b[1];
    w->o[2] = b[2];
    w->o += 3;
}

static void put_simple(out_t *w, uint8_t op) {
    sample(w, 2 * w->pk);
    *w->o++ = op;
    sample(w, 2 * w->pk + 1);
    w->pk++;
}

static void put_reg(out_t *w, uint8_t v, uint8_t r) {
    sample(w, 2 * w->pk);
    *w->o++ = OP_REG;
    *w->o++ = v;
    *w->o++ = (uint8_t)(0x80 | r);
    sample(w, 2 * w->pk + 1);
    w->pk++;
    w->next_addr = -1;
}

static void put_cell(out_t *w, int addr, const uint8_t *d) {
    sample(w, 2 * w->pk);
    if (addr == w->next_addr) {
        *w->o++ = OP_CONT;
    } else {
        *w->o++ = OP_ADDR;
        *w->o++ = (uint8_t)(addr & 0xff);
        *w->o++ = (uint8_t)((addr >> 8) | 0x40);
    }
    memcpy(w->o, d, 3);
    w->o += 3;
    sample(w, 2 * w->pk + 1);
    memcpy(w->o, d + 3, 5);
    w->o += 5;
    w->pk++;
    w->next_addr = addr + 8;
}

static inline bool same8(const uint8_t *a, const uint8_t *b) {
    uint32_t x[2], y[2];
    memcpy(x, a, 8);
    memcpy(y, b, 8);
    return x[0] == y[0] && x[1] == y[1];
}

// the cells that differ, from the cursor on, while there is room
static void chase(dlp_t *p, out_t *w) {
    int n;
    for (n = 0; n < DLP_CELLS; n++) {
        int i = p->cursor;
        for (int t = 0; t < 2; t++) {
            uint8_t *s = p->shadow[t] + i * 8;
            const uint8_t *d = p->target[t] + i * 8;
            if (same8(s, d)) continue;
            if (w->pk >= DLP_VIDEO) return;     // the rest of this cell next field
            put_cell(w, t * 0x2000 + i * 8, d);
            memcpy(s, d, 8);
            p->cells++;
        }
        p->cursor = (uint16_t)(i + 1 == DLP_CELLS ? 0 : i + 1);
    }
    if (!p->target_done) {              // a whole turn with nothing to send
        p->target_done = true;
        if (p->target_pic) p->drawn++;
    }
}

static int finish(dlp_t *p, out_t *w, uint8_t *list) {
    if (p->border != p->border_sent) {
        put_reg(w, p->border, 7);
        p->border_sent = p->border;
    }
    if (p->blank != p->blank_sent) {
        put_reg(w, p->blank ? R1_OFF : R1_ON, 1);
        p->blank_sent = p->blank;
    }
    chase(p, w);
    if (p->target_done && p->want && p->want != p->target_pic && w->pk < DLP_VIDEO) {
        load_picture(p, p->want);       // done early: the next one starts now
        chase(p, w);
    }
    while (w->pk < DLP_VIDEO) put_simple(w, OP_IDLE);
    put_simple(w, OP_KEYS);
    put_simple(w, OP_END);
    p->fields++;
    return (int)(w->o - list);
}

int dlp_field(dlp_t *p, uint16_t inputs, uint8_t *list) {
    dl_machine_t *m = &p->m;
    if (m->inputs != inputs) {
        m->inputs = inputs;
        m->dirty = true;
    }
    uint32_t pic = dl_run_field(m, p->ay, DLC_SAMPLES);

    // the picture: the player's, or behind the blank the one it is looking for
    p->blank = pic == 0;
    uint32_t want = pic;
    if (p->blank) want = m->search_frame != ~0u ? m->search_frame : 0;
    p->want = want;
    if (want && want != p->target_pic && (p->target_done || p->blank || !p->target_pic)) load_picture(p, want);

    // the sound: the field read before this one, as MAME's buffer has it now
    int32_t hunk = m->lasthunk;
    if (hunk >= 0 && (uint32_t)hunk < p->h.hunks && hunk != p->audio_hunk) {
        p->audio_hunk = read_secs(p, p->h.audio_sector + (uint32_t)hunk, p->audio_sec, 1) ? hunk : -1;
    }
    const int8_t *l = (const int8_t *)p->audio_sec, *r = l + DLC_SAMPLES;
    bool have = hunk >= 0 && hunk == p->audio_hunk;
    bool on_l = have && !(m->audiosquelch & 1), on_r = have && !(m->audiosquelch & 2);
    for (int i = 0; i < DLC_SAMPLES; i++) {
        const int v = (on_l ? l[i] : 0) + (on_r ? r[i] : 0) + (p->ay[i] >> 8);   // -384..381
        p->level[i] = (uint8_t)(loud[v + 384] + 128);
    }
    out_t w = {list, 0, -1, p->level};
    return finish(p, &w, list);
}

void dlp_still(dlp_t *p, const uint8_t px[192][256], uint8_t fg) {
    for (int y = 0; y < 192; y++)
        for (int c = 0; c < 32; c++) {
            uint8_t b = 0;
            for (int k = 0; k < 8; k++)
                if (px[y][c * 8 + k] > 1) b |= (uint8_t)(0x80 >> k);
            p->target[0][(y / 8) * 256 + c * 8 + y % 8] = b;
        }
    memset(p->target[1], (uint8_t)(fg << 4 | 1), DLC_PATTERN);
    p->target_pic = 0;
    p->want = 0;
    p->target_done = false;
    p->cursor = 0;
    p->blank = false;
}

int dlp_still_field(dlp_t *p, uint8_t *list) {
    memset(p->level, SILENT, sizeof p->level);
    out_t w = {list, 0, -1, p->level};
    return finish(p, &w, list);
}

// ---- the console's side, for the Mac test ------------------------------------------------------
static int level_of(const uint8_t *b) {
    for (int i = 0; i < 256; i++)
        if (dl_psg[i][0] == b[0] && dl_psg[i][1] == b[1] && dl_psg[i][2] == b[2]) return i;
    return -1;
}

bool dlp_run_list(const uint8_t *list, int n, uint8_t *vram, uint8_t *regs, uint8_t *levels, uint8_t *reports) {
    int i = 0, addr = -1, s = 0;
    (void)reports;
    for (int pk = 0; pk < DLP_PACKETS; pk++) {
        if (i + 7 > n) return false;
        int a = level_of(list + i);
        if (a < 0) return false;
        levels[s++] = (uint8_t)a;
        i += 3;
        uint8_t op = list[i++];
        uint8_t d[8];
        switch (op) {
        case OP_ADDR:
            addr = list[i] | (list[i + 1] & 0x3f) << 8;
            if (!(list[i + 1] & 0x40)) return false;
            i += 2;
            /* fall through */
        case OP_CONT:
            if (addr < 0) return false;
            memcpy(d, list + i, 3);
            i += 3;
            break;
        case OP_REG:
            if ((list[i + 1] & 0xF8) != 0x80) return false;
            regs[list[i + 1] & 7] = list[i];
            i += 2;
            addr = -1;
            break;
        case OP_IDLE: case OP_KEYS: case OP_END:
            break;
        default:
            return false;
        }
        int b = level_of(list + i);
        if (b < 0) return false;
        levels[s++] = (uint8_t)b;
        i += 3;
        if (op == OP_ADDR || op == OP_CONT) {
            memcpy(d + 3, list + i, 5);
            i += 5;
            for (int k = 0; k < 8; k++) vram[(addr + k) & 0x3fff] = d[k];
            addr += 8;
        }
        if ((pk == DLP_PACKETS - 2) != (op == OP_KEYS) || (pk == DLP_PACKETS - 1) != (op == OP_END)) return false;
    }
    return i == n;
}
