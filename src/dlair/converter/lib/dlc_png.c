/*
 * dlc_png.c -- see dlc_png.h.
 */
#include "dlc_png.h"

#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* a chunk at o: length, type, data, CRC; returns the next offset */
static size_t chunk(uint8_t *o, const char *type, const uint8_t *data, uint32_t len)
{
    be32(o, len);
    memcpy(o + 4, type, 4);
    if (len)
        memcpy(o + 8, data, len);
    be32(o + 8 + len, crc32(0, o + 4, len + 4));
    return 12 + (size_t)len;
}

uint8_t *dlc_png(const uint8_t *rgb, int w, int h, size_t *n)
{
    const size_t row = (size_t)w * 3 + 1, raw = row * (size_t)h;
    const size_t blocks = (raw + 65534) / 65535;
    const size_t zlen = 2 + raw + 5 * blocks + 4;
    uint8_t *z = (uint8_t *)malloc(zlen);
    uint8_t *out = (uint8_t *)malloc(8 + 25 + 12 + zlen + 12);
    uint8_t *flat = (uint8_t *)malloc(raw);
    if (!z || !out || !flat)
    {
        free(z);
        free(out);
        free(flat);
        return NULL;
    }
    for (int y = 0; y < h; y++)
    {
        flat[y * row] = 0;
        memcpy(flat + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i++)
    {
        a = (a + flat[i]) % 65521;
        b = (b + a) % 65521;
    }
    size_t zo = 0;
    z[zo++] = 0x78;
    z[zo++] = 0x01;
    for (size_t at = 0; at < raw;)
    {
        const size_t k = raw - at < 65535 ? raw - at : 65535;
        z[zo++] = at + k == raw;
        z[zo++] = (uint8_t)k;
        z[zo++] = (uint8_t)(k >> 8);
        z[zo++] = (uint8_t)~k;
        z[zo++] = (uint8_t)(~k >> 8);
        memcpy(z + zo, flat + at, k);
        zo += k;
        at += k;
    }
    be32(z + zo, (b << 16) | a);
    zo += 4;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    memcpy(out, sig, 8);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bits */
    ihdr[9] = 2;  /* RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    size_t o = 8;
    o += chunk(out + o, "IHDR", ihdr, 13);
    o += chunk(out + o, "IDAT", z, (uint32_t)zo);
    o += chunk(out + o, "IEND", NULL, 0);
    free(z);
    free(flat);
    *n = o;
    return out;
}
