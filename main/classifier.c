#include "classifier.h"

#include <math.h>
#include <string.h>

#define HOP_S            ((float)DET_HOP / (float)DET_FS)   /* 0.032 s */
#define MIN_NOTE_HOPS    5          /* ~160 ms of one pitch to count as a note */
#define EDGE_HOPS        2          /* skip transition hops at both ends of a note */
#define PITCH_TOL        0.045f     /* same note: within 4.5 % of its start pitch */
#define BRIGHT_MARGIN_DB 12.0f      /* brightness only trusted this far above the noise */
#define VIB_THRESHOLD    7.0f       /* cents: below this, vibrato says nothing */
#define VIB_SPAN         6.0f       /* cents above threshold for full cello evidence */
#define VIB_MAX_VOTE     1.5f
#define RESTRIKE_DB      2.5f       /* a jump this big inside a "note" means it is really several */
#define ATTACK_THRESHOLD 5.0f       /* dB: below this, attack says nothing */
#define ATTACK_SPAN      5.0f
#define ATTACK_MAX_VOTE  1.0f
#define NOTE_VOTE_CAP    1.5f       /* one note can't decide a session alone */
#define AUDIO_WEIGHT     0.5f
#define TIME_PRIOR_CAP   1.5f
#define MIN_VALID_NOTES  5          /* per feature, for the session mean to be trusted */

/* typical spread, weight, and clamp range of each feature.
 *                                       slope   darken  attack  pitch  vibrato */
static const float SCALE[CLS_NFEAT]  = { 6.0f,   3.0f,   4.0f,   1.0f,  1.0f  };
static const float WEIGHT[CLS_NFEAT] = { 1.0f,   1.0f,   0.0f,   0.15f, 0.0f  };  /* attack, vibrato: own rules */
static const float LO[CLS_NFEAT]     = { -40.0f, -60.0f, 0.0f,   5.5f,  0.0f  };
static const float HI[CLS_NFEAT]     = { 15.0f,  20.0f,  30.0f,  10.5f, 40.0f };

void cls_model_default(cls_model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->magic = CLS_MODEL_MAGIC;
    /*                          slope   darken  attack  log2 f0  vibrato
     *                          dB/s    dB/s    dB               cents   */
    const float cello[CLS_NFEAT] = {  0.0f,   0.0f,   2.0f,  7.4f,   10.0f };  /* ~170 Hz */
    const float piano[CLS_NFEAT] = { -12.0f,  -4.0f,  8.0f,  8.2f,    1.0f };  /* ~290 Hz */
    memcpy(m->centroid[INSTR_CELLO], cello, sizeof(cello));
    memcpy(m->centroid[INSTR_PIANO], piano, sizeof(piano));
}

bool cls_model_valid(const cls_model_t *m)
{
    if (m->magic != CLS_MODEL_MAGIC) {
        return false;
    }
    for (int k = 0; k < INSTR_COUNT; k++) {
        for (int f = 0; f < CLS_NFEAT; f++) {
            float v = m->centroid[k][f];
            if (!(v >= LO[f] && v <= HI[f])) {   /* also rejects NaN */
                return false;
            }
        }
    }
    return true;
}

void cls_reset(cls_session_t *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 3; i++) {
        s->pre_lv[i] = -90.0f;
    }
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* least-squares slope of y[a..b) per index */
static float slope_of(const float *y, int a, int b)
{
    int n = b - a;
    if (n < 3) {
        return 0.0f;
    }
    float mx = 0, my = 0;
    for (int i = a; i < b; i++) {
        mx += (float)i;
        my += y[i];
    }
    mx /= n;
    my /= n;
    float sxy = 0, sxx = 0;
    for (int i = a; i < b; i++) {
        sxy += ((float)i - mx) * (y[i] - my);
        sxx += ((float)i - mx) * ((float)i - mx);
    }
    return sxx > 0 ? sxy / sxx : 0.0f;
}

static float median(const float *v, int n)
{
    float t[CLS_NOTE_MAX];
    memcpy(t, v, n * sizeof(float));
    for (int i = 1; i < n; i++) {
        float x = t[i];
        int j = i - 1;
        while (j >= 0 && t[j] > x) {
            t[j + 1] = t[j];
            j--;
        }
        t[j + 1] = x;
    }
    return (n & 1) ? t[n / 2] : 0.5f * (t[n / 2 - 1] + t[n / 2]);
}

/*
 * Periodic vibrato depth in cents. Detrended pitch deviation r[] is RMS-scaled
 * by how strongly it anti-correlates with itself half a vibrato period later
 * (lags 2-4 hops = 4-8 Hz). Random jitter has ~0 correlation there.
 */
static float vibrato_cents(const float *f0, int a, int b)
{
    int n = b - a;
    float med = median(f0 + a, n);
    float c[CLS_NOTE_MAX];
    for (int i = 0; i < n; i++) {
        c[i] = 1200.0f * log2f(f0[a + i] / med);
    }
    float sl = slope_of(c, 0, n);
    float mean = 0;
    for (int i = 0; i < n; i++) {
        mean += c[i];
    }
    mean /= n;
    float var = 0;
    for (int i = 0; i < n; i++) {
        c[i] -= mean + sl * ((float)i - (n - 1) * 0.5f);
        var += c[i] * c[i];
    }
    if (var <= 1e-6f) {
        return 0.0f;
    }
    float rms = sqrtf(var / n);
    float rmin = 1.0f;
    for (int lag = 2; lag <= 4; lag++) {
        float acc = 0;
        for (int i = 0; i + lag < n; i++) {
            acc += c[i] * c[i + lag];
        }
        float r = acc / var;
        if (r < rmin) {
            rmin = r;
        }
    }
    /* real vibrato is very regular (r near -1); beating between overlapping
     * piano notes and pitch-tracker noise are much less so */
    return rms * clampf((-rmin - 0.3f) / 0.4f, 0.0f, 1.0f);
}

float cls_note_logit(const cls_model_t *m, const float feat[CLS_NFEAT], const bool valid[CLS_NFEAT])
{
    float d[INSTR_COUNT] = { 0 };
    for (int k = 0; k < INSTR_COUNT; k++) {
        for (int f = 0; f < CLS_NFEAT; f++) {
            if (!valid[f]) {
                continue;
            }
            float z = (feat[f] - m->centroid[k][f]) / SCALE[f];
            d[k] += WEIGHT[f] * z * z;
        }
    }
    float logit = 0.5f * (d[INSTR_CELLO] - d[INSTR_PIANO]);
    if (valid[F_VIBRATO]) {
        /* vibrato only ever argues for cello (plenty of cello is played without it) */
        logit -= VIB_MAX_VOTE * clampf((feat[F_VIBRATO] - VIB_THRESHOLD) / VIB_SPAN, 0.0f, 1.0f);
    }
    if (valid[F_ATTACK]) {
        /* a hammer-like jump argues for piano; a soft start proves nothing
         * (fast legato piano barely rises over the still-ringing previous note) */
        logit += ATTACK_MAX_VOTE * clampf((feat[F_ATTACK] - ATTACK_THRESHOLD) / ATTACK_SPAN, 0.0f, 1.0f);
    }
    return clampf(logit, -NOTE_VOTE_CAP, NOTE_VOTE_CAP);
}

static void finish_note(cls_session_t *s, const cls_model_t *m)
{
    int n = s->len;
    if (n < MIN_NOTE_HOPS) {
        return;
    }
    float feat[CLS_NFEAT] = { 0 };
    bool valid[CLS_NFEAT] = { 0 };

    /* peak among the first hops; the body of the note runs to n - EDGE_HOPS */
    int peak = 0;
    for (int i = 1; i < n && i < 4; i++) {
        if (s->lv[i] > s->lv[peak]) {
            peak = i;
        }
    }
    int body_end = n - EDGE_HOPS;

    /* A sudden jump in level partway through means the pitch tracker merged
     * several notes (e.g. fast piano scales locking onto a common period).
     * Its within-note measurements would be meaningless, so skip them. */
    bool restruck = false;
    for (int i = peak + 1; i < n; i++) {
        if (s->lv[i] - s->lv[i - 1] > RESTRIKE_DB) {
            restruck = true;
            break;
        }
    }

    if (!restruck && body_end - peak >= 5) {
        feat[F_SLOPE] = clampf(slope_of(s->lv, peak, body_end) / HOP_S, LO[F_SLOPE], HI[F_SLOPE]);
        valid[F_SLOPE] = true;

        /* brightness trend over the hops that are well above the noise */
        float bx[CLS_NOTE_MAX];
        int nb = 0, first = -1;
        for (int i = peak; i < body_end; i++) {
            if (s->br_ok[i]) {
                if (first < 0) {
                    first = i;
                }
                bx[nb++] = s->br[i];
            }
        }
        if (nb >= 5 && nb >= (body_end - peak) * 2 / 3) {
            feat[F_DARKEN] = clampf(slope_of(bx, 0, nb) / HOP_S, LO[F_DARKEN], HI[F_DARKEN]);
            valid[F_DARKEN] = true;
        }
    }

    if (!restruck && body_end - EDGE_HOPS >= 8) {
        feat[F_VIBRATO] = clampf(vibrato_cents(s->f0, EDGE_HOPS, body_end), LO[F_VIBRATO], HI[F_VIBRATO]);
        valid[F_VIBRATO] = true;
    }

    feat[F_ATTACK] = clampf(s->note_attack, LO[F_ATTACK], HI[F_ATTACK]);
    valid[F_ATTACK] = true;
    feat[F_PITCH] = clampf(log2f(median(s->f0, n)), LO[F_PITCH], HI[F_PITCH]);
    valid[F_PITCH] = true;

    /* short notes are measured poorly: let long notes carry the decision */
    float w = clampf((float)(n - 4) / 12.0f, 0.25f, 1.0f);
    s->logit_audio += w * cls_note_logit(m, feat, valid);
    for (int f = 0; f < CLS_NFEAT; f++) {
        if (valid[f]) {
            s->feat_sum[f] += feat[f];
            s->feat_n[f]++;
        }
    }
    s->n_notes++;
}

static float pre_min(const cls_session_t *s)
{
    float v = s->pre_lv[0];
    for (int i = 1; i < 3; i++) {
        if (s->pre_lv[i] < v) {
            v = s->pre_lv[i];
        }
    }
    return v;
}

void cls_push(cls_session_t *s, const cls_model_t *m, const det_out_t *o)
{
    bool pitched = o->f0 > 0.0f;
    float f0 = o->f0;

    if (pitched && s->len > 0) {
        /* fold octave errors back onto the note's pitch */
        float r = f0 / s->ref_f0;
        if (r > 1.5f) {
            f0 *= 0.5f;
        } else if (r < 0.75f) {
            f0 *= 2.0f;
        }
        if (fabsf(f0 / s->ref_f0 - 1.0f) > PITCH_TOL) {
            /* pitch moved: previous note ended, a new one starts here */
            finish_note(s, m);
            s->len = 0;
            f0 = o->f0;
        }
    }

    if (pitched) {
        if (s->len == 0) {
            s->ref_f0 = f0;
            s->lv[0] = o->level_db;
            s->f0[0] = f0;
            s->br[0] = o->bright_db;
            s->br_ok[0] = o->level_db > o->floor_db + BRIGHT_MARGIN_DB;
            s->len = 1;
            s->note_base = pre_min(s);
            s->note_attack = o->level_db - s->note_base;
        } else if (s->len < CLS_NOTE_MAX) {
            s->lv[s->len] = o->level_db;
            s->f0[s->len] = f0;
            s->br[s->len] = o->bright_db;
            s->br_ok[s->len] = o->level_db > o->floor_db + BRIGHT_MARGIN_DB;
            s->len++;
            if (s->len <= 3) {   /* attack = rise to the peak of the first 3 hops */
                float a = o->level_db - s->note_base;
                if (a > s->note_attack) {
                    s->note_attack = a;
                }
            }
        } else {
            /* very long note: score what we have and continue as a new segment */
            finish_note(s, m);
            s->len = 0;
        }
    } else if (s->len > 0) {
        finish_note(s, m);
        s->len = 0;
    }

    /* recent level history (includes the tail of the previous note, so the
     * attack of the next note is measured against what was sounding just before) */
    s->pre_lv[s->pre_idx] = o->level_db;
    s->pre_idx = (s->pre_idx + 1) % 3;
}

static float time_logit(const cls_model_t *m, int hour)
{
    if (hour < 0 || hour > 23) {
        return 0.0f;
    }
    float p = (float)m->hours[INSTR_PIANO][hour] + 1.0f;
    float c = (float)m->hours[INSTR_CELLO][hour] + 1.0f;
    return clampf(logf(p / c), -TIME_PRIOR_CAP, TIME_PRIOR_CAP);
}

float cls_p_piano(const cls_session_t *s, const cls_model_t *m, int hour)
{
    float logit = AUDIO_WEIGHT * s->logit_audio + time_logit(m, hour);
    return 1.0f / (1.0f + expf(-logit));
}

instrument_t cls_decide(const cls_session_t *s, const cls_model_t *m, int hour, float *confidence)
{
    float p = cls_p_piano(s, m, hour);
    instrument_t k = p >= 0.5f ? INSTR_PIANO : INSTR_CELLO;
    if (confidence) {
        *confidence = k == INSTR_PIANO ? p : 1.0f - p;
    }
    return k;
}

bool cls_session_features(const cls_session_t *s, float out[CLS_NFEAT], bool valid[CLS_NFEAT])
{
    bool any = false;
    for (int f = 0; f < CLS_NFEAT; f++) {
        valid[f] = s->feat_n[f] >= MIN_VALID_NOTES;
        out[f] = s->feat_n[f] ? s->feat_sum[f] / (float)s->feat_n[f] : 0.0f;
        any |= valid[f];
    }
    return any;
}

void cls_learn(cls_model_t *m, const float feat[CLS_NFEAT], const bool valid[CLS_NFEAT],
               instrument_t label, int hour, float rate)
{
    if (label >= INSTR_COUNT) {
        return;
    }
    if (feat && valid) {
        for (int f = 0; f < CLS_NFEAT; f++) {
            if (valid[f]) {
                float v = clampf(feat[f], LO[f], HI[f]);
                m->centroid[label][f] += rate * (v - m->centroid[label][f]);
            }
        }
    }
    if (hour >= 0 && hour < 24) {
        if (m->hours[label][hour] >= 1000) {
            for (int k = 0; k < INSTR_COUNT; k++) {
                for (int h = 0; h < 24; h++) {
                    m->hours[k][h] /= 2;
                }
            }
        }
        m->hours[label][hour]++;
    }
}
