/*
 * dlc_audio.c -- see dlc_audio.h.
 */
#pragma STDC FP_CONTRACT OFF
#include "dlc_audio.h"

#include <math.h>
#include <stdlib.h>

#define FIN 44100.0
#define FOUT (DLC_AUDIO_SAMPLES * 60000.0 / 1001.0)
#define OS 256 /* table points per input sample */

struct dlc_audio_kernel
{
    double half; /* reach in input samples */
    int tn;
    double *table;
};

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, k = 1.0;
    while (term > 1e-12 * sum)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        k += 1.0;
    }
    return sum;
}

dlc_audio_kernel *dlc_audio_kernel_new(void)
{
    dlc_audio_kernel *k = (dlc_audio_kernel *)calloc(1, sizeof(*k));
    if (!k)
        return NULL;
    const double down = FOUT / FIN;
    const double fc = 0.97 * 0.5 * down; /* cycles per input sample */
    k->half = 16.0 / down;
    k->tn = (int)ceil(k->half * OS) + 2;
    k->table = (double *)malloc(sizeof(double) * (size_t)k->tn);
    if (!k->table)
    {
        free(k);
        return NULL;
    }
    const double beta = 9.0, i0b = bessel_i0(beta), pi = 3.14159265358979323846;
    for (int i = 0; i < k->tn; i++)
    {
        const double t = (double)i / OS;
        if (t > k->half)
        {
            k->table[i] = 0.0;
            continue;
        }
        const double s = t == 0.0 ? 1.0 : sin(2.0 * pi * fc * t) / (2.0 * pi * fc * t);
        const double r = t / k->half;
        const double q = 1.0 - r * r;
        k->table[i] = 2.0 * fc * s * bessel_i0(beta * sqrt(q > 0.0 ? q : 0.0)) / i0b;
    }
    return k;
}

void dlc_audio_kernel_free(dlc_audio_kernel *k)
{
    if (k)
        free(k->table);
    free(k);
}

static double kern(const dlc_audio_kernel *k, double t)
{
    const double u = fabs(t) * OS;
    const int i = (int)u;
    if (i + 1 >= k->tn)
        return 0.0;
    const double f = u - i;
    return k->table[i] * (1.0 - f) + k->table[i + 1] * f;
}

/* sample i of the stream, 0 being this field's first */
static void at(const dlc_audio_window *w, int i, double *l, double *r)
{
    int f = 1;
    if (i < 0)
    {
        f = 0;
        i += w->n[0];
    }
    else if (i >= w->n[1])
    {
        i -= w->n[1];
        f = 2;
    }
    if (i < 0 || i >= w->n[f] || !w->l[f])
    {
        *l = *r = 0.0;
        return;
    }
    *l = w->l[f][i] / 32768.0;
    *r = w->r[f][i] / 32768.0;
}

static int8_t to8(double v, double gain, dlc_audio_stats *st)
{
    double q = round(v * gain);
    if (q > 127.0 || q < -127.0)
    {
        if (st)
            st->clipped++;
        q = q > 0 ? 127.0 : -127.0;
    }
    return (int8_t)q;
}

void dlc_audio_field(const dlc_audio_kernel *k, const dlc_audio_window *w, double gain, int8_t *out,
                     dlc_audio_stats *st)
{
    const int n = w->n[1];
    for (int j = 0; j < DLC_AUDIO_SAMPLES; j++)
    {
        const double c = (double)j * n / DLC_AUDIO_SAMPLES;
        const int lo = (int)ceil(c - k->half), hi = (int)floor(c + k->half);
        double sl = 0.0, sr = 0.0;
        for (int i = lo; i <= hi; i++)
        {
            double l, r;
            at(w, i, &l, &r);
            const double g = kern(k, (double)i - c);
            sl += l * g;
            sr += r * g;
        }
        if (st)
        {
            if (fabs(sl) > st->peak_l)
                st->peak_l = fabs(sl);
            if (fabs(sr) > st->peak_r)
                st->peak_r = fabs(sr);
            if (fabs(sl - sr) > st->diff)
                st->diff = fabs(sl - sr);
        }
        out[j] = to8(sl, gain, st);
        out[DLC_AUDIO_SAMPLES + j] = to8(sr, gain, st);
    }
}
