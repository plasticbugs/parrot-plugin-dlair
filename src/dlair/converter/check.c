/*
 * dlair-check: reads a DLAIR.DAT back, as the plugin would, to test the
 * converter (not for users).
 *
 *   dlair-check DLAIR.DAT [--png DIR P,P,...]
 *
 * Checks the header against the file, counts the VBI table's picture codes,
 * measures the sound, and with --png renders those pictures' records to
 * DIR/pNNNNN.png (the same as the converter's previews of that look).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlc_card.h"
#include "dlc_png.h"
#include "dlc_video.h"

static FILE *f;

static int rd(uint64_t sector, void *p, size_t n)
{
    return fseek(f, (long)(sector * DLC_SECTOR), SEEK_SET) || fread(p, 1, n, f) != n ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "dlair-check DLAIR.DAT [--png DIR P,P,...]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    uint8_t sec[DLC_SECTOR];
    dlc_header_t h;
    if (rd(0, sec, sizeof sec) || dlc_header_unpack(sec, &h))
    {
        printf("FAIL: not a DLAIR.DAT\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    int bad = 0;
#define CHECK(c, ...)                   \
    do                                  \
    {                                   \
        if (!(c))                       \
        {                               \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");               \
            bad = 1;                    \
        }                               \
    } while (0)
    printf("look %u, colour table %s (byte %02X), complete %u, %u fields, pictures %u..%u, %u sectors (%.1f MB)\n", h.look,
           h.has_colour ? "per picture" : "fixed", h.colour, h.complete, h.hunks, h.first_picture,
           h.first_picture + h.pictures - 1, h.total_sectors, h.total_sectors / 2048.0);
    CHECK(h.header_bytes == sizeof(dlc_header_t), "header_bytes %u", h.header_bytes);
    CHECK(h.samples_per_field == DLC_SAMPLES, "samples %u", h.samples_per_field);
    CHECK((long)h.total_sectors * DLC_SECTOR == size, "size %ld, header says %u sectors", size, h.total_sectors);
    CHECK(h.vbi_sector == 1 && h.audio_sector == h.vbi_sector + h.vbi_sectors && h.video_sector == h.audio_sector + h.hunks,
          "layout");
    CHECK(h.pic_sectors == (h.has_colour ? 24u : 12u), "pic_sectors %u", h.pic_sectors);
    CHECK(h.total_sectors == h.video_sector + h.pictures * h.pic_sectors, "total");
    CHECK(h.complete == 1, "not complete");

    /* the VBI table: picture codes, and each picture number's first field */
    uint8_t *vbi = (uint8_t *)malloc((size_t)h.vbi_sectors * DLC_SECTOR);
    rd(h.vbi_sector, vbi, (size_t)h.vbi_sectors * DLC_SECTOR);
    uint32_t pics = 0, lo = 0xFFFFFFFF, hi = 0;
    for (uint32_t i = 0; i < h.hunks; i++)
    {
        const uint32_t v = vbi[4 * i] | vbi[4 * i + 1] << 8 | (uint32_t)vbi[4 * i + 2] << 16;
        if ((v & 0xF00000) == 0xF00000)
        {
            const uint32_t p = ((v >> 16) & 7) * 10000 + ((v >> 12) & 15) * 1000 + ((v >> 8) & 15) * 100 + ((v >> 4) & 15) * 10 + (v & 15);
            pics++;
            lo = p < lo ? p : lo;
            hi = p > hi ? p : hi;
        }
    }
    printf("VBI: %u fields with a picture code, %u..%u\n", pics, lo, hi);
    CHECK(lo == h.first_picture && hi == h.first_picture + h.pictures - 1, "picture range");

    /* the sound: how many fields are silent, and the loudest */
    uint32_t silent = 0, differ = 0;
    int peak = 0;
    double sum2 = 0;
    for (uint32_t i = 0; i < h.hunks; i++)
    {
        rd(h.audio_sector + i, sec, sizeof sec);
        int quiet = 1, same = 1;
        for (int k = 0; k < 2 * DLC_SAMPLES; k++)
        {
            const int v = (int8_t)sec[k];
            quiet &= v >= -1 && v <= 1;
            peak = abs(v) > peak ? abs(v) : peak;
            sum2 += (double)v * v;
        }
        for (int k = 0; k < DLC_SAMPLES; k++)
            same &= sec[k] == sec[DLC_SAMPLES + k];
        for (int k = 2 * DLC_SAMPLES; k < DLC_SECTOR; k++)
            CHECK(sec[k] == 0, "field %u: sector tail not zero", i);
        silent += (uint32_t)quiet;
        differ += (uint32_t)!same;
    }
    printf("sound: %u of %u fields silent, peak %d, rms %.1f, %u fields where left and right differ\n", silent, h.hunks, peak,
           sqrt(sum2 / (2.0 * DLC_SAMPLES * h.hunks)), differ);
    CHECK(silent < h.hunks / 2, "mostly silent");

    for (int i = 2; i + 2 < argc; i++)
        if (!strcmp(argv[i], "--png"))
        {
            const char *dir = argv[i + 1];
            uint8_t *rec = (uint8_t *)malloc((size_t)h.pic_sectors * DLC_SECTOR), col[DLC_SCREEN], rgb[DLC_W * DLC_H * 3];
            for (const char *s = argv[i + 2]; *s;)
            {
                const int p = atoi(s);
                if (p >= (int)h.first_picture && p < (int)(h.first_picture + h.pictures))
                {
                    rd(h.video_sector + (uint64_t)(p - h.first_picture) * h.pic_sectors, rec, (size_t)h.pic_sectors * DLC_SECTOR);
                    if (!h.has_colour)
                        memset(col, h.colour, sizeof col);
                    dlc_render(rec, h.has_colour ? rec + DLC_SCREEN : col, rgb);
                    char path[4096];
                    snprintf(path, sizeof path, "%s/p%05d.png", dir, p);
                    size_t n;
                    uint8_t *png = dlc_png(rgb, DLC_W, DLC_H, &n);
                    FILE *o = fopen(path, "wb");
                    if (o)
                    {
                        fwrite(png, 1, n, o);
                        fclose(o);
                        printf("%s\n", path);
                    }
                    free(png);
                }
                while (*s && *s != ',')
                    s++;
                if (*s == ',')
                    s++;
            }
            free(rec);
        }
    printf("%s\n", bad ? "FAILED" : "ok");
    return bad;
}
