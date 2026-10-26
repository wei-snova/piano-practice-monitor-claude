/*
 * app.h - shared types between the audio task, the storage task and the UI.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "classifier.h"   /* instrument_t */
#include "session.h"

extern const char *const INSTR_NAMES[INSTR_COUNT];

/* Snapshot published by the audio task ~30x/s, read by the UI. */
typedef struct {
    sess_state_t state;
    int64_t now_ms;
    int64_t start_ms;          /* valid when state == SS_PRACTICING */
    int64_t last_active_ms;

    /* instrument of the running session */
    instrument_t instrument;
    float confidence;          /* 0.5..1 */
    int n_notes;               /* notes heard so far this session */
    bool set_by_user;          /* tapped during the session */

    /* the session that just ended (correctable for a while) */
    bool last_valid;
    instrument_t last_instrument;
    uint32_t last_secs;
    bool last_corrected;

    float level_db;
    float floor_db;
    float sens_db;
    float activity;
    bool active;
} live_state_t;

void app_get_live(live_state_t *out);

/* Commands from the UI to the audio task. */
typedef enum {
    CMD_SET_INSTRUMENT,    /* during a session: override; after: correct the last one */
    CMD_END_SESSION,
    CMD_SET_SENSITIVITY,
} app_cmd_type_t;

typedef struct {
    app_cmd_type_t type;
    int value;
} app_cmd_t;

void app_send_cmd(app_cmd_type_t type, int value);

/* Wall clock helpers */
bool app_clock_valid(void);
/* Local calendar date as YYYYMMDD; 0 if the clock is not set. */
uint32_t app_date_of(time_t t);
/* Date `days_back` days before `date` (YYYYMMDD). */
uint32_t app_date_minus(uint32_t date, int days_back);
