/*
 * dlc_card.c -- see dlc_card.h.
 */
#include "dlc_card.h"

#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(dlc_header_t, hunks) == 40 && offsetof(dlc_header_t, resume) == 112,
               "the packing below is the struct's own layout");

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void dlc_header_layout(dlc_header_t *h, uint32_t hunks, uint32_t first_picture, uint32_t pictures, int look,
                       int has_colour, uint8_t colour, const uint8_t sha1[20])
{
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, DLC_MAGIC, 8);
    h->version = DLC_VERSION;
    h->header_bytes = sizeof(dlc_header_t);
    memcpy(h->sha1, sha1, 20);
    h->look = (uint8_t)look;
    h->has_colour = (uint8_t)(has_colour != 0);
    h->colour = has_colour ? 0 : colour;
    h->hunks = hunks;
    h->first_picture = first_picture;
    h->pictures = pictures;
    h->vbi_sector = 1;
    h->vbi_sectors = (hunks * 4 + DLC_SECTOR - 1) / DLC_SECTOR;
    h->audio_sector = h->vbi_sector + h->vbi_sectors;
    h->video_sector = h->audio_sector + hunks;
    h->pic_sectors = (has_colour ? 2 : 1) * DLC_PATTERN / DLC_SECTOR;
    h->total_sectors = h->video_sector + pictures * h->pic_sectors;
    h->samples_per_field = DLC_SAMPLES;
}

/* the header's u32 fields after the first 40 bytes, in order */
#define NWORDS 18

static uint32_t *words(dlc_header_t *h, int i)
{
    uint32_t *w[NWORDS] = {&h->hunks,        &h->first_picture, &h->pictures,     &h->vbi_sector,   &h->vbi_sectors,
                           &h->audio_sector, &h->video_sector,  &h->pic_sectors,  &h->total_sectors, &h->samples_per_field,
                           &h->reserved[0],  &h->reserved[1],   &h->reserved[2],  &h->reserved[3],  &h->reserved[4],
                           &h->reserved[5],  &h->reserved[6],   &h->reserved[7]};
    return w[i];
}

void dlc_header_pack(const dlc_header_t *hc, uint8_t *sec)
{
    dlc_header_t *h = (dlc_header_t *)hc;
    memset(sec, 0, DLC_SECTOR);
    memcpy(sec, h->magic, 8);
    put32(sec + 8, h->version);
    put32(sec + 12, h->header_bytes);
    memcpy(sec + 16, h->sha1, 20);
    sec[36] = h->look;
    sec[37] = h->has_colour;
    sec[38] = h->colour;
    sec[39] = h->complete;
    for (int i = 0; i < NWORDS; i++)
        put32(sec + 40 + 4 * i, *words(h, i));
    memcpy(sec + 112, h->resume, sizeof(h->resume));
}

int dlc_header_unpack(const uint8_t *sec, dlc_header_t *h)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, sec, 8);
    h->version = get32(sec + 8);
    h->header_bytes = get32(sec + 12);
    memcpy(h->sha1, sec + 16, 20);
    h->look = sec[36];
    h->has_colour = sec[37];
    h->colour = sec[38];
    h->complete = sec[39];
    for (int i = 0; i < NWORDS; i++)
        *words(h, i) = get32(sec + 40 + 4 * i);
    memcpy(h->resume, sec + 112, sizeof(h->resume));
    return memcmp(h->magic, DLC_MAGIC, 8) == 0 && h->version == DLC_VERSION ? 0 : -1;
}

uint32_t dlc_chunks(const dlc_header_t *h)
{
    return (h->hunks + DLC_CHUNK_HUNKS - 1) / DLC_CHUNK_HUNKS;
}

void dlc_resume_start(dlc_header_t *h)
{
    memset(h->resume, 0, sizeof(h->resume));
    put32(h->resume, DLC_RESUME_MAGIC);
    put32(h->resume + 4, DLC_CHUNK_HUNKS);
    put32(h->resume + 8, dlc_chunks(h));
}

int dlc_resume_valid(const dlc_header_t *h)
{
    return get32(h->resume) == DLC_RESUME_MAGIC && get32(h->resume + 4) == DLC_CHUNK_HUNKS &&
           get32(h->resume + 8) == dlc_chunks(h) && dlc_chunks(h) <= DLC_RESUME_BITS;
}

int dlc_chunk_done(const dlc_header_t *h, uint32_t k)
{
    return (h->resume[12 + k / 8] >> (k % 8)) & 1;
}

void dlc_chunk_set_done(dlc_header_t *h, uint32_t k)
{
    h->resume[12 + k / 8] |= (uint8_t)(1u << (k % 8));
}

uint32_t dlc_chunks_done(const dlc_header_t *h)
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < dlc_chunks(h); k++)
        n += (uint32_t)dlc_chunk_done(h, k);
    return n;
}
