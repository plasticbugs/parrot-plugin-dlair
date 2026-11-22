// dlmachine.c -- see dlmachine.h. The board, the player and the laserdisc
// follow MAME 0.288 line by line (BSD-3-Clause: Aaron Giles for the dlair
// driver and the laserdisc base, Ryan Holtz for the LD-V1000 HLE); the names
// are MAME's where they can be.
#include "dlmachine.h"

#include <string.h>

// the screen: set_raw(28,636,362 Hz, 910, 0, 704, 525, 44, 524), in MAME's integer attoseconds
#define PIXEL_AS        34920636916LL                   // 10^18 / 28636362
#define SCAN_AS         (PIXEL_AS * 910)
#define FRAME_AS        (SCAN_AS * 525)                 // 16,683,334,286,619,000
#define IRQ_CYCLES      131072                          // 16 MHz / 8 / 65536
#define US(n)           ((dl_time_t){(n) * 4LL, 0})
#define MS(n)           ((dl_time_t){(n) * 4000LL, 0})

// laserdsc.cpp's disc geometry
#define LEAD_IN_TRACKS  900                             // (55000 - 53500) * 1000 / 1666
#define MAX_TOTAL_TRACKS 54000
#define LEAD_OUT_TRACKS 1200                            // 2000 * 1000 / 1666

#define VBI_CODE_LEADIN  0x88ffff
#define VBI_CODE_LEADOUT 0x80eeee

enum {
    CMD_CLEAR = 0xbf, CMD_0 = 0x3f, CMD_1 = 0x0f, CMD_2 = 0x8f, CMD_3 = 0x4f, CMD_4 = 0x2f, CMD_5 = 0xaf,
    CMD_6 = 0x6f, CMD_7 = 0x1f, CMD_8 = 0x9f, CMD_9 = 0x5f, CMD_STORE = 0xf5, CMD_RECALL = 0x7f,
    CMD_DISPLAY = 0xf1, CMD_AUDIO1 = 0xf4, CMD_AUDIO2 = 0xfc, CMD_PLAY = 0xfd, CMD_STOP = 0xfb,
    CMD_AUTOSTOP = 0xf3, CMD_SEARCH = 0xf7, CMD_SCAN_FWD = 0xf0, CMD_SCAN_REV = 0xf8, CMD_STEP_FWD = 0xf6,
    CMD_STEP_REV = 0xfe, CMD_REJECT = 0xf9, CMD_NO_ENTRY = 0xff, CMD_LOAD = 0xcc,
    CMD_DISPLAY_DISABLE = 0xcd, CMD_DISPLAY_ENABLE = 0xce, CMD_GET_FRAME_NUM = 0xc2,
    CMD_GET_2ND_DISPLAY = 0xc3, CMD_GET_1ST_DISPLAY = 0xc4, CMD_TRANSFER_MEMORY = 0xc8,
    CMD_FWD_X0 = 0xa0, CMD_FWD_X1_4 = 0xa1, CMD_FWD_X1_2 = 0xa2, CMD_FWD_X1 = 0xa3, CMD_FWD_X2 = 0xa4,
    CMD_FWD_X3 = 0xa5, CMD_FWD_X4 = 0xa6, CMD_FWD_X5 = 0xa7, CMD_SKIP_FWD_10 = 0xb1, CMD_SKIP_FWD_100 = 0xba,
};
enum {
    STATUS_PARK = 0x7c, STATUS_PLAY = 0x64, STATUS_STOP = 0x65, STATUS_SEARCH = 0x50,
    STATUS_SEARCH_FINISH = 0xd0, STATUS_AUTOSTOP = 0x54, STATUS_SCAN = 0x4c, STATUS_FORWARD = 0x2e,
    STATUS_READY = 0x80,
};

static const uint8_t empty_page[256];

// ---- time ---------------------------------------------------------------------------
static inline dl_time_t t_add(dl_time_t t, dl_time_t d) {
    t.c += d.c;
    t.a += d.a;
    if (t.a >= DL_APC) {
        t.a -= DL_APC;
        t.c++;
    }
    return t;
}
static inline dl_time_t t_as(int64_t as) { return (dl_time_t){as / DL_APC, as % DL_APC}; }
static inline bool t_before(dl_time_t x, dl_time_t y) { return x.c < y.c || (x.c == y.c && x.a < y.a); }
static inline int64_t t_diff(dl_time_t b, dl_time_t a) { return (b.c - a.c) * DL_APC + (b.a - a.a); }   // under 9 s apart
static inline dl_time_t t_cyc(int64_t c) { return (dl_time_t){c, 0}; }

dl_time_t dl_vblank_time(uint64_t k) {
    const int64_t whole = FRAME_AS / DL_APC, part = FRAME_AS % DL_APC;
    int64_t pa = (int64_t)k * part;
    return (dl_time_t){(int64_t)k * whole + pa / DL_APC, pa % DL_APC};
}

void dl_time_str(dl_time_t t, char *buf, int len) {
    int64_t s = t.c / DL_CYCLES_S, as = (t.c % DL_CYCLES_S) * DL_APC + t.a;
    // (snprintf of %lld is avoided: newlib-nano lacks it)
    char tmp[40];
    int n = 0;
    uint64_t v = (uint64_t)as;
    for (int i = 0; i < 18; i++, v /= 10) tmp[n++] = (char)('0' + v % 10);
    tmp[n++] = '.';
    v = (uint64_t)s;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    int o = 0;
    while (n && o < len - 1) buf[o++] = tmp[--n];
    buf[o] = 0;
}

static inline int64_t consumed(const dl_machine_t *m) { return m->given - m->cpu.icount; }

// ---- timers ---------------------------------------------------------------------------
static void arm(dl_machine_t *m, int i, dl_time_t now, dl_time_t d, int32_t param) {
    dl_timer_t *t = &m->t[i];
    t->at = t_add(now, d);
    t->period = (dl_time_t){0, 0};
    t->param = param;
    t->seq = ++m->seq;
    t->armed = true;
}
static void arm_periodic(dl_machine_t *m, int i, dl_time_t now, dl_time_t d) {
    arm(m, i, now, d, 0);
    m->t[i].period = d;
}
static void disarm(dl_machine_t *m, int i) { m->t[i].armed = false; }

// the first timer due by cycle c (-1: none)
static int due(const dl_machine_t *m, int64_t c) {
    int best = -1;
    for (int i = 0; i < DLT_COUNT; i++) {
        const dl_timer_t *t = &m->t[i];
        if (!t->armed || t->at.c > c) continue;
        if (best < 0 || t_before(t->at, m->t[best].at) ||
            (t->at.c == m->t[best].at.c && t->at.a == m->t[best].at.a && t->seq < m->t[best].seq))
            best = i;
    }
    return best;
}

static int64_t next_event(const dl_machine_t *m) {
    int64_t c = INT64_MAX;
    for (int i = 0; i < DLT_COUNT; i++)
        if (m->t[i].armed && m->t[i].at.c < c) c = m->t[i].at.c;
    return c;
}

// ---- the laserdisc base -----------------------------------------------------------------
static void add_and_clamp_track(dl_machine_t *m, int32_t delta) {
    m->curtrack += delta;
    if (m->curtrack < 1) m->curtrack = 1;
    if (m->curtrack > m->maxtrack - 1) m->curtrack = m->maxtrack - 1;
}

static void update_slider_pos(dl_machine_t *m, dl_time_t now) {
    if (m->attospertrack == 0) {
        m->sliderupdate = now;
        return;
    }
    int64_t delta = t_diff(now, m->sliderupdate);
    if (m->attospertrack >= 0) {
        int32_t covered = (int32_t)(delta / m->attospertrack);
        add_and_clamp_track(m, covered);
        if (covered) m->sliderupdate = t_add(m->sliderupdate, t_as(covered * m->attospertrack));
    } else {
        int32_t covered = (int32_t)(delta / -m->attospertrack);
        add_and_clamp_track(m, -covered);
        if (covered) m->sliderupdate = t_add(m->sliderupdate, t_as(covered * -m->attospertrack));
    }
}

static void set_slider_speed(dl_machine_t *m, dl_time_t now, double tracks_per_vsync) {
    update_slider_pos(m, now);
    double vsyncperiod = (double)FRAME_AS * 1e-18;
    if (tracks_per_vsync == 0)
        m->attospertrack = 0;
    else if (tracks_per_vsync > 0)
        m->attospertrack = (int64_t)(vsyncperiod / tracks_per_vsync * 1e18);
    else
        m->attospertrack = (int64_t)(-vsyncperiod / -tracks_per_vsync * 1e18);
}

static void advance_slider(dl_machine_t *m, dl_time_t now, int32_t numtracks) {
    update_slider_pos(m, now);
    add_and_clamp_track(m, numtracks);
}

static void read_track_data(dl_machine_t *m) {
    int32_t chdtrack = m->curtrack - 1 - LEAD_IN_TRACKS;
    if (chdtrack < 0) chdtrack = 0;
    if (chdtrack > m->chdtracks - 1) chdtrack = m->chdtracks - 1;
    int32_t readhunk = chdtrack * 2 + m->fieldnum;
    uint32_t line = m->vbi && (uint32_t)readhunk < m->hunks ? m->vbi[readhunk] & 0xffffff : 0;
    if ((uint32_t)(m->curtrack - 1) < LEAD_IN_TRACKS) line = VBI_CODE_LEADIN;

    dl_frame_t *frame = &m->frame[m->videoindex];
    if ((line & 0xf00000) == 0xf00000) {
        if (frame->numfields >= 2) m->videoindex = (uint8_t)((m->videoindex + 1) % 3);
        frame = &m->frame[m->videoindex];
        frame->numfields = 0;
        frame->code = line;
    }
    if (m->videosquelch) frame->numfields = 0;
    m->metadata[m->fieldnum] = line;
    m->readok = !m->videosquelch;
    m->readhunk = readhunk;
}

static void process_track_data(dl_machine_t *m) {
    if (m->readok) m->frame[m->videoindex].numfields++;
    m->lasthunk = m->readok ? m->readhunk : -1;     // its audio is in the buffer from now
}

// ---- the player (ldv1000hle) ----------------------------------------------------------------
static uint32_t bcd_to_literal(uint32_t bcd) {
    uint32_t value = 0, shift = 28, multiplier = 10000000;
    for (int i = 0; i < 8; i++) {
        uint32_t digit = (bcd >> shift) & 0xf;
        value += digit * multiplier;
        multiplier /= 10;
        shift -= 4;
    }
    return value;
}

static uint32_t literal_to_bcd(uint32_t value) {
    uint32_t bcd = 0, shift = 28, multiplier = 10000000;
    for (int i = 0; i < 8; i++) {
        uint32_t digit = value / multiplier;
        bcd |= digit << shift;
        value -= digit * multiplier;
        multiplier /= 10;
        shift -= 4;
    }
    return bcd;
}

static uint32_t cmd_number_to_bcd(const dl_machine_t *m) {
    uint32_t bcd = 0, shift = 0;
    for (int i = (int)m->cmd_number_length - 1; i >= 0; i--) {
        bcd |= (uint32_t)m->cmd_number[i] << shift;
        shift += 4;
    }
    return bcd;
}

static int cmd_to_number(uint8_t cmd) {
    static const uint8_t codes[10] = {CMD_0, CMD_1, CMD_2, CMD_3, CMD_4, CMD_5, CMD_6, CMD_7, CMD_8, CMD_9};
    for (int i = 0; i < 10; i++)
        if (codes[i] == cmd) return i;
    return -1;
}

static bool is_command_byte(uint8_t d) {
    if (cmd_to_number(d) >= 0) return true;
    switch (d) {
    case CMD_CLEAR: case CMD_STORE: case CMD_RECALL: case CMD_DISPLAY: case CMD_AUDIO1: case CMD_AUDIO2:
    case CMD_PLAY: case CMD_STOP: case CMD_AUTOSTOP: case CMD_SEARCH: case CMD_SCAN_FWD: case CMD_SCAN_REV:
    case CMD_STEP_FWD: case CMD_STEP_REV: case CMD_REJECT: case CMD_NO_ENTRY: case CMD_LOAD:
    case CMD_DISPLAY_DISABLE: case CMD_DISPLAY_ENABLE: case CMD_GET_FRAME_NUM: case CMD_GET_2ND_DISPLAY:
    case CMD_GET_1ST_DISPLAY: case CMD_TRANSFER_MEMORY:
        return true;
    }
    return (d >= CMD_FWD_X0 && d <= CMD_FWD_X5) || (d >= CMD_SKIP_FWD_10 && d <= CMD_SKIP_FWD_100);
}

static void set_mode(dl_machine_t *m, dl_time_t now, uint8_t mode) {
    if (m->mode == mode) return;
    m->mode = mode;
    if (mode != DLM_PARK)
        disarm(m, DLT_PARK);
    else
        arm_periodic(m, DLT_PARK, now, MS(21));
}

static void set_playing(dl_machine_t *m, dl_time_t now, uint8_t status, double fields_per_vsync) {
    set_mode(m, now, DLM_PLAY);
    m->status = status;
    m->play_speed = fields_per_vsync;
    m->search_frame = ~0u;
    m->stop_frame = ~0u;
}

static void set_stopped(dl_machine_t *m, dl_time_t now, uint8_t status) {
    set_mode(m, now, DLM_STOP);
    m->status = status;
}

static void cmd_search(dl_machine_t *m, dl_time_t now, uint32_t frame) {
    set_mode(m, now, DLM_SEARCH);
    m->status = STATUS_SEARCH;
    m->search_frame = frame;
    m->stop_frame = ~0u;
}

static void cmd_play_forward(dl_machine_t *m, dl_time_t now, double fields_per_vsync) {
    set_slider_speed(m, now, fields_per_vsync);
    set_playing(m, now, STATUS_FORWARD, fields_per_vsync);
}

static void process_command(dl_machine_t *m, dl_time_t now, uint8_t command) {
    if (!(m->status & STATUS_READY)) {
        if (command == CMD_NO_ENTRY)
            m->status |= STATUS_READY;
        else
            m->status &= (uint8_t)~STATUS_READY;
        return;
    }
    m->status &= (uint8_t)~STATUS_READY;
    int digit = cmd_to_number(command);
    if (digit >= 0) {
        if (m->cmd_number_length < sizeof m->cmd_number) m->cmd_number[m->cmd_number_length] = (uint8_t)digit;
        m->cmd_number_length++;
        return;                         // a number keeps the number
    }
    switch (command) {
    case CMD_STORE:
        m->user_ram[m->curr_register] = (uint16_t)(m->cmd_number_length == 0 ? literal_to_bcd(m->curr_frame) : cmd_number_to_bcd(m));
        break;
    case CMD_AUDIO1:
    case CMD_AUDIO2: {
        bool *e = &m->audio_enable[command == CMD_AUDIO2];
        *e = m->cmd_number_length == 0 ? !*e : (cmd_number_to_bcd(m) & 1);
        break;
    }
    case CMD_PLAY:
        set_slider_speed(m, now, 0);
        set_playing(m, now, STATUS_PLAY, 1.0);
        break;
    case CMD_STOP:
        if (m->cmd_number_length > 0) {
            arm(m, DLT_STOP, now, MS(100 * (int64_t)cmd_number_to_bcd(m)), 0);
            m->pre_stop_status = m->status;
        }
        set_stopped(m, now, STATUS_STOP);
        break;
    case CMD_AUTOSTOP: {
        uint32_t value = bcd_to_literal(cmd_number_to_bcd(m));
        if (value < m->curr_frame) {
            cmd_search(m, now, value);
            break;
        }
        m->status = STATUS_AUTOSTOP;
        break;
    }
    case CMD_SEARCH:
        cmd_search(m, now, bcd_to_literal(cmd_number_to_bcd(m)));
        break;
    case CMD_SCAN_FWD:
    case CMD_SCAN_REV:
        set_mode(m, now, command == CMD_SCAN_FWD ? DLM_SCAN_FORWARD : DLM_SCAN_REVERSE);
        m->status = STATUS_SCAN;
        m->scan_speed = 4000;
        m->scan_speed_accum = 0;
        m->stop_frame = ~0u;
        m->search_frame = ~0u;
        break;
    case CMD_STEP_FWD:
    case CMD_STEP_REV:
        advance_slider(m, now, command == CMD_STEP_FWD ? 1 : -1);
        set_stopped(m, now, STATUS_STOP);
        m->stop_frame = ~0u;
        m->search_frame = ~0u;
        break;
    case CMD_REJECT:
        set_mode(m, now, DLM_PARK);
        m->status = STATUS_PARK;
        m->stop_frame = ~0u;
        m->search_frame = ~0u;
        break;
    case CMD_NO_ENTRY:
        m->status |= STATUS_READY;
        break;
    case CMD_FWD_X0: cmd_play_forward(m, now, 0.0); break;
    case CMD_FWD_X1_4: cmd_play_forward(m, now, 0.25); break;
    case CMD_FWD_X1_2: cmd_play_forward(m, now, 0.5); break;
    case CMD_FWD_X1: cmd_play_forward(m, now, 1.0); break;
    case CMD_FWD_X2: cmd_play_forward(m, now, 2.0); break;
    case CMD_FWD_X3: cmd_play_forward(m, now, 3.0); break;
    case CMD_FWD_X4: cmd_play_forward(m, now, 4.0); break;
    case CMD_FWD_X5: cmd_play_forward(m, now, 5.0); break;
    default:
        if (command >= CMD_SKIP_FWD_10 && command <= CMD_SKIP_FWD_100) {
            int32_t amount = 10 * (command - CMD_SKIP_FWD_10 + 1);
            set_mode(m, now, DLM_SEARCH);
            m->search_frame = ~0u;
            m->stop_frame = m->curr_frame + (uint32_t)amount;
            m->scan_speed = amount;
        }
        break;                          // CLEAR, RECALL, DISPLAY, LOAD, ... do nothing
    }
    m->cmd_number_length = 0;
}

static void data_w(dl_machine_t *m, dl_time_t now, uint8_t data) {
    if (m->cmd_length < sizeof m->cmd_buffer) m->cmd_buffer[m->cmd_length++] = data;
    if (!is_command_byte(data)) return;
    for (int i = 0; i < m->cmd_length; i++) process_command(m, now, m->cmd_buffer[i]);
    m->cmd_length = 0;
}

static void update_audio_enable(dl_machine_t *m) {
    if (m->mode == DLM_PLAY && m->play_speed == 1.0)
        m->audiosquelch = (uint8_t)((!m->audio_enable[0] ? 1 : 0) | (!m->audio_enable[1] ? 2 : 0));
    else
        m->audiosquelch = 3;
}

static int32_t player_update(dl_machine_t *m, dl_time_t now) {
    if (m->mode != DLM_PARK) arm(m, DLT_ST_ON, now, US(500), 0);
    if (!m->fieldnum) return 0;
    if (m->mode == DLM_SCAN_FORWARD || m->mode == DLM_SCAN_REVERSE) {
        m->scan_speed_accum += m->scan_speed;
        int elapsed = m->scan_speed_accum / 60;
        m->scan_speed_accum -= elapsed * 60;
        if (m->mode == DLM_SCAN_REVERSE) elapsed = -elapsed;
        if (m->stop_frame != ~0u) {
            int next_frame = (int)m->curr_frame + elapsed;
            if ((uint32_t)next_frame >= m->stop_frame) {     // (MAME compares an int with a u32)
                elapsed = (int)m->stop_frame - (int)m->curr_frame;
                m->stop_frame = ~0u;
                set_playing(m, now, STATUS_FORWARD, m->play_speed);
            }
        }
        return elapsed;
    }
    return m->mode == DLM_PLAY || m->mode == DLM_SEARCH;
}

static void process_vbi_data(dl_machine_t *m, dl_time_t now, int32_t param) {
    if (param == 0) {
        m->videoenable = m->mode == DLM_PLAY || m->mode == DLM_STOP;
        update_audio_enable(m);
    }
    uint32_t line = m->metadata[m->fieldnum];
    if ((line & 0xf00000) != 0xf00000 && line != VBI_CODE_LEADIN && line != VBI_CODE_LEADOUT) return;
    uint32_t old_frame = m->curr_frame;
    m->curr_frame = line == VBI_CODE_LEADIN ? 0 : line == VBI_CODE_LEADOUT ? 54000 : bcd_to_literal(line & 0x7ffff);
    if (m->mode == DLM_STOP) return;
    if (m->stop_frame != ~0u && m->search_frame == ~0u) {
        int32_t old_delta = (int32_t)m->stop_frame - (int32_t)old_frame;
        int32_t curr_delta = (int32_t)m->stop_frame - (int32_t)m->curr_frame;
        if (curr_delta == 0 || (old_delta < 0) != (curr_delta < 0)) {
            m->stop_frame = ~0u;
            set_stopped(m, now, (uint8_t)((m->status & STATUS_READY) | STATUS_STOP));
        }
    }
    if (m->search_frame != ~0u) {
        int32_t delta = (int32_t)m->search_frame - (int32_t)m->curr_frame;
        if (delta == 0) {
            m->search_frame = ~0u;
            set_stopped(m, now, STATUS_SEARCH_FINISH);
        } else if (delta <= 2 && delta > 0) {
            // close: let the disc run up to it
        } else if (delta < 0) {
            advance_slider(m, now, delta / 2 < -2 ? delta / 2 : -2);
        } else {
            advance_slider(m, now, delta / 2 > 1 ? delta / 2 : 1);
        }
    }
}

// ---- the timers' work ----------------------------------------------------------------------
static void log_f(dl_machine_t *m, dl_time_t now);

static void fire(dl_machine_t *m, int i) {
    dl_timer_t *t = &m->t[i];
    dl_time_t now = t->at;
    int32_t param = t->param;
    if (t->period.c || t->period.a) {
        t->at = t_add(t->at, t->period);
        t->seq = ++m->seq;
    } else {
        t->armed = false;
    }
    m->events++;
    if (m->log) m->log(m->log_ctx, 'T', now, (uint32_t)i, (uint32_t)consumed(m));
    switch (i) {
    case DLT_VBLANK:                    // screen vblank_begin: the frame is done, then laserdisc vblank_state_changed
        m->fields++;
        if (m->log) log_f(m, now);
        update_slider_pos(m, now);
        if (m->mode != DLM_PARK) arm(m, DLT_VBI, now, t_as(SCAN_AS * 39), m->fieldnum);   // time_until_pos(19*2)
        arm(m, DLT_FETCH, now, t_as(SCAN_AS * 33), 0);                                    // time_until_pos(16*2)
        break;
    case DLT_FETCH:
        process_track_data(m);
        update_slider_pos(m, now);
        add_and_clamp_track(m, player_update(m, now));
        m->fieldnum ^= 1;
        read_track_data(m);
        break;
    case DLT_VBI:
        process_vbi_data(m, now, param);
        break;
    case DLT_ST_ON:
    case DLT_PARK:
        m->status_strobe = false;
        arm(m, DLT_ST_OFF, now, US(26), 0);
        arm(m, DLT_CMD_ON, now, US(54), 0);
        break;
    case DLT_ST_OFF:
        m->status_strobe = true;
        break;
    case DLT_CMD_ON:
        m->command_strobe = false;
        arm(m, DLT_CMD_OFF, now, US(25), 0);
        break;
    case DLT_CMD_OFF:
        m->command_strobe = true;
        break;
    case DLT_STOP:
        set_playing(m, now, m->pre_stop_status, m->play_speed);
        break;
    }
}

// every timer that a CPU access at cycle c sees
static void fire_until(dl_machine_t *m, int64_t c) {
    int i;
    while ((i = due(m, c)) >= 0) fire(m, i);
}

static void log_f(dl_machine_t *m, dl_time_t now) {
    m->log(m->log_ctx, 'F', now, (uint32_t)m->fields, 0);
}

// ---- the board ----------------------------------------------------------------------------
static void ay_catch_up(dl_machine_t *m, int64_t c) {
    if (!m->audio || m->audio_len <= 0) return;
    int64_t pos = c - m->audio_c0;
    if (pos > m->audio_len) pos = m->audio_len;
    if (pos < 0) pos = 0;
    int due_n = (int)(pos * m->audio_n / m->audio_len);
    if (due_n > m->audio_done) {
        ay_render(&m->ay, m->audio + m->audio_done, due_n - m->audio_done, m->ay_level);
        m->audio_done = due_n;
    }
}

// the CPU's state but R, for the idle skip
static void snapshot(const z80_t *cpu, uint8_t *s) {
    uint16_t w[14] = {cpu->pc.w, cpu->sp.w, cpu->af.w, cpu->bc.w, cpu->de.w, cpu->hl.w, cpu->ix.w,
                      cpu->iy.w, cpu->wz.w, cpu->af2.w, cpu->bc2.w, cpu->de2.w, cpu->hl2.w, cpu->prvpc.w};
    memcpy(s, w, sizeof w);
    uint8_t *b = s + sizeof w;
    b[0] = cpu->f.s_val;
    b[1] = cpu->f.z_val;
    b[2] = cpu->f.yx_val;
    b[3] = cpu->f.h_val;
    b[4] = cpu->f.pv_val;
    b[5] = (uint8_t)((cpu->f.n ? 1 : 0) | (cpu->f.c ? 2 : 0));
    b[6] = cpu->f.q;
    b[7] = (uint8_t)(cpu->iff1 | (cpu->iff2 << 1) | (cpu->halt << 2) | (cpu->im << 3) | (cpu->irq_state << 5));
    b[8] = cpu->i;
    b[9] = (uint8_t)(cpu->r2 & 0x80);
    b[10] = cpu->service_attention;
    b[11] = cpu->f.qtemp;
}

// A read of a port from the same instruction as the last port read, with
// nothing changed since but the clock and R: the CPU is going round a loop
// whose every turn will be the same until the next event. Turns are stepped
// over until the last one whose read comes before it.
static int64_t idle_check(dl_machine_t *m, int64_t c) {
    z80_t *cpu = &m->cpu;
    uint8_t s[sizeof m->idle_state];
    snapshot(cpu, s);
    if (!m->dirty && m->events == m->idle_events && cpu->prvpc.w == m->idle_pc && c > m->idle_c &&
        !memcmp(s, m->idle_state, sizeof s)) {
        int64_t turn = c - m->idle_c;
        int64_t limit = next_event(m);
        if (m->irq_next < limit) limit = m->irq_next;
        if (m->run_stop < limit) limit = m->run_stop;
        if (limit - 1 - c >= turn) {
            int64_t k = (limit - 1 - c) / turn;
            uint8_t dr = (uint8_t)((cpu->r - m->idle_r) & 0x7f);
            cpu->icount -= (int32_t)(k * turn);
            cpu->r = (uint8_t)(cpu->r + k * dr);
            m->skipped += (uint64_t)(k * turn);
            c += k * turn;
        }
    }
    m->idle_pc = cpu->prvpc.w;
    memcpy(m->idle_state, s, sizeof s);
    m->idle_r = cpu->r;
    m->idle_c = c;
    m->idle_events = m->events;
    m->dirty = false;
    return c;
}

static uint8_t mem_read(z80_t *cpu, uint16_t a) {
    dl_machine_t *m = (dl_machine_t *)cpu->user;
    if (a < 0xc000 || a >= 0xe000) return 0;         // nothing there (MAME's unmapped value)
    int64_t c = consumed(m);
    fire_until(m, c);
    if (m->skip) c = idle_check(m, c);
    switch (a & 0x38) {
    case 0x00:                          // the AY's data; 14 and 15 are the DIP switches
        return m->ay_active ? ay_read(&m->ay, m->ay_latch) : 0xff;
    case 0x08:                          // P1, active low
        return (uint8_t)((~m->inputs & 0x1f) | 0xe0);
    case 0x10:                          // SYSTEM: start and coin active low, the strobes
        return (uint8_t)((~(m->inputs >> 8) & 0x0f) | 0x30 | (m->status_strobe ? 0x40 : 0) | (m->command_strobe ? 0 : 0x80));
    case 0x20:                          // the player's status
        if (m->log) m->log(m->log_ctx, 'R', t_cyc(c), m->status, 0);
        return m->status;
    }
    return 0;
}

static void mem_write(z80_t *cpu, uint16_t a, uint8_t v) {
    dl_machine_t *m = (dl_machine_t *)cpu->user;
    if (a >= 0xa000 && a < 0xc000) {   // RAM, mirrored (here, not through the page table, so a change is seen)
        uint8_t *p = &m->ram[a & 0x7ff];
        if (*p != v) {
            *p = v;
            m->dirty = true;
        }
        return;
    }
    if (a < 0xe000) return;
    int64_t c = consumed(m);
    fire_until(m, c);
    m->dirty = true;
    dl_time_t now = t_cyc(c);
    switch (a & 0x38) {
    case 0x00:
        if (!m->ay_active) break;
        ay_catch_up(m, c);
        ay_write(&m->ay, m->ay_latch, v);
        break;
    case 0x08: {                        // misc: bit 5 falling sends the latch to the player
        if (m->log) m->log(m->log_ctx, 'W', now, 0x08, v);
        uint8_t diff = v ^ m->last_misc;
        m->last_misc = v;
        if ((diff & 0x20) && !(v & 0x20)) data_w(m, now, m->ld_latch);
        break;
    }
    case 0x10:
        m->ay_active = (v >> 4) == 0;
        if (m->ay_active) m->ay_latch = v & 0x0f;
        break;
    case 0x20:
        if (m->log) m->log(m->log_ctx, 'W', now, 0x20, v);
        m->ld_latch = v;
        break;
    case 0x30:                          // the scoreboard: e030-e037 digits 8-15, e038-e03f 0-7
    case 0x38: {
        int d = ((a & 0x38) == 0x38 ? 0 : 8) + (a & 7);
        if (m->leds[d] != (v & 0x0f)) {
            m->leds[d] = v & 0x0f;
            m->led_serial++;
        }
        break;
    }
    }
}

static uint8_t io_read(z80_t *cpu, uint16_t port) {
    (void)cpu;
    (void)port;
    return 0xff;
}

static void io_write(z80_t *cpu, uint16_t port, uint8_t v) {
    (void)cpu;
    (void)port;
    (void)v;
}

void dl_init(dl_machine_t *m, const uint8_t *rom, const uint32_t *vbi, uint32_t hunks) {
    dl_log_fn log = m->log;
    void *ctx = m->log_ctx;
    bool skip = m->skip;
    memset(m, 0, sizeof *m);
    m->log = log;
    m->log_ctx = ctx;
    m->skip = skip;
    m->rom = rom;
    m->vbi = vbi;
    m->hunks = hunks;
    m->dsw1 = DL_DSW1_DEFAULT;
    m->dsw2 = DL_DSW2_DEFAULT;
    m->ay_level = 6000;
    ay_init(&m->ay, 2000000, 11628);
    z80_t *cpu = &m->cpu;
    z80_init(cpu);
    cpu->user = m;
    cpu->read = mem_read;
    cpu->write = mem_write;
    cpu->in = io_read;
    cpu->out = io_write;
    for (int p = 0; p < 0x80; p++) cpu->rd_page[p] = rom + (p << 8);
    for (int p = 0x80; p < 0xa0; p++) cpu->rd_page[p] = empty_page;
    for (int p = 0xa0; p < 0xc0; p++) cpu->rd_page[p] = m->ram + ((p & 7) << 8);
    dl_reset(m);
}

void dl_reset(dl_machine_t *m) {
    z80_reset(&m->cpu);
    m->cpu.icount = 0;
    m->given = 0;
    memset(m->ram, 0, sizeof m->ram);
    ay_reset(&m->ay);
    m->ay.port_in[0] = m->dsw1;
    m->ay.port_in[1] = m->dsw2;
    m->ay_latch = 0;
    m->ay_active = false;
    m->last_misc = 0;
    m->ld_latch = 0;
    memset(m->leds, 0x0f, sizeof m->leds);
    memset(m->t, 0, sizeof m->t);
    m->seq = 0;
    m->fields = 0;
    dl_time_t zero = {0, 0};
    m->irq_next = IRQ_CYCLES;
    arm_periodic(m, DLT_VBLANK, zero, t_as(FRAME_AS));

    // laserdisc_device::device_reset, then the player's
    m->audiosquelch = 3;
    m->videosquelch = false;
    m->videoenable = true;
    m->fieldnum = 0;
    m->curtrack = 1;
    m->attospertrack = 0;
    m->sliderupdate = zero;
    m->chdtracks = (int32_t)(m->hunks / 2);
    m->maxtrack = LEAD_IN_TRACKS + MAX_TOTAL_TRACKS + LEAD_OUT_TRACKS;
    if (m->maxtrack < LEAD_IN_TRACKS + LEAD_OUT_TRACKS + m->chdtracks) m->maxtrack = LEAD_IN_TRACKS + LEAD_OUT_TRACKS + m->chdtracks;
    memset(m->frame, 0, sizeof m->frame);
    m->videoindex = 0;
    m->metadata[0] = m->metadata[1] = 0;
    m->readok = false;
    m->readhunk = m->lasthunk = -1;

    m->cmd_length = 0;
    m->status = STATUS_PARK | STATUS_READY;
    m->pre_stop_status = 0;
    m->mode = DLM_PARK;
    m->curr_frame = 0;
    m->search_frame = ~0u;
    m->stop_frame = ~0u;
    m->cmd_number_length = 0;
    m->curr_register = 0;
    m->scan_speed = 0;
    m->scan_speed_accum = 0;
    m->play_speed = 0.0;
    m->audio_enable[0] = m->audio_enable[1] = true;
    m->status_strobe = true;
    m->command_strobe = true;
    arm_periodic(m, DLT_PARK, zero, MS(21));
    m->dirty = true;
}

// the CPU to cycle stop (or the instruction boundary just past it), with the periodic IRQ
static void run_cpu_to(dl_machine_t *m, int64_t stop) {
    while (consumed(m) < stop) {
        int64_t s = stop < m->irq_next ? stop : m->irq_next, c = consumed(m);
        if (c < s) {
            m->run_stop = s;
            int32_t n = (int32_t)(s - c);
            m->given += n;
            z80_run(&m->cpu, n);
        }
        if (consumed(m) >= m->irq_next) {
            z80_hold_irq(&m->cpu, 0xff);    // irq0_line_hold
            m->irq_next += IRQ_CYCLES;
            m->dirty = true;
        }
    }
}

void dl_run_to_vblank(dl_machine_t *m) {
    int64_t e = m->t[DLT_VBLANK].at.c;
    uint64_t f = m->fields;
    run_cpu_to(m, e);
    // everything up to and including the vblank (an access in the
    // instruction that crossed it may have fired it already)
    while (m->fields == f) fire(m, due(m, e));
}

uint32_t dl_picture(const dl_machine_t *m) {
    if (!m->videoenable) return 0;
    const dl_frame_t *f = &m->frame[m->videoindex];
    if (f->numfields < 2) f = &m->frame[(m->videoindex + 2) % 3];
    if ((f->code & 0xf00000) != 0xf00000) return 0;
    return bcd_to_literal(f->code & 0x7ffff);
}

uint32_t dl_run_field(dl_machine_t *m, int16_t *ay, int n) {
    int64_t c0 = consumed(m), e = m->t[DLT_VBLANK].at.c;
    m->audio = ay;
    m->audio_n = n;
    m->audio_done = 0;
    m->audio_c0 = c0;
    m->audio_len = e > c0 ? e - c0 : 1;
    if (ay) memset(ay, 0, sizeof *ay * (size_t)n);
    dl_run_to_vblank(m);
    if (ay && m->audio_done < n) ay_render(&m->ay, ay + m->audio_done, n - m->audio_done, m->ay_level);
    m->audio = NULL;
    return dl_picture(m);
}
