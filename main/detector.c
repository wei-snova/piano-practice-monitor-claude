#include "detector.h"

#include <math.h>
#include <string.h>

#define YIN_THRESHOLD   0.15f
#define FLOOR_DOWN_RATE 0.2f    /* fraction per hop toward a quieter level  */
#define FLOOR_UP_DB     0.03f   /* dB per hop (~1 dB/s) on non-harmonic hops */
#define FLOOR_INIT_DB  -70.0f   /* start low so booting mid-practice still works */
#define FLOOR_MIN_DB   -90.0f
#define ABS_MIN_DB     -65.0f   /* never call anything quieter than this "loud" */
#define PITCH_TOL       0.04f   /* 4 % counts as the same note */

void det_default_params(det_params_t *p)
{
    p->sens_db     = 15.0f;
    p->harm_min    = 0.70f;
    p->stable_hops = 2;
    p->activity_on = 0.30f;
}

void det_init(detector_t *d, const det_params_t *p)
{
    memset(d, 0, sizeof(*d));
    if (p) {
        d->p = *p;
    } else {
        det_default_params(&d->p);
    }
}

void det_set_sensitivity(detector_t *d, float sens_db)
{
    d->p.sens_db = sens_db;
}

void det_decimate_16k(const int16_t *in, int n_in, float *out, float *state)
{
    const float k = 1.0f / (32768.0f * 4.0f);
    float prev = *state;
    for (int i = 0, o = 0; i + 1 < n_in; i += 2, o++) {
        float a = (float)in[i];
        float b = (float)in[i + 1];
        out[o] = (prev + 2.0f * a + b) * k;
        prev = b;
    }
    *state = prev;
}

/* YIN cumulative-mean-normalised difference. Returns harmonicity, writes f0. */
static float yin(const float *x, float *f0_out)
{
    static float cmnd[DET_TAU_MAX + 1];
    float running = 0.0f;
    cmnd[0] = 1.0f;

    /* x[j + tau] must stay inside the frame: tau <= DET_FRAME - DET_WIN */
    for (int tau = 1; tau <= DET_TAU_MAX; tau++) {
        float s = 0.0f;
        const float *a = x;
        const float *b = x + tau;
        for (int j = 0; j < DET_WIN; j++) {
            float diff = a[j] - b[j];
            s += diff * diff;
        }
        running += s;
        cmnd[tau] = (running > 0.0f) ? s * (float)tau / running : 1.0f;
    }

    int best = -1;
    for (int tau = DET_TAU_MIN; tau <= DET_TAU_MAX; tau++) {
        if (cmnd[tau] < YIN_THRESHOLD) {
            while (tau + 1 <= DET_TAU_MAX && cmnd[tau + 1] < cmnd[tau]) {
                tau++;
            }
            best = tau;
            break;
        }
    }
    if (best < 0) {
        /* no dip under threshold: take the global minimum */
        best = DET_TAU_MIN;
        for (int tau = DET_TAU_MIN + 1; tau <= DET_TAU_MAX; tau++) {
            if (cmnd[tau] < cmnd[best]) {
                best = tau;
            }
        }
    }

    /* parabolic interpolation for sub-sample period */
    float tau_f = (float)best;
    if (best > 1 && best < DET_TAU_MAX) {
        float s0 = cmnd[best - 1], s1 = cmnd[best], s2 = cmnd[best + 1];
        float den = s0 - 2.0f * s1 + s2;
        if (fabsf(den) > 1e-9f) {
            float delta = 0.5f * (s0 - s2) / den;
            if (delta > -1.0f && delta < 1.0f) {
                tau_f += delta;
            }
        }
    }

    *f0_out = (float)DET_FS / tau_f;
    float h = 1.0f - cmnd[best];
    if (h < 0.0f) {
        h = 0.0f;
    }
    if (h > 1.0f) {
        h = 1.0f;
    }
    return h;
}

static bool same_note(float a, float b)
{
    if (a <= 0.0f || b <= 0.0f) {
        return false;
    }
    float r = a / b;
    /* accept octave jumps: YIN (and polyphonic piano) often flips octaves */
    if (r > 1.5f) {
        r *= 0.5f;
    } else if (r < 0.75f) {
        r *= 2.0f;
    }
    return fabsf(r - 1.0f) < PITCH_TOL;
}

void det_push(detector_t *d, const float *hop, det_out_t *out)
{
    /* slide window */
    memmove(d->frame, d->frame + DET_HOP, (DET_FRAME - DET_HOP) * sizeof(float));
    memcpy(d->frame + DET_FRAME - DET_HOP, hop, DET_HOP * sizeof(float));
    if (d->filled < DET_FRAME) {
        d->filled += DET_HOP;
    }

    memset(out, 0, sizeof(*out));
    if (d->filled < DET_FRAME) {
        out->floor_db = d->floor_db;
        return;
    }

    /* level (remove DC first) */
    float mean = 0.0f;
    for (int i = 0; i < DET_FRAME; i++) {
        mean += d->frame[i];
    }
    mean /= DET_FRAME;
    static float x[DET_FRAME];
    float energy = 0.0f;
    for (int i = 0; i < DET_FRAME; i++) {
        x[i] = d->frame[i] - mean;
        energy += x[i] * x[i];
    }
    float rms = sqrtf(energy / DET_FRAME);
    float denergy = 0.0f;
    for (int i = 1; i < DET_FRAME; i++) {
        float dx = x[i] - x[i - 1];
        denergy += dx * dx;
    }
    float bright_db = 10.0f * log10f((denergy + 1e-12f) / (energy + 1e-12f));
    float level_db = 20.0f * log10f(rms + 1e-9f);

    if (!d->floor_init) {
        d->floor_db = level_db < FLOOR_INIT_DB ? level_db : FLOOR_INIT_DB;
        d->floor_init = true;
    }

    bool loud = (level_db > d->floor_db + d->p.sens_db) && (level_db > ABS_MIN_DB);

    float f0 = 0.0f, harm = 0.0f;
    if (loud) {
        harm = yin(x, &f0);
    }

    bool harmonic = loud && harm >= d->p.harm_min;
    if (harmonic) {
        if (same_note(f0, d->last_f0)) {
            d->stable_run++;
        } else {
            d->stable_run = 0;
        }
        d->last_f0 = f0;
    } else {
        d->stable_run = 0;
        d->last_f0 = 0.0f;
    }
    bool musical = harmonic && d->stable_run >= d->p.stable_hops;

    /* noise floor: drops quickly, rises slowly and never during music */
    if (level_db < d->floor_db) {
        d->floor_db += FLOOR_DOWN_RATE * (level_db - d->floor_db);
    } else if (!loud) {
        d->floor_db += FLOOR_UP_DB;          /* quiet frames: track up (~1 dB/s) */
    } else if (!harmonic) {
        d->floor_db += FLOOR_UP_DB * 0.1f;   /* loud noise: drift (~0.1 dB/s);   */
    }                                        /* never rises on pitched sound, so */
                                             /* note attacks can't lift it       */
    if (d->floor_db < FLOOR_MIN_DB) {
        d->floor_db = FLOOR_MIN_DB;
    }

    /* activity window */
    d->ring_sum -= d->ring[d->ring_idx];
    d->ring[d->ring_idx] = musical ? 1 : 0;
    d->ring_sum += d->ring[d->ring_idx];
    d->ring_idx = (d->ring_idx + 1) % DET_RING;
    if (d->ring_count < DET_RING) {
        d->ring_count++;
    }

    out->level_db    = level_db;
    out->floor_db    = d->floor_db;
    out->harmonicity = harm;
    out->f0          = harmonic ? f0 : 0.0f;
    out->bright_db   = bright_db;
    out->musical     = musical;
    out->activity    = (float)d->ring_sum / (float)DET_RING;
    out->active      = out->activity >= d->p.activity_on;
}
