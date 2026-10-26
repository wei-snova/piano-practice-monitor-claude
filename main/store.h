/*
 * store.h - persistent practice log.
 *
 * - Daily totals per instrument in NVS (one 64-bit entry per day, ~2 years kept).
 * - Every session appended to /spiffs/sessions.csv (downloadable over Wi-Fi).
 * - The in-progress session is checkpointed every 30 s, so a power cut loses
 *   at most ~30 s; it is committed on the next boot.
 * - The learned cello/piano model is kept in NVS.
 *
 * All writes go through a queue to a low-priority task so audio never blocks.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "app.h"
#include "classifier.h"
#include "esp_err.h"

typedef struct {
    uint32_t secs[INSTR_COUNT];
    uint16_t sessions;
} day_rec_t;

esp_err_t store_init(void);

/* Queue a finished session (wall-clock start, seconds). */
void store_commit_session(time_t start, uint32_t secs, instrument_t instr);
/* Move the most recently committed session to another instrument. */
void store_relabel_last(instrument_t instr);
/* Queue a checkpoint of the running session. */
void store_checkpoint(time_t start, uint32_t secs, instrument_t instr);
void store_clear_checkpoint(void);

/* Learned instrument model. load: synchronous, falls back to defaults. */
void store_load_model(cls_model_t *m);
void store_save_model(const cls_model_t *m);   /* queued */

/* Synchronous reads (safe from any task). */
bool store_get_day(uint32_t date, day_rec_t *out);
/* Changes whenever a session is committed or relabelled. */
uint32_t store_version(void);

/* Settings */
int  store_get_setting(const char *key, int def);
void store_set_setting(const char *key, int value);

#define STORE_CSV_PATH "/spiffs/sessions.csv"
