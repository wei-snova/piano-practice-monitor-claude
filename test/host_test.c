/*
 * Host-side test of detector + session logic with synthetic signals.
 * Build & run:  make -C test
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/detector.h"
#include "../main/session.h"

#define FS_IN 16000
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double frand(void) { return (double)rand() / RAND_MAX * 2.0 - 1.0; }

typedef enum { SIG_SILENCE, SIG_NOISE, SIG_CELLO, SIG_PIANO, SIG_SPEECH, SIG_PIANO_FAST } sig_t2;

/* generator state */
static double ph[4];
static double env[4];
static double note_t;
static int note_idx;

static const double cello_notes[] = {65.4, 98.0, 130.8, 146.8, 196.0, 220.0, 293.7, 440.0, 587.3};
static const double piano_notes[] = {130.8, 164.8, 196.0, 261.6, 329.6, 392.0, 523.3, 659.3, 880.0};

static double harmonic_tone(double *phase, double f, int nh)
{
    double v = 0;
    for (int h = 1; h <= nh; h++) {
        v += sin(*phase * h) / h;
    }
    *phase += 2 * M_PI * f / FS_IN;
    if (*phase > 2 * M_PI * 1000) {
        *phase -= 2 * M_PI * 1000;
    }
    return v;
}

static double gen(sig_t2 s, double t, double noise_amp)
{
    double v = 0;
    double dt = 1.0 / FS_IN;
    switch (s) {
    case SIG_SILENCE:
        break;
    case SIG_NOISE:
        v = 0.05 * frand();
        break;
    case SIG_CELLO: { /* sustained bowed notes, 0.8 s each, vibrato 5.5 Hz +-1 % */
        note_t += dt;
        if (note_t > 0.8) { note_t = 0; note_idx = (note_idx + 3) % 9; }
        double f = cello_notes[note_idx] * (1.0 + 0.01 * sin(2 * M_PI * 5.5 * t));
        double a = note_t < 0.08 ? note_t / 0.08 : 1.0;  /* bow attack */
        v = 0.15 * a * harmonic_tone(&ph[0], f, 8);
        break;
    }
    case SIG_PIANO:
    case SIG_PIANO_FAST: { /* 3-note chords / notes with exponential decay */
        double step = (s == SIG_PIANO) ? 0.6 : 0.15;
        note_t += dt;
        if (note_t > step || env[0] == 0) {
            note_t = 0;
            note_idx = (note_idx + 2) % 7;
            for (int k = 0; k < 3; k++) env[k] = 1.0;
        }
        for (int k = 0; k < 3; k++) {
            double f = piano_notes[note_idx + k] ;
            if (s == SIG_PIANO_FAST && k > 0) continue; /* single-note runs */
            v += 0.12 * env[k] * harmonic_tone(&ph[k], f, 6);
            env[k] *= exp(-dt / 0.8);
        }
        break;
    }
    case SIG_SPEECH: { /* voiced 150 ms gliding pitch, 80 ms fricative, pauses */
        double cyc = fmod(t, 0.5);
        if (cyc < 0.15) {
            double f = 120 + 60 * (cyc / 0.15) + 20 * sin(t * 7); /* glide */
            v = 0.15 * harmonic_tone(&ph[0], f, 10) * sin(M_PI * cyc / 0.15);
        } else if (cyc < 0.23) {
            v = 0.05 * frand();
        } else if (cyc < 0.38) {
            double c2 = cyc - 0.23;
            double f = 190 - 70 * (c2 / 0.15);
            v = 0.15 * harmonic_tone(&ph[0], f, 10) * sin(M_PI * c2 / 0.15);
        }
        break;
    }
    }
    return v + noise_amp * frand();
}

typedef struct { sig_t2 sig; double secs; } seg_t;

static const char *sname(sig_t2 s)
{
    const char *n[] = {"silence", "room noise", "cello", "piano chords", "speech", "fast piano"};
    return n[s];
}

/* returns fraction of hops marked active */
static double run_signal(sig_t2 s, double secs, double noise, double *musical_frac)
{
    detector_t d;
    det_init(&d, NULL);
    float dec_state = 0;
    int16_t in[DET_HOP * 2];
    float hop[DET_HOP];
    int hops = (int)(secs * FS_IN / (DET_HOP * 2));
    int act = 0, mus = 0, counted = 0;
    double t = 0;
    note_t = 0; note_idx = 0; memset(env, 0, sizeof(env));
    for (int h = 0; h < hops; h++) {
        for (int i = 0; i < DET_HOP * 2; i++) {
            double v = gen(s, t, noise);
            t += 1.0 / FS_IN;
            if (v > 1) v = 1;
            if (v < -1) v = -1;
            in[i] = (int16_t)(v * 32000);
        }
        det_decimate_16k(in, DET_HOP * 2, hop, &dec_state);
        det_out_t o;
        det_push(&d, hop, &o);
        if (h > hops / 3) { /* ignore warm-up */
            counted++;
            act += o.active;
            mus += o.musical;
        }
    }
    *musical_frac = (double)mus / counted;
    return (double)act / counted;
}

static int failures = 0;

static void expect(const char *what, int ok)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    srand(1);
    printf("Detector: fraction of time judged 'practicing' (30 s each, light room noise)\n");
    sig_t2 sigs[] = {SIG_SILENCE, SIG_NOISE, SIG_CELLO, SIG_PIANO, SIG_PIANO_FAST, SIG_SPEECH};
    double res[6];
    for (int i = 0; i < 6; i++) {
        double mf;
        res[i] = run_signal(sigs[i], 30, 0.002, &mf);
        printf("  %-13s active %5.1f %%   musical frames %5.1f %%\n", sname(sigs[i]), res[i] * 100, mf * 100);
    }
    expect("silence never active", res[0] < 0.01);
    expect("room noise never active", res[1] < 0.01);
    expect("cello active > 90 %", res[2] > 0.90);
    expect("piano chords active > 90 %", res[3] > 0.90);
    expect("fast piano active > 80 %", res[4] > 0.80);
    expect("speech active < 10 %", res[5] < 0.10);

    printf("\nSame, in a noisy room (fan/traffic ~ -35 dBFS)\n");
    for (int i = 2; i < 6; i++) {
        double mf;
        res[i] = run_signal(sigs[i], 30, 0.03, &mf);
        printf("  %-13s active %5.1f %%   musical frames %5.1f %%\n", sname(sigs[i]), res[i] * 100, mf * 100);
    }
    expect("noisy: cello active > 80 %", res[2] > 0.80);
    expect("noisy: piano chords active > 80 %", res[3] > 0.80);
    expect("noisy: speech active < 10 %", res[5] < 0.10);

    printf("\nSession scenario: 30s silence, 3m cello, 40s pause, 2m piano, 3m silence\n");
    seg_t plan[] = {{SIG_SILENCE, 30}, {SIG_CELLO, 180}, {SIG_SILENCE, 40}, {SIG_PIANO, 120}, {SIG_SILENCE, 180}};
    detector_t d;
    det_init(&d, NULL);
    session_t s;
    session_init(&s, NULL);
    float dec_state = 0;
    int16_t in[DET_HOP * 2];
    float hop[DET_HOP];
    double t = 0;
    int64_t now_ms = 0;
    int started = 0, ended = 0;
    int64_t total = 0, start_at = -1, end_at = -1;
    for (size_t k = 0; k < sizeof(plan) / sizeof(plan[0]); k++) {
        int hops = (int)(plan[k].secs * FS_IN / (DET_HOP * 2));
        note_t = 0; note_idx = 0; memset(env, 0, sizeof(env));
        for (int h = 0; h < hops; h++) {
            for (int i = 0; i < DET_HOP * 2; i++) {
                double v = gen(plan[k].sig, t, 0.002);
                t += 1.0 / FS_IN;
                in[i] = (int16_t)(v * 32000);
            }
            det_decimate_16k(in, DET_HOP * 2, hop, &dec_state);
            det_out_t o;
            det_push(&d, hop, &o);
            now_ms += 32;
            int64_t dur = 0;
            sess_event_t ev = session_update(&s, o.active, now_ms, &dur);
            if (ev == SEV_STARTED) { started++; start_at = s.start; }
            if (ev == SEV_ENDED) { ended++; total += dur; end_at = now_ms; }
        }
    }
    printf("  sessions started %d, ended %d, logged %.1f min, start at %.1f s, closed at %.1f s\n",
           started, ended, total / 60000.0, start_at / 1000.0, end_at / 1000.0);
    /* expected: one session from ~30 s to ~370 s => ~5.7 min, closed ~90 s after */
    expect("exactly one session", started == 1 && ended == 1);
    expect("logged 5.4-5.9 min (pause kept, trailing silence trimmed)", total > 5.4 * 60000 && total < 5.9 * 60000);
    expect("start back-dated to ~30 s", start_at > 29000 && start_at < 33000);

    printf("\nSession: short noodling (40 s) is discarded\n");
    session_init(&s, NULL);
    now_ms = 0;
    int discarded = 0;
    for (int i = 0; i < 6000; i++) {
        now_ms += 32;
        int64_t dur;
        bool active = now_ms > 1000 && now_ms < 41000;
        sess_event_t ev = session_update(&s, active, now_ms, &dur);
        if (ev == SEV_DISCARDED) discarded++;
        if (ev == SEV_ENDED) ended = 99;
    }
    expect("40 s burst discarded", discarded == 1 && ended != 99);

    printf("\nSession: instrument switch splits time\n");
    session_init(&s, NULL);
    now_ms = 0;
    int64_t part1 = 0, part2 = 0;
    for (int i = 0; i < 20000; i++) {
        now_ms += 32;
        int64_t dur = 0;
        bool active = now_ms < 400000;
        if (now_ms == 200000) part1 = session_split(&s, now_ms);
        if (session_update(&s, active, now_ms, &dur) == SEV_ENDED) part2 = dur;
    }
    printf("  part1 %.1f s, part2 %.1f s\n", part1 / 1000.0, part2 / 1000.0);
    expect("split parts add up to ~400 s", llabs(part1 + part2 - 400000) < 1000);

    printf("\n%s (%d failure%s)\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
