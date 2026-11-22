/*
 * dlc_audio.h -- a field's sound for the cart: the disc's 44.1 kHz stereo
 * (735 or 736 samples a field) taken as one stream, band-limited and
 * resampled to DLC_SAMPLES a field (11,628 Hz; 11,660 on the console), at a
 * fixed gain, rounded to int8 a channel.
 *
 * The resampler is the Movies one (app/engine/src/dsp.rs): a Kaiser-windowed
 * sinc, beta 9, 16 zero crossings each side at the lower rate, cutoff 0.97 of
 * its Nyquist, tabulated. Its reach (about 61 input samples) never passes
 * the fields either side, so a field's bytes need only its neighbours.
 */
#ifndef DLC_AUDIO_H
#define DLC_AUDIO_H

#include <stdint.h>

#define DLC_AUDIO_SAMPLES 194
/* int8 units for a full-scale input. The disc's sound is on its right
 * channel, peaking at 0.251 of full scale (8221): 400 puts that at 100,
 * leaving room for the board's beeps. The left channel is all but silent
 * (noise, rms about 22 of 32768, peaks to 0.165 here and there). */
#define DLC_AUDIO_GAIN 400.0

typedef struct dlc_audio_kernel dlc_audio_kernel;

dlc_audio_kernel *dlc_audio_kernel_new(void);
void dlc_audio_kernel_free(dlc_audio_kernel *k);

/* the three fields around the one wanted (n = 0: none, at the disc's ends) */
typedef struct
{
    const int16_t *l[3], *r[3]; /* previous, this one, next */
    int n[3];
} dlc_audio_window;

typedef struct
{
    double peak_l, peak_r; /* largest |sample| before the gain (1 = full scale) */
    double diff;           /* largest |left - right| */
    uint32_t clipped;      /* samples past +-127 */
} dlc_audio_stats;

/* DLC_AUDIO_SAMPLES left then right into out; stats (optional) accumulate */
void dlc_audio_field(const dlc_audio_kernel *k, const dlc_audio_window *w, double gain, int8_t *out,
                     dlc_audio_stats *stats);

#endif
