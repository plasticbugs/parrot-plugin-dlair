/*
 * dlc_video.h -- a laserdisc picture made into a TMS9918 Mode 2 screen, the
 * way Movies does it (app/engine/src: video.rs, filters.rs, presets.rs):
 *
 *   two YUY2 fields  -> woven 704x480 RGB (dlc_weave)
 *                    -> 256x192, Lanczos-3, then the look's grade (dlc_fit)
 *                    -> 6144 pattern + 6144 colour bytes (dlc_encode)
 *
 * The arithmetic follows the Rust engine's operation for operation, so a
 * picture gives the same bytes (build with FMA contraction off). Every
 * picture is encoded on its own (the looks' temporal stickiness is 0), so
 * the bytes do not depend on the order or the threads.
 */
#ifndef DLC_VIDEO_H
#define DLC_VIDEO_H

#include <stdint.h>

#define DLC_W 256
#define DLC_H 192
#define DLC_SCREEN 6144
/* --size half: the picture at half the width and half the height, in the middle, black around it */
#define DLC_HALF_W 128
#define DLC_HALF_H 96
#define DLC_HALF_X 64
#define DLC_HALF_Y 48

#define DLC_SRC_W 704 /* the active area MAME shows: field lines 22-261, x 8-711 */
#define DLC_SRC_H 480
#define DLC_FIELD_FIRST_LINE 22
#define DLC_FIELD_FIRST_X 8

enum { DLC_BW, DLC_GREY, DLC_COOL, DLC_VIVID, DLC_NLOOKS };

/* "bw", "grey", "cool", "vivid" -> DLC_*, or -1 */
int dlc_look_id(const char *name);
const char *dlc_look_name(int look);
/* 1 when the look's colour table differs from cell to cell; else *colour is every cell's byte */
int dlc_look_has_colour(int look, uint8_t *colour);

/* One field's lines into the woven picture (704x480x3 RGB): the field of an
 * even hunk takes the even rows, of an odd hunk the odd rows; double = 1
 * fills both (a picture of one field). BT.601 studio range, as ld2pico. */
void dlc_weave(const uint8_t *yuy2, uint32_t w, uint32_t h, int odd, int dbl, uint8_t *rgb704);

/* the woven picture to 256x192 RGB with the look's grade */
void dlc_fit(const uint8_t *rgb704, int look, uint8_t *rgb256);
/* change a look's settings for this run: "key=value,..." with amp chroma bias temp vib sat bright gamma
   contrast black; 0, or -1 for a key it doesn't know */
int dlc_tune(int look, const char *spec);
/* as dlc_fit, the picture at DLC_HALF_W x DLC_HALF_H in the middle of a black DLC_W x DLC_H */
void dlc_fit_half(const uint8_t *rgb704, int look, uint8_t *rgb256);
/* the cells around a half-size picture: no pixels set, black on black (col may be a scratch buffer) */
void dlc_blank_outside_half(uint8_t *pat, uint8_t *col);

/* 256x192 RGB to the screen: pat and col are 6144 bytes in VRAM order; shown
 * (optional) gets what the console shows, 256x192 RGB */
void dlc_encode(const uint8_t *rgb256, int look, uint8_t *pat, uint8_t *col, uint8_t *shown);

/* what the console shows for a pattern and colour table */
void dlc_render(const uint8_t *pat, const uint8_t *col, uint8_t *rgb256);

/* VRAM offset of pixel row y, cell column c (either table) */
static inline int dlc_addr(int y, int c)
{
    int r = y / 8, l = y % 8;
    return (r / 8) * 2048 + (r % 8) * 256 + c * 8 + l;
}

#endif
