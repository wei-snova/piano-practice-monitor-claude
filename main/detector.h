/*
 * detector.h - "is someone playing music right now?" detector.
 *
 * Pure C (no ESP-IDF dependencies) so it can be unit-tested on a PC.
 *
 * Input: mono audio at 8 kHz, delivered in hops of DET_HOP samples.
 * Per hop it computes:
 *   - level (dBFS) and an adaptive noise floor
 *   - harmonicity + fundamental frequency via the YIN algorithm
 *   - pitch stability (music holds notes; speech glides)
 * A frame is "musical" when it is loud, harmonic and pitch-stable.
 * "active" = the fraction of musical frames over the last ~3 s is high enough.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DET_FS        8000
#define DET_FRAME     512                        /* 64 ms analysis window   */
#define DET_HOP       256                        /* 32 ms between analyses  */
#define DET_TAU_MIN   8                          /* 1000 Hz highest f0      */
#define DET_TAU_MAX   128                        /* 62.5 Hz lowest f0 (cello C2 = 65 Hz) */
#define DET_WIN       (DET_FRAME - DET_TAU_MAX)  /* YIN integration window  */
#define DET_RING      94                         /* ~3 s of hops            */

typedef struct {
    float sens_db;        /* how far above the noise floor counts as "loud" (dB) */
    float harm_min;       /* minimum harmonicity (0..1) for a musical frame      */
    int   stable_hops;    /* consecutive pitch-stable hops required              */
    float activity_on;    /* fraction of musical hops in window => active        */
} det_params_t;

typedef struct {
    float level_db;       /* frame RMS, dBFS                   */
    float floor_db;       /* tracked noise floor, dBFS         */
    float harmonicity;    /* 0..1 (1 = perfectly periodic)     */
    float f0;             /* Hz, 0 if none                     */
    float bright_db;      /* high-frequency share of the sound, dB (first-difference
                             energy / energy); falls as upper partials die away */
    bool  musical;        /* this hop looked like music        */
    float activity;       /* 0..1 fraction over the window     */
    bool  active;         /* activity >= activity_on           */
} det_out_t;

typedef struct {
    det_params_t p;
    float frame[DET_FRAME];
    int   filled;
    bool  floor_init;
    float floor_db;
    float last_f0;
    int   stable_run;
    uint8_t ring[DET_RING];
    int   ring_idx;
    int   ring_sum;
    int   ring_count;
} detector_t;

void det_default_params(det_params_t *p);
void det_init(detector_t *d, const det_params_t *p);
void det_set_sensitivity(detector_t *d, float sens_db);

/* Push DET_HOP new samples (float, nominal range -1..1). Fills *out. */
void det_push(detector_t *d, const float *hop, det_out_t *out);

/* Helper: 16 kHz int16 -> 8 kHz float with a [1 2 1]/4 anti-alias filter.
 * n_in must be even; writes n_in/2 samples. `state` keeps one sample of history. */
void det_decimate_16k(const int16_t *in, int n_in, float *out, float *state);

#ifdef __cplusplus
}
#endif
