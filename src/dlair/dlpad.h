// dlpad.h -- the controls, as the board's inputs (dlmachine.h's DL_IN_*).
// Portable, header-only.
//
//   the stick   the controller's joystick; a USB pad's d-pad or left stick
//   the sword   either fire button; a pad's face buttons, L, R, ZL, ZR
//   coin        keypad * or #; a pad's - (one coin a press)
//   start       keypad 1 (one player), 2 (two players); a pad's + (one)
// (* or # for a coin and 1 and 2 for the starts are the arcade plugins' keys)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../sdk/gamepad.h"
#include "dlmachine.h"

// the ColecoVision keypad's codes, as the kernel reports them
enum { DLKEY_1 = 0xD, DLKEY_2 = 0x7, DLKEY_STAR = 0x9, DLKEY_HASH = 0x6, DLKEY_NONE = 0xF };

// A coin is a pulse of DLPAD_COIN fields from the press, however long it is
// held (a coin switch held shut looks jammed to a coin routine); another
// needs the key let go for DLPAD_COIN_UP fields first.
#define DLPAD_COIN      6
#define DLPAD_COIN_UP   3
#define DLPAD_STICK     1024            // a stick this far from the middle (of 2047) is a direction

typedef struct {
    uint8_t coin_left, coin_wait;
} dlpad_t;

// joy, fire, key: the controller as cvf_input() gives it (low = pressed);
// pad: a USB pad, or NULL
static inline uint16_t dlpad_field(dlpad_t *s, uint8_t joy, uint8_t fire, uint8_t key, const gamepad_t *pad) {
    uint16_t in = 0;
    if (!(joy & 1)) in |= DL_IN_UP;
    if (!(joy & 2)) in |= DL_IN_RIGHT;
    if (!(joy & 4)) in |= DL_IN_DOWN;
    if (!(joy & 8)) in |= DL_IN_LEFT;
    if (!(fire & 1) || !(fire & 2)) in |= DL_IN_SWORD;
    bool coin = key == DLKEY_STAR || key == DLKEY_HASH;
    if (key == DLKEY_1) in |= DL_IN_START1;
    if (key == DLKEY_2) in |= DL_IN_START2;
    if (pad) {
        uint32_t b = pad->buttons;
        if ((b & GP_UP) || pad->ly > DLPAD_STICK) in |= DL_IN_UP;
        if ((b & GP_DOWN) || pad->ly < -DLPAD_STICK) in |= DL_IN_DOWN;
        if ((b & GP_LEFT) || pad->lx < -DLPAD_STICK) in |= DL_IN_LEFT;
        if ((b & GP_RIGHT) || pad->lx > DLPAD_STICK) in |= DL_IN_RIGHT;
        if (b & (GP_SOUTH | GP_EAST | GP_WEST | GP_NORTH | GP_L | GP_R | GP_ZL | GP_ZR)) in |= DL_IN_SWORD;
        if (b & GP_MINUS) coin = true;
        if (b & GP_PLUS) in |= DL_IN_START1;
    }
    if (coin) {
        if (!s->coin_left && !s->coin_wait) s->coin_left = DLPAD_COIN;
        s->coin_wait = DLPAD_COIN_UP;
    } else if (s->coin_wait) {
        s->coin_wait--;
    }
    if (s->coin_left) {
        s->coin_left--;
        in |= DL_IN_COIN1;
    }
    return in;
}
