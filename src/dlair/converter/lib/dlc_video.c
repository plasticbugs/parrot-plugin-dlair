/*
 * dlc_video.c -- see dlc_video.h. A port of the Movies encoder
 * (app/engine/src/video.rs's FrameEncoder for a first frame, filters.rs's
 * resize and grade, presets.rs's looks).
 */
#pragma STDC FP_CONTRACT OFF
#include "dlc_video.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CELLS_ROW (DLC_W / 8)

/* TMS9918 palette, hardware colours 1..15 (0 is transparent) */
static const double PALETTE[15][3] = {
    {0.0, 0.0, 0.0},       /*  1 black */
    {33.0, 200.0, 66.0},   /*  2 medium green */
    {94.0, 220.0, 120.0},  /*  3 light green */
    {84.0, 85.0, 237.0},   /*  4 dark blue */
    {125.0, 118.0, 252.0}, /*  5 light blue */
    {212.0, 82.0, 77.0},   /*  6 dark red */
    {66.0, 235.0, 245.0},  /*  7 cyan */
    {252.0, 85.0, 84.0},   /*  8 medium red */
    {255.0, 121.0, 120.0}, /*  9 light red */
    {212.0, 193.0, 84.0},  /* 10 dark yellow */
    {230.0, 206.0, 128.0}, /* 11 light yellow */
    {33.0, 176.0, 59.0},   /* 12 dark green */
    {201.0, 91.0, 186.0},  /* 13 magenta */
    {204.0, 204.0, 204.0}, /* 14 gray */
    {255.0, 255.0, 255.0}, /* 15 white */
};

static int neutral(int i)
{
    return i == 0 || i == 13 || i == 14;
}

static const uint8_t BAYER8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},    {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},   {63, 31, 55, 23, 61, 29, 53, 21},
};

/* --- the looks (presets.rs) ---------------------------------------------- */

typedef struct
{
    const char *name;
    int mono;   /* 0 colour, 1 bw, 2 grey */
    int diffuse; /* Atkinson (else Bayer) */
    double amp, chroma, colour_bias;
    /* the grade: temperature (0 = none), vibrance, saturation, brightness, gamma */
    double temperature, vibrance, saturation, brightness, gamma;
    double contrast, black; /* contrast around mid grey (1 = none); black point, 0..1 of the range, crushed to black */
} look_t;

static look_t LOOKS[DLC_NLOOKS] = {
    {"bw", 1, 1, 255.0, 1.0, 0.0, 0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0},
    {"grey", 2, 0, 160.0, 1.0, 0.0, 0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0},
    {"cool", 0, 0, 48.0, 1.5, 0.6, 8000.0, 0.5, 1.6, 0.04, 1.1, 1.0, 0.0},
    {"vivid", 0, 0, 48.0, 1.3, 0.8, 9500.0, 0.8, 1.7, 0.04, 1.1, 1.0, 0.0},
};

int dlc_tune(int look, const char *spec)
{
    if (look < 0 || look >= DLC_NLOOKS)
        return -1;
    look_t *l = &LOOKS[look];
    const struct
    {
        const char *key;
        double *v;
    } keys[] = {{"amp", &l->amp},           {"chroma", &l->chroma},   {"bias", &l->colour_bias},
                {"temp", &l->temperature},  {"vib", &l->vibrance},    {"sat", &l->saturation},
                {"bright", &l->brightness}, {"gamma", &l->gamma},     {"contrast", &l->contrast},
                {"black", &l->black}};
    for (const char *s = spec; *s;)
    {
        const char *eq = strchr(s, '=');
        if (!eq)
            return -1;
        int found = 0;
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
            if (strlen(keys[k].key) == (size_t)(eq - s) && !strncmp(s, keys[k].key, (size_t)(eq - s)))
            {
                *keys[k].v = atof(eq + 1);
                found = 1;
            }
        if (!found)
            return -1;
        const char *comma = strchr(eq, ',');
        s = comma ? comma + 1 : eq + strlen(eq);
    }
    return 0;
}

int dlc_look_id(const char *name)
{
    for (int i = 0; i < DLC_NLOOKS; i++)
        if (!strcmp(name, LOOKS[i].name))
            return i;
    return -1;
}

const char *dlc_look_name(int look)
{
    return look >= 0 && look < DLC_NLOOKS ? LOOKS[look].name : "?";
}

int dlc_look_has_colour(int look, uint8_t *colour)
{
    if (LOOKS[look].mono == 1)
    {
        if (colour)
            *colour = 0x1F; /* the one pair: black (bit set) on white */
        return 0;
    }
    return 1;
}

/* --- weaving --------------------------------------------------------------- */

static inline uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void dlc_weave(const uint8_t *yuy2, uint32_t w, uint32_t h, int odd, int dbl, uint8_t *rgb)
{
    for (int i = 0; i < DLC_SRC_H / 2; i++)
    {
        uint32_t sy = DLC_FIELD_FIRST_LINE + (uint32_t)i;
        if (sy >= h)
            sy = h - 1;
        uint8_t *o = rgb + (size_t)(2 * i + (odd ? 1 : 0)) * DLC_SRC_W * 3;
        for (int x = 0; x < DLC_SRC_W; x += 2)
        {
            uint32_t sx = DLC_FIELD_FIRST_X + (uint32_t)x;
            if (sx + 1 >= w)
                sx = (w - 2) & ~1u;
            const uint8_t *s = yuy2 + ((size_t)sy * w + sx) * 2;
            const int y0 = s[0] - 16, u = s[1] - 128, y1 = s[2] - 16, v = s[3] - 128;
            const int yy[2] = {y0, y1};
            for (int k = 0; k < 2; k++)
            {
                const int c = 298 * yy[k];
                o[3 * (x + k) + 0] = clamp8((c + 409 * v + 128) >> 8);
                o[3 * (x + k) + 1] = clamp8((c - 100 * u - 208 * v + 128) >> 8);
                o[3 * (x + k) + 2] = clamp8((c + 516 * u + 128) >> 8);
            }
        }
        if (dbl)
            memcpy(rgb + (size_t)(2 * i + (odd ? 0 : 1)) * DLC_SRC_W * 3, o, DLC_SRC_W * 3);
    }
}

/* --- Lanczos-3 (filters.rs) ------------------------------------------------ */

typedef struct
{
    int lo, n;
    float w[64];
} taps_t;

static double lanczos3(double x)
{
    if (x == 0.0)
        return 1.0;
    if (fabs(x) >= 3.0)
        return 0.0;
    const double px = 3.14159265358979323846 * x;
    return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
}

static void weights(int src, int dst, taps_t *t)
{
    const double ratio = (double)src / (double)dst;
    const double scale = ratio > 1.0 ? ratio : 1.0;
    const double support = 3.0 * scale;
    for (int o = 0; o < dst; o++)
    {
        const double center = ((double)o + 0.5) * ratio - 0.5;
        double flo = floor(center - support);
        int lo = flo > 0.0 ? (int)flo : 0;
        int hi = (int)ceil(center + support);
        if (hi > src - 1)
            hi = src - 1;
        double w[64];
        int n = 0;
        double s = 0.0;
        for (int i = lo; i <= hi && n < 64; i++)
            w[n++] = lanczos3(((double)i - center) / scale);
        for (int k = 0; k < n; k++)
            s += w[k];
        t[o].lo = lo;
        t[o].n = n;
        for (int k = 0; k < n; k++)
            t[o].w[k] = (float)(w[k] / s);
    }
}

static void resize(const uint8_t *src, int sw, int sh, uint8_t *out, int tw, int th)
{
    taps_t *wx = (taps_t *)malloc(sizeof(taps_t) * (size_t)tw), *wy = (taps_t *)malloc(sizeof(taps_t) * (size_t)th);
    if (!wx || !wy)
    {
        free(wx);
        free(wy);
        return;
    }
    weights(sw, tw, wx);
    weights(sh, th, wy);
    float *mid = (float *)malloc(sizeof(float) * (size_t)sh * tw * 3);
    if (!mid)
    {
        free(wx);
        free(wy);
        return;
    }
    for (int y = 0; y < sh; y++)
    {
        const uint8_t *row = src + (size_t)y * sw * 3;
        for (int x = 0; x < tw; x++)
        {
            const taps_t *t = &wx[x];
            float a0 = 0, a1 = 0, a2 = 0;
            for (int k = 0; k < t->n; k++)
            {
                const int p = (t->lo + k) * 3;
                const float wk = t->w[k];
                a0 += (float)row[p] * wk;
                a1 += (float)row[p + 1] * wk;
                a2 += (float)row[p + 2] * wk;
            }
            float *m = mid + ((size_t)y * tw + x) * 3;
            m[0] = a0;
            m[1] = a1;
            m[2] = a2;
        }
    }
    for (int y = 0; y < th; y++)
    {
        const taps_t *t = &wy[y];
        for (int x = 0; x < tw; x++)
        {
            float a[3] = {0, 0, 0};
            for (int k = 0; k < t->n; k++)
            {
                const float *m = mid + ((size_t)(t->lo + k) * tw + x) * 3;
                const float wk = t->w[k];
                a[0] += m[0] * wk;
                a[1] += m[1] * wk;
                a[2] += m[2] * wk;
            }
            for (int c = 0; c < 3; c++)
            {
                float r = roundf(a[c]);
                r = r < 0.0f ? 0.0f : r > 255.0f ? 255.0f : r;
                out[((size_t)y * tw + x) * 3 + c] = (uint8_t)r;
            }
        }
    }
    free(mid);
    free(wx);
    free(wy);
}

/* --- the grade (filters.rs) ------------------------------------------------ */

static void kelvin_rgb(double t, double m[3])
{
    const double k = t / 100.0;
    double r, g, b;
    if (k <= 66.0)
    {
        r = 1.0;
        g = 0.3900815787690196 * log(k) - 0.6318414437886275;
    }
    else
    {
        double d = k - 60.0;
        if (d < 0.0)
            d = 0.0;
        r = 1.292936186062745 * pow(d, -0.1332047592);
        g = 1.129890860895294 * pow(d, -0.0755148492);
    }
    if (k >= 66.0)
        b = 1.0;
    else if (k <= 19.0)
        b = 0.0;
    else
        b = 0.543206789110196 * log(k - 10.0) - 1.19625408914;
    const double v[3] = {r, g, b};
    for (int i = 0; i < 3; i++)
        m[i] = v[i] < 0.0 ? 0.0 : v[i] > 1.0 ? 1.0 : v[i];
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void grade(uint8_t *rgb, size_t npx, const look_t *g)
{
    if (g->temperature == 0.0 && g->vibrance == 0.0 && g->saturation == 1.0 && g->brightness == 0.0 && g->gamma == 1.0 &&
        g->contrast == 1.0 && g->black == 0.0)
        return;
    double mul[3] = {1, 1, 1};
    const int has_mul = g->temperature != 0.0;
    if (has_mul)
        kelvin_rgb(g->temperature, mul);
    double lut[256];
    for (int y = 0; y <= 255; y++)
    {
        double v = (double)y / 255.0 + g->brightness;
        if (g->black > 0.0 && g->black < 1.0)
            v = (v - g->black) / (1.0 - g->black);
        v = (v - 0.5) * g->contrast + 0.5;
        if (v <= 0.0)
            lut[y] = 0.0;
        else
        {
            v = pow(v, 1.0 / g->gamma);
            lut[y] = v >= 1.0 ? 255.0 : 256.0 * v;
        }
    }
    const int eq = g->brightness != 0.0 || g->gamma != 1.0 || g->saturation != 1.0 || g->contrast != 1.0 || g->black != 0.0;
    for (size_t i = 0; i < npx; i++)
    {
        uint8_t *px = rgb + 3 * i;
        double c[3] = {(double)px[0] / 255.0, (double)px[1] / 255.0, (double)px[2] / 255.0};
        if (has_mul)
            for (int k = 0; k < 3; k++)
                c[k] *= mul[k];
        if (g->vibrance != 0.0)
        {
            const double mx = fmax(fmax(c[0], c[1]), c[2]);
            const double mn = fmin(fmin(c[0], c[1]), c[2]);
            const double luma = 0.072186 * c[0] + 0.715158 * c[1] + 0.212656 * c[2];
            const double sgn = g->vibrance > 0.0 ? 1.0 : -1.0;
            const double k = 1.0 + g->vibrance * (1.0 + sgn * (mx - mn));
            for (int j = 0; j < 3; j++)
                c[j] = clampd(luma + (c[j] - luma) * k, 0.0, 1.0);
        }
        if (eq)
        {
            const double y = 16.0 + 65.481 * c[0] + 128.553 * c[1] + 24.966 * c[2];
            const double cb = 128.0 - 37.797 * c[0] - 74.203 * c[1] + 112.0 * c[2];
            const double cr = 128.0 + 112.0 * c[0] - 93.786 * c[1] - 18.214 * c[2];
            const double yi = clampd(y, 0.0, 255.0);
            const double fl = floor(yi);
            const size_t lo = (size_t)fl;
            const double f = yi - fl;
            const double y2 = lo >= 255 ? lut[255] : lut[lo] * (1.0 - f) + lut[lo + 1] * f;
            const double cb2 = clampd((cb - 128.0) * g->saturation + 128.0, 0.0, 255.0);
            const double cr2 = clampd((cr - 128.0) * g->saturation + 128.0, 0.0, 255.0);
            const double yy = 1.1643836 * (y2 - 16.0);
            c[0] = (yy + 1.5960268 * (cr2 - 128.0)) / 255.0;
            c[1] = (yy - 0.3917623 * (cb2 - 128.0) - 0.8129680 * (cr2 - 128.0)) / 255.0;
            c[2] = (yy + 2.0172321 * (cb2 - 128.0)) / 255.0;
        }
        for (int k = 0; k < 3; k++)
            px[k] = (uint8_t)clampd(round(c[k] * 255.0), 0.0, 255.0);
    }
}

void dlc_fit(const uint8_t *rgb704, int look, uint8_t *rgb256)
{
    resize(rgb704, DLC_SRC_W, DLC_SRC_H, rgb256, DLC_W, DLC_H);
    grade(rgb256, (size_t)DLC_W * DLC_H, &LOOKS[look]);
}

void dlc_fit_half(const uint8_t *rgb704, int look, uint8_t *rgb256)
{
    static _Thread_local uint8_t small[DLC_HALF_W * DLC_HALF_H * 3];
    resize(rgb704, DLC_SRC_W, DLC_SRC_H, small, DLC_HALF_W, DLC_HALF_H);
    grade(small, (size_t)DLC_HALF_W * DLC_HALF_H, &LOOKS[look]);
    memset(rgb256, 0, (size_t)DLC_W * DLC_H * 3);
    for (int y = 0; y < DLC_HALF_H; y++)
        memcpy(rgb256 + 3 * ((size_t)(y + DLC_HALF_Y) * DLC_W + DLC_HALF_X), small + 3 * (size_t)y * DLC_HALF_W,
               3 * DLC_HALF_W);
}

void dlc_blank_outside_half(uint8_t *pat, uint8_t *col)
{
    for (int y = 0; y < DLC_H; y++)
        for (int c = 0; c < CELLS_ROW; c++)
        {
            const int x = c * 8;
            if (y >= DLC_HALF_Y && y < DLC_HALF_Y + DLC_HALF_H && x >= DLC_HALF_X && x < DLC_HALF_X + DLC_HALF_W)
                continue;
            const int at = dlc_addr(y, c);
            pat[at] = 0;
            col[at] = 0x11; /* black on black: the same bytes every picture, so the plugin never sends them again */
        }
}

/* --- the encoder (video.rs, a first frame: no temporal terms) --------------- */

static double luma3(const double p[3])
{
    return p[0] * 0.299 + p[1] * 0.587 + p[2] * 0.114;
}

typedef struct
{
    int n;
    uint8_t a[120], b[120];
    double scale[120];
} pairs_t;

static void make_pairs(const look_t *l, pairs_t *p)
{
    p->n = 0;
    for (int i = 0; i < 15; i++)
        for (int j = i; j < 15; j++)
        {
            int keep = l->mono == 1 ? (i == 0 && j == 14) : l->mono == 2 ? (neutral(i) && neutral(j)) : 1;
            if (!keep)
                continue;
            p->a[p->n] = (uint8_t)i;
            p->b[p->n] = (uint8_t)j;
            p->scale[p->n] = l->mono ? 1.0 : 1.0 + l->colour_bias * (double)(neutral(i) + neutral(j)) / 2.0;
            p->n++;
        }
}

/* ordered / none: the best pair for each 8x1 cell, tried exhaustively */
static void enc_fit(const look_t *l, const pairs_t *pr, const uint8_t *rgb, const double *lum, uint8_t *pat,
                    uint8_t *col, uint8_t *shown)
{
    const int mono = l->mono != 0;
    const double chroma = l->chroma;
    double d[8][15], err[120];
    double pal_y[15];
    for (int k = 0; k < 15; k++)
        pal_y[k] = 0.299 * PALETTE[k][0] + 0.587 * PALETTE[k][1] + 0.114 * PALETTE[k][2];
    for (int y = 0; y < DLC_H; y++)
        for (int c = 0; c < CELLS_ROW; c++)
        {
            for (int i = 0; i < 8; i++)
            {
                const int x = c * 8 + i;
                double px[3];
                for (int ch = 0; ch < 3; ch++)
                    px[ch] = mono ? lum[y * DLC_W + x] : (double)rgb[3 * (y * DLC_W + x) + ch];
                const double off = ((double)BAYER8[y % 8][x % 8] / 64.0 - 0.5) * l->amp;
                for (int ch = 0; ch < 3; ch++)
                {
                    px[ch] += off;
                    px[ch] = clampd(px[ch], -64.0, 319.0);
                }
                const double ay = 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2];
                for (int k = 0; k < 15; k++)
                {
                    const double *q = PALETTE[k];
                    const double by = pal_y[k];
                    const double dy = ay - by;
                    const double dcb = (px[2] - ay) - (q[2] - by);
                    const double dcr = (px[0] - ay) - (q[0] - by);
                    d[i][k] = dy * dy + chroma * (dcb * dcb + dcr * dcr);
                }
            }
            for (int k = 0; k < pr->n; k++)
            {
                const int a = pr->a[k], b = pr->b[k];
                double s = 0.0;
                for (int i = 0; i < 8; i++)
                    s += fmin(d[i][a], d[i][b]);
                err[k] = s * pr->scale[k];
            }
            int best = 0;
            for (int k = 1; k < pr->n; k++)
                if (err[k] < err[best])
                    best = k;
            const int ia = pr->a[best], ib = pr->b[best];
            uint8_t bits = 0;
            for (int i = 0; i < 8; i++)
            {
                const int use_a = d[i][ia] <= d[i][ib];
                if (use_a)
                    bits |= (uint8_t)(0x80 >> i);
                if (shown)
                {
                    const double *q = PALETTE[use_a ? ia : ib];
                    uint8_t *o = shown + 3 * (y * DLC_W + c * 8 + i);
                    for (int ch = 0; ch < 3; ch++)
                        o[ch] = (uint8_t)q[ch];
                }
            }
            const int at = dlc_addr(y, c);
            pat[at] = bits;
            col[at] = (uint8_t)(((ia + 1) << 4) | (ib + 1));
        }
}

/* Atkinson, serpentine, within the Mode 2 rule (one pair per 8x1 cell) */
static void enc_diffuse(const pairs_t *pr, const double *lum, uint8_t *pat, uint8_t *col, uint8_t *shown)
{
    static const struct
    {
        int dx, dy;
        double w;
    } K[6] = {{1, 0, 1.0 / 8.0}, {2, 0, 1.0 / 8.0}, {-1, 1, 1.0 / 8.0}, {0, 1, 1.0 / 8.0}, {1, 1, 1.0 / 8.0}, {0, 2, 1.0 / 8.0}};
    double pl[15];
    for (int k = 0; k < 15; k++)
        pl[k] = luma3(PALETTE[k]);
    const int np = pr->n;
    double *img = (double *)malloc(sizeof(double) * DLC_W * DLC_H);
    uint8_t *bits = (uint8_t *)malloc(DLC_W * DLC_H);
    uint8_t *sel = (uint8_t *)malloc(DLC_H * CELLS_ROW);
    if (!img || !bits || !sel)
    {
        free(img);
        free(bits);
        free(sel);
        return;
    }
    memcpy(img, lum, sizeof(double) * DLC_W * DLC_H);
    for (int y = 0; y < DLC_H; y++)
    {
        const int fwd = y % 2 == 0;
        const int s = fwd ? 1 : -1;
        for (int ci = 0; ci < CELLS_ROW; ci++)
        {
            const int c = fwd ? ci : CELLS_ROW - 1 - ci;
            const int x0 = c * 8;
            int k = 0;
            if (np > 1)
            {
                double best = 0;
                int have = 0;
                for (int i = 0; i < np; i++)
                {
                    const double a = pl[pr->a[i]], b = pl[pr->b[i]];
                    double e = 0.0;
                    for (int x = x0; x < x0 + 8; x++)
                    {
                        const double v = img[y * DLC_W + x];
                        const double da = v - a, db = v - b;
                        e += fmin(da * da, db * db);
                    }
                    if (!have || e < best)
                    {
                        best = e;
                        have = 1;
                        k = i;
                    }
                }
            }
            sel[y * CELLS_ROW + c] = (uint8_t)k;
            const double a = pl[pr->a[k]], b = pl[pr->b[k]];
            for (int j = 0; j < 8; j++)
            {
                const int x = fwd ? x0 + j : x0 + 7 - j;
                const double v = img[y * DLC_W + x];
                const double da = fabs(v - a), db = fabs(v - b);
                const int use_a = da <= db;
                bits[y * DLC_W + x] = (uint8_t)use_a;
                const double e = v - (use_a ? a : b);
                for (int t = 0; t < 6; t++)
                {
                    const int xx = x + K[t].dx * s;
                    if (xx >= 0 && xx < DLC_W && y + K[t].dy < DLC_H)
                        img[(y + K[t].dy) * DLC_W + xx] += e * K[t].w;
                }
            }
        }
    }
    for (int y = 0; y < DLC_H; y++)
        for (int c = 0; c < CELLS_ROW; c++)
        {
            const int ia = pr->a[sel[y * CELLS_ROW + c]], ib = pr->b[sel[y * CELLS_ROW + c]];
            uint8_t byte = 0;
            for (int i = 0; i < 8; i++)
            {
                const int x = c * 8 + i;
                const int ua = bits[y * DLC_W + x];
                if (ua)
                    byte |= (uint8_t)(0x80 >> i);
                if (shown)
                {
                    const double *q = PALETTE[ua ? ia : ib];
                    uint8_t *o = shown + 3 * (y * DLC_W + x);
                    for (int ch = 0; ch < 3; ch++)
                        o[ch] = (uint8_t)q[ch];
                }
            }
            const int at = dlc_addr(y, c);
            pat[at] = byte;
            col[at] = (uint8_t)(((ia + 1) << 4) | (ib + 1));
        }
    free(img);
    free(bits);
    free(sel);
}

void dlc_encode(const uint8_t *rgb, int look, uint8_t *pat, uint8_t *col, uint8_t *shown)
{
    const look_t *l = &LOOKS[look];
    pairs_t pr;
    make_pairs(l, &pr);
    double *lum = NULL;
    if (l->mono)
    {
        lum = (double *)malloc(sizeof(double) * DLC_W * DLC_H);
        if (!lum)
            return;
        for (int i = 0; i < DLC_W * DLC_H; i++)
            lum[i] = (double)rgb[3 * i] * 0.299 + (double)rgb[3 * i + 1] * 0.587 + (double)rgb[3 * i + 2] * 0.114;
    }
    if (l->diffuse)
        enc_diffuse(&pr, lum, pat, col, shown);
    else
        enc_fit(l, &pr, rgb, lum, pat, col, shown);
    free(lum);
}

void dlc_render(const uint8_t *pat, const uint8_t *col, uint8_t *out)
{
    memset(out, 0, (size_t)DLC_W * DLC_H * 3);
    for (int y = 0; y < DLC_H; y++)
        for (int c = 0; c < CELLS_ROW; c++)
        {
            const int a = dlc_addr(y, c);
            const uint8_t bits = pat[a], cb = col[a];
            for (int i = 0; i < 8; i++)
            {
                const int n = (bits & (0x80 >> i)) ? cb >> 4 : cb & 15;
                if (n > 0)
                {
                    uint8_t *o = out + 3 * (y * DLC_W + c * 8 + i);
                    for (int ch = 0; ch < 3; ch++)
                        o[ch] = (uint8_t)PALETTE[n - 1][ch];
                }
            }
        }
}
