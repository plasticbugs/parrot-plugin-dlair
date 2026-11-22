/*
 * dlc_png.h -- an RGB picture as a PNG in memory (stored deflate blocks: no
 * compression, no library), for the converter's previews.
 */
#ifndef DLC_PNG_H
#define DLC_PNG_H

#include <stddef.h>
#include <stdint.h>

/* a malloc'd PNG of the w x h RGB picture (*n bytes), or NULL */
uint8_t *dlc_png(const uint8_t *rgb, int w, int h, size_t *n);

#endif
