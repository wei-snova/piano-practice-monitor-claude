#include "session.h"

#include <string.h>

void session_default_params(sess_params_t *p)
{
    p->start_hold_ms    = 5 * 1000;
    p->candidate_gap_ms = 3 * 1000;
    p->stop_gap_ms      = 90 * 1000;
    p->min_session_ms   = 60 * 1000;
}

void session_init(session_t *s, const sess_params_t *p)
{
    memset(s, 0, sizeof(*s));
    if (p) {
        s->p = *p;
    } else {
        session_default_params(&s->p);
    }
    s->state = SS_IDLE;
}

static sess_event_t finish(session_t *s, int64_t *dur_ms)
{
    int64_t dur = s->last_active - s->start;
    s->state = SS_IDLE;
    if (dur >= s->p.min_session_ms) {
        if (dur_ms) {
            *dur_ms = dur;
        }
        return SEV_ENDED;
    }
    if (dur_ms) {
        *dur_ms = 0;
    }
    return SEV_DISCARDED;
}

sess_event_t session_update(session_t *s, bool active, int64_t now_ms, int64_t *dur_ms)
{
    switch (s->state) {
    case SS_IDLE:
        if (active) {
            s->state = SS_CANDIDATE;
            s->cand_start = now_ms;
            s->cand_last = now_ms;
        }
        break;

    case SS_CANDIDATE:
        if (active) {
            s->cand_last = now_ms;
            if (now_ms - s->cand_start >= s->p.start_hold_ms) {
                s->state = SS_PRACTICING;
                s->start = s->cand_start;
                s->last_active = now_ms;
                return SEV_STARTED;
            }
        } else if (now_ms - s->cand_last > s->p.candidate_gap_ms) {
            s->state = SS_IDLE;
        }
        break;

    case SS_PRACTICING:
        if (active) {
            s->last_active = now_ms;
        } else if (now_ms - s->last_active > s->p.stop_gap_ms) {
            return finish(s, dur_ms);
        }
        break;
    }
    return SEV_NONE;
}

sess_event_t session_force_end(session_t *s, int64_t *dur_ms)
{
    if (s->state != SS_PRACTICING) {
        s->state = SS_IDLE;
        return SEV_NONE;
    }
    return finish(s, dur_ms);
}

int64_t session_split(session_t *s, int64_t now_ms)
{
    if (s->state != SS_PRACTICING) {
        return 0;
    }
    int64_t dur = s->last_active - s->start;
    s->start = now_ms;
    s->last_active = now_ms;
    return dur > 0 ? dur : 0;
}
