// dlcardio.c -- see dlcardio.h.
#include "dlcardio.h"

#include <string.h>

#include "../starwars/swzip.h"

const dl_romfile_t dl_romfiles[DL_ROMFILES] = {
    {"dl_f2_u1.bin", 0xf5ea3b9d, 0x0000},
    {"dl_f2_u2.bin", 0xdcc1dff2, 0x2000},
    {"dl_f2_u3.bin", 0xab514e5b, 0x4000},
    {"dl_f2_u4.bin", 0xf5ec23d2, 0x6000},
};

// the CHD MAME lists for dlair (its header's SHA-1)
const uint8_t dl_chd_sha1[20] = {0xc3, 0xee, 0x2e, 0x27, 0xaa, 0x48, 0x47, 0xbf, 0x38, 0x84,
                                 0xfc, 0x0d, 0x18, 0xf2, 0x6f, 0x31, 0x54, 0x56, 0xaa, 0x9e};

static uint8_t sec[512];

uint32_t dl_crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static int wanted(uint32_t size, uint32_t crc, uint32_t found) {
    for (int i = 0; i < DL_ROMFILES; i++)
        if (size == DL_ROMSIZE && dl_romfiles[i].crc == crc && !(found & 1u << i)) return i;
    return -1;
}

static bool read_file(fs_t *fs, const fs_file_t *f, uint8_t *buf, uint32_t n, void (*tick)(void)) {
    if (fs_open(fs, f) != FS_OK) return false;
    for (uint32_t at = 0; at < n; at += 512) {
        uint32_t lba = fs_lba(fs, at / 512);
        if (!lba || !fs->read(lba, sec)) return false;
        memcpy(buf + at, sec, n - at < 512 ? n - at : 512);
        if (tick) tick();
    }
    return true;
}

uint32_t dl_load_roms(fs_t *fs, uint8_t *rom, uint8_t *buf, uint32_t max, void (*tick)(void)) {
    uint32_t found = 0;
    fs_dir_t d;
    fs_file_t e;
    static fs_file_t files[32];
    int nf = 0;
    // the folder's listing first: reading a file moves fsro's map.
    // fs_dir_next: 1 an entry, 0 the end, < 0 an error
    if (fs_dir_open_path(fs, &d, DL_HOME) != FS_OK) return 0;
    while (nf < 32 && fs_dir_next(fs, &d, &e) == 1)
        if (!e.is_dir && e.size && e.size <= max) files[nf++] = e;
    for (int i = 0; i < nf && found != DL_ROMS_ALL; i++) {
        uint32_t n = (uint32_t)files[i].size;
        if (!read_file(fs, &files[i], buf, n, tick)) continue;
        int r = wanted(n, dl_crc32(buf, n), found);
        if (r >= 0) {
            memcpy(rom + dl_romfiles[r].at, buf, DL_ROMSIZE);
            found |= 1u << r;
            continue;
        }
        swzip_entry_t z;                // a zip of the set (MAME's dlair.zip)
        for (int k = 0; swzip_entry(buf, n, k, &z); k++) {
            int w = z.data ? wanted(z.usize, z.crc, found) : -1;
            if (w < 0) continue;
            uint8_t *dst = rom + dl_romfiles[w].at;
            if (swzip_read(&z, dst) && dl_crc32(dst, DL_ROMSIZE) == z.crc) found |= 1u << w;
            if (tick) tick();
        }
    }
    return found;
}

// every cluster of f, from the FAT (fsro's next_clus, with its own sector)
static int map_chain(fs_t *fs, const fs_file_t *f, dl_dat_t *d, uint32_t *map, uint32_t map_max, void (*tick)(void)) {
    uint32_t cbytes = 512u << fs->spc_shift;
    uint32_t n = (uint32_t)((f->size + cbytes - 1) / cbytes), c = f->first_clus, cached = 0, pieces = 0;
    if (n > map_max) return DL_DAT_PIECES;
    for (uint32_t i = 0; i < n; i++) {
        if (c < 2 || c >= fs->nclus + 2) return DL_DAT_UNREADABLE;
        map[i] = c;
        if (!i || c != map[i - 1] + 1) pieces++;
        if (i + 1 == n) break;
        if (f->contig) {                // exFAT's NoFatChain: one run, no FAT
            c++;
            continue;
        }
        uint32_t per = fs->type == FS_FAT16 ? 256 : 128, lba = fs->fat_lba + c / per;
        if (lba != cached) {
            if (!fs->read(lba, sec)) return DL_DAT_UNREADABLE;
            cached = lba;
            if (tick) tick();
        }
        const uint8_t *e = sec + c % per * (fs->type == FS_FAT16 ? 2 : 4);
        c = fs->type == FS_FAT16 ? (uint32_t)(e[0] | e[1] << 8) : (uint32_t)(e[0] | e[1] << 8 | e[2] << 16 | (uint32_t)e[3] << 24);
        if (fs->type == FS_FAT32) c &= 0x0FFFFFFF;
    }
    d->map = map;
    d->clusters = n;
    d->pieces = pieces;
    return DL_DAT_OK;
}

uint32_t dl_dat_lba(dl_dat_t *d, uint32_t sec) {
    if (!d->map) return fs_lba(d->fs, sec);
    fs_t *fs = d->fs;
    uint32_t ci = sec >> fs->spc_shift;
    if (ci >= d->clusters) return 0;
    return fs->heap_lba + ((d->map[ci] - 2) << fs->spc_shift) + (sec & ((1u << fs->spc_shift) - 1));
}

int dl_open_dat(fs_t *fs, fs_file_t *f, dlc_header_t *h, dl_dat_t *d, uint32_t *map, uint32_t map_max, void (*tick)(void)) {
    d->fs = fs;
    d->map = NULL;
    d->clusters = d->pieces = 0;
    if (fs_find(fs, DL_HOME "/" DL_DAT, f) != FS_OK || f->is_dir) return DL_DAT_MISSING;
    int r = fs_open(fs, f);
    if (r == FS_EFRAG) {
        if (!map) return DL_DAT_PIECES;
        if ((r = map_chain(fs, f, d, map, map_max, tick)) != DL_DAT_OK) return r;
    } else if (r != FS_OK) {
        return DL_DAT_UNREADABLE;
    } else {
        d->pieces = (uint32_t)fs->next;
    }
    uint32_t lba = dl_dat_lba(d, 0);
    if (!lba || !fs->read(lba, sec)) return DL_DAT_UNREADABLE;
    memcpy(h, sec, sizeof *h);
    if (memcmp(h->magic, DLC_MAGIC, 8) || h->version != DLC_VERSION) return DL_DAT_NOT_ONE;
    if (memcmp(h->sha1, dl_chd_sha1, 20)) return DL_DAT_OTHER_DISC;
    if (!h->complete) return DL_DAT_UNFINISHED;
    if ((uint64_t)h->total_sectors * DLC_SECTOR > f->size || !h->hunks || h->hunks > DLC_HUNKS_MAX ||
        h->samples_per_field != DLC_SAMPLES || h->pic_sectors != (h->has_colour ? 24u : 12u))
        return DL_DAT_NOT_ONE;
    return DL_DAT_OK;
}
