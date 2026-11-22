/*
 * dlc_chd.h -- MAME's laserdisc image (a CHD of version 5, codec "avhu"),
 * read field by field for the converter: no files here, the caller hands in
 * a read callback (a file, a browser's File, ...).
 *
 * dlc_chd_open reads the header, the hunk map and the metadata (the VBI of
 * every field, and so the picture numbers). Each thread then decodes fields
 * through its own dlc_hunkreader, which may have its own callback.
 */
#ifndef DLC_CHD_H
#define DLC_CHD_H

#include <stddef.h>
#include <stdint.h>

/* n bytes from ofs into dst; 0 when all of them came */
typedef int (*dlc_read_fn)(void *ctx, uint64_t ofs, void *dst, size_t n);

#define DLC_CHD_MAX_FIELD_SAMPLES 1024

typedef struct dlc_chd dlc_chd;
typedef struct dlc_hunkreader dlc_hunkreader;

/* NULL on failure, with the reason in err */
dlc_chd *dlc_chd_open(dlc_read_fn rd, void *ctx, char *err, size_t errlen);
void dlc_chd_close(dlc_chd *d);

/* the SHA-1 in the header (of the data and metadata): the one MAME lists */
void dlc_chd_sha1(const dlc_chd *d, uint8_t sha1[20]);
uint32_t dlc_chd_hunks(const dlc_chd *d);
/* a field's line 17/18 code (24 bits, MAME's vbi_metadata.line1718) */
uint32_t dlc_chd_line1718(const dlc_chd *d, uint32_t hunk);
/* the picture numbers on the disc, and where each starts: its first field
 * (-1: not on the disc) and how many fields it spans */
int dlc_chd_first(const dlc_chd *d);
int dlc_chd_last(const dlc_chd *d);
int32_t dlc_chd_picture(const dlc_chd *d, int p, int *nfields);
void dlc_chd_info(const dlc_chd *d, char *out, size_t outlen);

typedef struct
{
    uint32_t samples;            /* stereo pairs (735 or 736) */
    const int16_t *left, *right; /* valid until the next read */
    const uint8_t *yuy2;         /* the field, width x height YUY2 (NULL unless asked for) */
    uint32_t width, height;
} dlc_field;

dlc_hunkreader *dlc_hunkreader_new(dlc_chd *d, dlc_read_fn rd, void *ctx);
void dlc_hunkreader_free(dlc_hunkreader *r);
/* 0 on success; want_video 0 decodes the sound alone (much faster) */
int dlc_hunkreader_read(dlc_hunkreader *r, uint32_t hunk, int want_video, dlc_field *f);
/* fields whose decoded bytes failed the map's CRC (video reads only) */
uint32_t dlc_hunkreader_crc_errors(const dlc_hunkreader *r);

#endif
