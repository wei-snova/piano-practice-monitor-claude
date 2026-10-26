/*
 * session.h - practice session state machine (pure C, host-testable).
 *
 * IDLE --(music active)--> CANDIDATE --(held start_hold_ms)--> PRACTICING
 * CANDIDATE --(no music for candidate_gap_ms)--> IDLE
 * PRACTICING --(no music for stop_gap_ms)--> IDLE, session ends
 *
 * The session start is back-dated to when the candidate began, and the
 * trailing silence is trimmed, so duration = last_active - start.
 * Sessions shorter than min_session_ms are discarded.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SS_IDLE = 0,
    SS_CANDIDATE,
    SS_PRACTICING,
} sess_state_t;

typedef enum {
    SEV_NONE = 0,
    SEV_STARTED,
    SEV_ENDED,       /* *dur_ms holds the committed duration */
    SEV_DISCARDED,   /* too short, nothing to commit          */
} sess_event_t;

typedef struct {
    int64_t start_hold_ms;
    int64_t candidate_gap_ms;
    int64_t stop_gap_ms;
    int64_t min_session_ms;
} sess_params_t;

typedef struct {
    sess_params_t p;
    sess_state_t state;
    int64_t cand_start;
    int64_t cand_last;
    int64_t start;
    int64_t last_active;
} session_t;

void session_default_params(sess_params_t *p);
void session_init(session_t *s, const sess_params_t *p);

/* Call once per detector hop. */
sess_event_t session_update(session_t *s, bool active, int64_t now_ms, int64_t *dur_ms);

/* Manual "end now". Same min-length rule as an automatic end. */
sess_event_t session_force_end(session_t *s, int64_t *dur_ms);

/* Instrument switched mid-session: returns the duration so far (to be committed
 * to the old instrument, no minimum) and restarts the session clock at now. */
int64_t session_split(session_t *s, int64_t now_ms);

#ifdef __cplusplus
}
#endif
