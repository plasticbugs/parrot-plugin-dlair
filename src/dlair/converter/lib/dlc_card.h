/*
 * dlc_card.h -- DLAIR.DAT's header (../../dlcard.h, the plugin's) laid out,
 * packed and read back, and the converter's resume record in it: the file
 * is made in chunks of hunks, and the header says which are done.
 */
#ifndef DLC_CARD_H
#define DLC_CARD_H

#include <stdint.h>

#include "dlcard.h"

#define DLC_CHUNK_HUNKS 256
#define DLC_RESUME_MAGIC 0x314D5352u /* "RSM1" */
#define DLC_RESUME_BITS ((256 - 12) * 8)

/* the layout for a disc: sector numbers, sizes; complete 0, nothing done */
void dlc_header_layout(dlc_header_t *h, uint32_t hunks, uint32_t first_picture, uint32_t pictures, int look,
                       int has_colour, uint8_t colour, const uint8_t sha1[20]);
/* to and from the 512-byte sector, little-endian; unpack returns 0 if it is one */
void dlc_header_pack(const dlc_header_t *h, uint8_t *sec);
int dlc_header_unpack(const uint8_t *sec, dlc_header_t *h);

uint32_t dlc_chunks(const dlc_header_t *h);
/* the resume record: started (the fixed parts are written), chunks done */
void dlc_resume_start(dlc_header_t *h);
int dlc_resume_valid(const dlc_header_t *h);
int dlc_chunk_done(const dlc_header_t *h, uint32_t k);
void dlc_chunk_set_done(dlc_header_t *h, uint32_t k);
uint32_t dlc_chunks_done(const dlc_header_t *h);

#endif
