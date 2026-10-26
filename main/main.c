/*
 * Practice Logger - fully automatic cello/piano practice time logging
 * for the Waveshare ESP32-S3-Touch-LCD-4B.
 *
 * Tasks:
 *   audio  (core 1, prio 5): mic -> detector -> session state machine
 *                            -> cello/piano classifier
 *   store  (core 0, prio 2): NVS daily totals, CSV log, checkpoints
 *   LVGL   (BSP / esp_lvgl_port): UI, refreshed by a 200 ms LVGL timer
 */
#include <string.h>
#include <time.h>

#include "app.h"
#include "bsp/esp-bsp.h"
#include "detector.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net.h"
#include "session.h"
#include "store.h"
#include "ui.h"

static const char *TAG = "main";

#define MIC_RATE        16000
#define MIC_HOP         (DET_HOP * 2)   /* 512 samples @16 kHz = 32 ms */
#define CHECKPOINT_MS   30000

const char *const INSTR_NAMES[INSTR_COUNT] = { "cello", "piano" };

static QueueHandle_t s_cmd_q;
static portMUX_TYPE s_live_mux = portMUX_INITIALIZER_UNLOCKED;
static live_state_t s_live;

/* ---------- shared helpers ---------- */

void app_get_live(live_state_t *out)
{
    taskENTER_CRITICAL(&s_live_mux);
    *out = s_live;
    taskEXIT_CRITICAL(&s_live_mux);
}

static void publish_live(const live_state_t *in)
{
    taskENTER_CRITICAL(&s_live_mux);
    s_live = *in;
    taskEXIT_CRITICAL(&s_live_mux);
}

void app_send_cmd(app_cmd_type_t type, int value)
{
    app_cmd_t c = { .type = type, .value = value };
    xQueueSend(s_cmd_q, &c, 0);
}

bool app_clock_valid(void)
{
    return time(NULL) > 1700000000;   /* anything after Nov 2023 means SNTP ran */
}

uint32_t app_date_of(time_t t)
{
    if (t < 1700000000) {
        return 0;
    }
    struct tm tm;
    localtime_r(&t, &tm);
    return (uint32_t)((tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
}

uint32_t app_date_minus(uint32_t date, int days_back)
{
    struct tm tm = {
        .tm_year = (int)(date / 10000) - 1900,
        .tm_mon = (int)(date / 100 % 100) - 1,
        .tm_mday = (int)(date % 100) - days_back,
        .tm_hour = 12,    /* noon avoids DST edge cases */
        .tm_isdst = -1,
    };
    mktime(&tm);
    return (uint32_t)((tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
}

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* wall-clock time of a monotonic timestamp (uses the clock as it is *now*,
 * so a session that started before SNTP synced still gets the right date) */
static time_t wall_of(int64_t mono_ms)
{
    return time(NULL) - (time_t)((now_ms() - mono_ms) / 1000);
}

/* ---------- instrument labelling & learning ---------- */

/* The session that just ended stays correctable for a while; what the model
 * learns from it is applied when that window closes (or the next session
 * starts), so a correction is learned instead of the mistake. */
typedef struct {
    bool valid;
    int64_t end_ms;
    instrument_t label;
    float confidence;        /* of the automatic decision */
    bool by_user;            /* set during the session or corrected afterwards */
    bool confirmed;          /* user tapped the instrument it already had */
    int hour;
    uint32_t secs;
    float feat[CLS_NFEAT];
    bool fvalid[CLS_NFEAT];
} pending_t;

static void apply_learning(pending_t *p, cls_model_t *model)
{
    if (!p->valid) {
        return;
    }
    p->valid = false;

    float rate;
    if (p->by_user) {
        rate = 0.3f;                        /* corrections teach the most */
    } else if (p->confirmed) {
        rate = 0.2f;
    } else if (p->confidence >= 0.95f) {
        rate = 0.1f;                        /* gentle self-training on clear cases */
    } else {
        rate = 0.0f;                        /* unsure and unconfirmed: learn nothing */
    }
    bool trust_time = p->by_user || p->confirmed || p->confidence >= 0.8f;
    if (rate <= 0.0f && !trust_time) {
        return;
    }
    cls_learn(model, rate > 0 ? p->feat : NULL, rate > 0 ? p->fvalid : NULL, p->label,
              trust_time ? p->hour : -1, rate);
    store_save_model(model);
    ESP_LOGI(TAG, "model updated from %s session (rate %.2f%s)", INSTR_NAMES[p->label], rate,
             p->by_user ? ", user label" : "");
}

/* ---------- audio task ---------- */

static void audio_task(void *arg)
{
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    if (!mic) {
        ESP_LOGE(TAG, "microphone init failed");
        vTaskDelete(NULL);
        return;
    }
    esp_codec_dev_sample_info_t fmt = {
        .sample_rate = MIC_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(mic, &fmt) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        vTaskDelete(NULL);
        return;
    }
    esp_codec_dev_set_in_gain(mic, (float)CONFIG_PL_MIC_GAIN_DB);

    det_params_t dp;
    det_default_params(&dp);
    dp.sens_db = (float)store_get_setting("sens", 15);
    static detector_t det;
    det_init(&det, &dp);

    sess_params_t sp = {
        .start_hold_ms = CONFIG_PL_START_HOLD_S * 1000LL,
        .candidate_gap_ms = 3000,
        .stop_gap_ms = CONFIG_PL_STOP_GAP_S * 1000LL,
        .min_session_ms = CONFIG_PL_MIN_SESSION_S * 1000LL,
    };
    session_t sess;
    session_init(&sess, &sp);

    static cls_model_t model;
    store_load_model(&model);
    static cls_session_t cls;
    cls_reset(&cls);
    static pending_t pending;

    int session_hour = -1;
    bool override_set = false;
    instrument_t override_instr = INSTR_CELLO;
    instrument_t label = INSTR_CELLO;
    float confidence = 0.5f;

    static int16_t pcm[MIC_HOP];
    static float hop[DET_HOP];
    float dec_state = 0;
    int64_t last_ckpt = 0;
    int log_div = 0;
    const int64_t correct_window_ms = CONFIG_PL_CORRECT_WINDOW_MIN * 60 * 1000LL;

    ESP_LOGI(TAG, "listening (gain %d dB, sensitivity %.0f dB)", CONFIG_PL_MIC_GAIN_DB, dp.sens_db);

    for (;;) {
        if (esp_codec_dev_read(mic, pcm, sizeof(pcm)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        det_decimate_16k(pcm, MIC_HOP, hop, &dec_state);
        det_out_t o;
        det_push(&det, hop, &o);
        int64_t t = now_ms();

        /* current label of the running session */
        if (override_set) {
            label = override_instr;
            confidence = 1.0f;
        } else {
            label = cls_decide(&cls, &model, session_hour, &confidence);
        }

        /* ---- UI commands ---- */
        app_cmd_t c;
        bool force_end = false;
        while (xQueueReceive(s_cmd_q, &c, 0) == pdTRUE) {
            switch (c.type) {
            case CMD_SET_INSTRUMENT:
                if (c.value < 0 || c.value >= INSTR_COUNT) {
                    break;
                }
                if (sess.state != SS_IDLE) {
                    override_set = true;                 /* "this one is piano" */
                    override_instr = (instrument_t)c.value;
                    label = override_instr;
                } else if (pending.valid) {
                    if ((instrument_t)c.value != pending.label) {
                        store_relabel_last((instrument_t)c.value);   /* fix the last session */
                        pending.label = (instrument_t)c.value;
                        pending.by_user = true;
                    } else {
                        pending.confirmed = true;
                    }
                }
                break;
            case CMD_END_SESSION:
                force_end = true;
                break;
            case CMD_SET_SENSITIVITY:
                det_set_sensitivity(&det, (float)c.value);
                store_set_setting("sens", c.value);
                break;
            }
        }

        /* ---- session state machine ---- */
        sess_state_t before = sess.state;
        int64_t dur = 0;
        time_t start_wall = wall_of(sess.start);
        sess_event_t ev = force_end ? session_force_end(&sess, &dur)
                                    : session_update(&sess, o.active, t, &dur);

        if (before == SS_IDLE && sess.state == SS_CANDIDATE) {
            cls_reset(&cls);                              /* fresh evidence per session */
            override_set = false;
        }
        if (sess.state != SS_IDLE) {
            cls_push(&cls, &model, &o);
        }

        switch (ev) {
        case SEV_STARTED: {
            apply_learning(&pending, &model);             /* previous session is now final */
            if (app_clock_valid()) {
                time_t sw = wall_of(sess.start);
                struct tm tm;
                localtime_r(&sw, &tm);
                session_hour = tm.tm_hour;
            } else {
                session_hour = -1;
            }
            last_ckpt = t;
            ESP_LOGI(TAG, "session started");
            break;
        }
        case SEV_ENDED: {
            instrument_t final_label = override_set ? override_instr
                                              : cls_decide(&cls, &model, session_hour, &confidence);
            ESP_LOGI(TAG, "session ended: %.1f min of %s (%s, %d notes, confidence %.2f)", dur / 60000.0,
                     INSTR_NAMES[final_label], override_set ? "set by user" : "auto", cls.n_notes, confidence);
            store_commit_session(start_wall, (uint32_t)(dur / 1000), final_label);
            pending = (pending_t){
                .valid = true, .end_ms = t, .label = final_label, .confidence = confidence,
                .by_user = override_set, .hour = session_hour, .secs = (uint32_t)(dur / 1000),
            };
            cls_session_features(&cls, pending.feat, pending.fvalid);
            override_set = false;
            break;
        }
        case SEV_DISCARDED:
            ESP_LOGI(TAG, "too short, discarded");
            store_clear_checkpoint();
            override_set = false;
            break;
        default:
            break;
        }

        if (pending.valid && sess.state == SS_IDLE && t - pending.end_ms > correct_window_ms) {
            apply_learning(&pending, &model);
        }
        if (sess.state == SS_PRACTICING && t - last_ckpt >= CHECKPOINT_MS) {
            last_ckpt = t;
            store_checkpoint(wall_of(sess.start), (uint32_t)((sess.last_active - sess.start) / 1000), label);
        }

        live_state_t L = {
            .state = sess.state,
            .now_ms = t,
            .start_ms = sess.start,
            .last_active_ms = sess.last_active,
            .instrument = label,
            .confidence = confidence,
            .n_notes = cls.n_notes,
            .set_by_user = override_set,
            .last_valid = pending.valid,
            .last_instrument = pending.label,
            .last_secs = pending.secs,
            .last_corrected = pending.by_user,
            .level_db = o.level_db,
            .floor_db = o.floor_db,
            .sens_db = det.p.sens_db,
            .activity = o.activity,
            .active = o.active,
        };
        publish_live(&L);

        if (++log_div >= 31 * 5) {   /* every ~5 s, handy for tuning over serial */
            log_div = 0;
            ESP_LOGD(TAG, "level %.1f floor %.1f harm %.2f f0 %.0f act %.2f state %d notes %d p(piano) %.2f",
                     o.level_db, o.floor_db, o.harmonicity, o.f0, o.activity, sess.state, cls.n_notes,
                     cls_p_piano(&cls, &model, session_hour));
        }
    }
}

/* ---------- entry ---------- */

void app_main(void)
{
    ESP_ERROR_CHECK(store_init());
    s_cmd_q = xQueueCreate(8, sizeof(app_cmd_t));

    net_start();

    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "display init failed");
    }
    bsp_display_backlight_on();

    live_state_t init = { .state = SS_IDLE, .sens_db = (float)store_get_setting("sens", 15) };
    publish_live(&init);

    bsp_display_lock(0);
    ui_init(init.sens_db);
    bsp_display_unlock();

    xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 5, NULL, 1);
}
