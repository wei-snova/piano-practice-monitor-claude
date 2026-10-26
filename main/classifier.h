/*
 * classifier.h - automatic cello vs piano recognition (pure C, host-testable).
 *
 * Works on the detector's per-hop output. It cuts the audio into notes
 * (runs of pitched frames at one pitch) and measures, per note:
 *
 *   slope    level change during the note, dB/s.
 *            Piano strings decay as soon as they are struck (clearly negative);
 *            a bowed cello note holds or swells (around zero).
 *   darken   brightness change during the note, dB/s.
 *            A piano's upper partials die away faster than its fundamental, so
 *            every note gets darker as it rings, sustain pedal or not. A bow
 *            keeps feeding all partials, so a cello note keeps its colour.
 *   attack   level jump at the start of the note over what was sounding just
 *            before, dB. One-sided: a hammer-like jump is evidence for piano,
 *            but a soft start proves nothing (fast legato piano barely rises).
 *   pitch    log2(f0). Weak hint only: the ranges overlap a lot.
 *   vibrato  periodic 4-7 Hz pitch wobble, cents. One-sided: vibrato is strong
 *            evidence for cello, but its absence proves nothing (scales and
 *            etudes are often played without it), so it never votes for piano.
 *
 * Features that a short note cannot measure reliably are left out of that
 * note's vote.
 *
 * Each note votes for the closer instrument profile, the votes add up over
 * the session, and a weak time-of-day prior (when you usually practise each
 * instrument) breaks ties. Because you never play both at once, the whole
 * session gets one label.
 *
 * The two instrument profiles start from physical defaults and adapt to your
 * own instruments and room after confident or user-corrected sessions.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "detector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INSTR_CELLO = 0,
    INSTR_PIANO,
    INSTR_COUNT,
} instrument_t;

enum { F_SLOPE = 0, F_DARKEN, F_ATTACK, F_PITCH, F_VIBRATO, CLS_NFEAT };

#define CLS_MODEL_MAGIC  0x434C5333u   /* "CLS3" */
#define CLS_NOTE_MAX     48            /* hops kept per note (~1.5 s) */

/* Persistent, learned model (~140 bytes). */
typedef struct {
    uint32_t magic;
    float centroid[INSTR_COUNT][CLS_NFEAT];
    uint16_t hours[INSTR_COUNT][24];   /* sessions per hour of day */
} cls_model_t;

/* Per-session evidence. */
typedef struct {
    /* note being tracked */
    int   len;
    float lv[CLS_NOTE_MAX];
    float f0[CLS_NOTE_MAX];
    float br[CLS_NOTE_MAX];
    bool  br_ok[CLS_NOTE_MAX];         /* far enough above the noise to trust br */
    float ref_f0;
    float pre_lv[3];
    int   pre_idx;
    float note_base;                   /* level just before the note started */
    float note_attack;
    /* accumulated */
    int   n_notes;
    float logit_audio;                 /* > 0 favours piano */
    float feat_sum[CLS_NFEAT];
    int   feat_n[CLS_NFEAT];
} cls_session_t;

void cls_model_default(cls_model_t *m);
bool cls_model_valid(const cls_model_t *m);

void cls_reset(cls_session_t *s);

/* Feed every detector hop (including silent ones). */
void cls_push(cls_session_t *s, const cls_model_t *m, const det_out_t *o);

/* Probability the session is piano. hour = local hour 0..23, or -1 if unknown. */
float cls_p_piano(const cls_session_t *s, const cls_model_t *m, int hour);

/* Most likely instrument and its confidence (0.5..1). */
instrument_t cls_decide(const cls_session_t *s, const cls_model_t *m, int hour, float *confidence);

/* Mean feature vector of the session's notes. valid[f] is set for features
 * measured on enough notes to be trusted. Returns false if nothing usable. */
bool cls_session_features(const cls_session_t *s, float out[CLS_NFEAT], bool valid[CLS_NFEAT]);

/* Adapt the model to a finished session with a trusted label.
 * feat/valid may be NULL (time-of-day only). */
void cls_learn(cls_model_t *m, const float feat[CLS_NFEAT], const bool valid[CLS_NFEAT],
               instrument_t label, int hour, float rate);

/* Exposed for tests: vote (> 0 piano) for one note's features. */
float cls_note_logit(const cls_model_t *m, const float feat[CLS_NFEAT], const bool valid[CLS_NFEAT]);

#ifdef __cplusplus
}
#endif
