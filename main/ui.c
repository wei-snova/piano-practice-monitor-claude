#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "net.h"
#include "store.h"

#define COL_BG      lv_color_hex(0x111315)
#define COL_PANEL   lv_color_hex(0x1C1F23)
#define COL_TEXT    lv_color_hex(0xECEDEE)
#define COL_DIM     lv_color_hex(0x8A9096)
#define COL_CELLO   lv_color_hex(0xE0A458)
#define COL_PIANO   lv_color_hex(0x6FA8DC)
#define COL_GREEN   lv_color_hex(0x5CC27A)
#define COL_AMBER   lv_color_hex(0xE0A458)

#define PAUSE_SHOW_MS   10000   /* after this much silence, show "Paused" */
#define IDENTIFY_NOTES  8       /* notes before the detected instrument is shown as settled */

static const char *const INSTR_TITLE[INSTR_COUNT] = { "Cello", "Piano" };
#define DIM_AFTER_MS    120000  /* dim backlight after 2 min without touch/practice */

static lv_obj_t *s_clock, *s_status, *s_timer, *s_caption, *s_end_btn;
static lv_obj_t *s_btnm;
static lv_obj_t *s_today_total, *s_goal_bar, *s_today_split;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ser[INSTR_COUNT];
static lv_obj_t *s_day_lbl[7];
static lv_obj_t *s_level_bar, *s_sens_slider, *s_net_lbl;

static uint32_t s_loaded_version = UINT32_MAX;
static uint32_t s_loaded_date = UINT32_MAX;
static day_rec_t s_today;
static int s_brightness = -1;

static const char *s_instr_map[] = { "Cello", "Piano", "" };

/* ---------- helpers ---------- */

static void fmt_duration(char *buf, size_t n, int64_t ms)
{
    int s = (int)(ms / 1000);
    if (s < 0) {
        s = 0;
    }
    if (s >= 3600) {
        snprintf(buf, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    } else {
        snprintf(buf, n, "%d:%02d", s / 60, s % 60);
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

static void set_status(const char *text, lv_color_t color)
{
    lv_label_set_text(s_status, text);
    lv_obj_set_style_bg_color(s_status, color, 0);
}

static void set_brightness(int pct)
{
    if (pct != s_brightness) {
        bsp_display_brightness_set(pct);
        s_brightness = pct;
    }
}

/* ---------- events ---------- */

static void on_instrument(lv_event_t *e)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(s_btnm);
    if (id < INSTR_COUNT) {
        app_send_cmd(CMD_SET_INSTRUMENT, (int)id);
    }
}

static void on_end(lv_event_t *e)
{
    app_send_cmd(CMD_END_SESSION, 0);
}

static void on_sens(lv_event_t *e)
{
    /* slider: right = more sensitive = lower threshold above the noise floor */
    int v = lv_slider_get_value(s_sens_slider);
    app_send_cmd(CMD_SET_SENSITIVITY, 36 - v);
}

/* ---------- history ---------- */

static void reload_history(uint32_t today)
{
    static const char *wd[] = { "S", "M", "T", "W", "T", "F", "S" };
    uint32_t max_min = 30;
    uint32_t mins[7][INSTR_COUNT] = { 0 };

    for (int i = 0; i < 7; i++) {
        uint32_t d = today ? app_date_minus(today, 6 - i) : 0;
        day_rec_t r = { 0 };
        if (today || i == 6) {
            store_get_day(today ? d : 0, &r);
        }
        if (i == 6) {
            s_today = r;
        }
        for (int k = 0; k < INSTR_COUNT; k++) {
            mins[i][k] = r.secs[k] / 60;
            if (mins[i][k] > max_min) {
                max_min = mins[i][k];
            }
        }
        if (today) {
            struct tm tm = { .tm_year = (int)(d / 10000) - 1900, .tm_mon = (int)(d / 100 % 100) - 1,
                             .tm_mday = (int)(d % 100), .tm_hour = 12 };
            mktime(&tm);
            lv_label_set_text(s_day_lbl[i], wd[tm.tm_wday]);
        } else {
            lv_label_set_text(s_day_lbl[i], i == 6 ? "now" : "");
        }
        lv_obj_set_style_text_color(s_day_lbl[i], i == 6 ? COL_TEXT : COL_DIM, 0);
    }
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)max_min);
    for (int i = 0; i < 7; i++) {
        for (int k = 0; k < INSTR_COUNT; k++) {
            lv_chart_set_value_by_id(s_chart, s_ser[k], i, (int32_t)mins[i][k]);
        }
    }
    lv_chart_refresh(s_chart);
}

/* ---------- periodic refresh (runs inside the LVGL task) ---------- */

static void ui_tick(lv_timer_t *t)
{
    live_state_t L;
    app_get_live(&L);
    char buf[64];

    /* clock */
    time_t now = time(NULL);
    uint32_t today = app_date_of(now);
    if (app_clock_valid()) {
        struct tm tm;
        localtime_r(&now, &tm);
        int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
        char day[24];
        strftime(day, sizeof(day), "%a %b %d", &tm);
        snprintf(buf, sizeof(buf), "%s  %d:%02d %s", day, h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
        lv_label_set_text(s_clock, buf);
    } else {
        lv_label_set_text(s_clock, "Clock not set");
    }

    /* history (on new session, new day, or first run) */
    if (store_version() != s_loaded_version || today != s_loaded_date) {
        s_loaded_version = store_version();
        s_loaded_date = today;
        reload_history(today);
    }

    /* status + timer */
    int64_t live_ms = 0;
    if (L.state == SS_PRACTICING) {
        int64_t gap = L.now_ms - L.last_active_ms;
        if (gap < PAUSE_SHOW_MS) {
            live_ms = L.now_ms - L.start_ms;
            set_status("Practicing", COL_GREEN);
        } else {
            live_ms = L.last_active_ms - L.start_ms;
            int left = (int)((CONFIG_PL_STOP_GAP_S * 1000LL - gap) / 1000);
            snprintf(buf, sizeof(buf), "Paused - ends in %ds", left > 0 ? left : 0);
            set_status(buf, COL_AMBER);
        }
        fmt_duration(buf, sizeof(buf), live_ms);
        lv_label_set_text(s_timer, buf);
        lv_obj_set_style_text_color(s_timer, COL_TEXT, 0);
        if (L.set_by_user) {
            snprintf(buf, sizeof(buf), "%s  -  set by you", INSTR_TITLE[L.instrument]);
        } else if (L.n_notes < IDENTIFY_NOTES) {
            snprintf(buf, sizeof(buf), "%s?  -  still listening", INSTR_TITLE[L.instrument]);
        } else {
            snprintf(buf, sizeof(buf), "%s  -  detected (%d%%)", INSTR_TITLE[L.instrument],
                     (int)(L.confidence * 100.0f + 0.5f));
        }
        lv_label_set_text(s_caption, buf);
        lv_obj_set_hidden(s_end_btn, false);
    } else {
        set_status(L.state == SS_CANDIDATE ? "Hearing music..." : "Listening",
                   L.state == SS_CANDIDATE ? COL_AMBER : lv_color_hex(0x3A3F45));
        lv_label_set_text(s_timer, "0:00");
        lv_obj_set_style_text_color(s_timer, COL_DIM, 0);
        if (L.state == SS_IDLE && L.last_valid) {
            int m = (int)((L.last_secs + 30) / 60);
            if (L.last_corrected) {
                snprintf(buf, sizeof(buf), "Last: %s %d min  -  corrected", INSTR_TITLE[L.last_instrument], m);
            } else {
                snprintf(buf, sizeof(buf), "Last: %s %d min  -  wrong? tap %s", INSTR_TITLE[L.last_instrument], m,
                         INSTR_TITLE[L.last_instrument == INSTR_CELLO ? INSTR_PIANO : INSTR_CELLO]);
            }
            lv_label_set_text(s_caption, buf);
        } else {
            lv_label_set_text(s_caption, "starts automatically when you play");
        }
        lv_obj_set_hidden(s_end_btn, true);
    }

    /* instrument row = indicator: the running session's instrument, or the last
     * session's while it can still be corrected; otherwise nothing lit */
    int want = -1;
    if (L.state == SS_PRACTICING) {
        want = (int)L.instrument;
    } else if (L.state == SS_IDLE && L.last_valid) {
        want = (int)L.last_instrument;
    }
    for (int k = 0; k < INSTR_COUNT; k++) {
        bool has = lv_buttonmatrix_has_button_ctrl(s_btnm, k, LV_BUTTONMATRIX_CTRL_CHECKED);
        if (k == want && !has) {
            lv_buttonmatrix_set_button_ctrl(s_btnm, k, LV_BUTTONMATRIX_CTRL_CHECKED);
        } else if (k != want && has) {
            lv_buttonmatrix_clear_button_ctrl(s_btnm, k, LV_BUTTONMATRIX_CTRL_CHECKED);
        }
    }

    /* today (stored + running session) */
    uint32_t secs[INSTR_COUNT];
    for (int k = 0; k < INSTR_COUNT; k++) {
        secs[k] = s_today.secs[k];
    }
    if (L.state == SS_PRACTICING && live_ms > 0) {
        secs[L.instrument] += (uint32_t)(live_ms / 1000);
    }
    uint32_t total_min = (secs[INSTR_CELLO] + secs[INSTR_PIANO]) / 60;
    snprintf(buf, sizeof(buf), "%lu / %d min", (unsigned long)total_min, CONFIG_PL_DAILY_GOAL_MIN);
    lv_label_set_text(s_today_total, buf);
    lv_bar_set_value(s_goal_bar, (int32_t)(total_min > CONFIG_PL_DAILY_GOAL_MIN ? CONFIG_PL_DAILY_GOAL_MIN : total_min),
                     LV_ANIM_ON);
    lv_obj_set_style_bg_color(s_goal_bar, total_min >= CONFIG_PL_DAILY_GOAL_MIN ? COL_GREEN : COL_PIANO,
                              LV_PART_INDICATOR);
    snprintf(buf, sizeof(buf), "#E0A458 Cello# %lu min      #6FA8DC Piano# %lu min",
             (unsigned long)(secs[INSTR_CELLO] / 60), (unsigned long)(secs[INSTR_PIANO] / 60));
    lv_label_set_text(s_today_split, buf);

    /* mic level relative to the noise floor */
    float above = L.level_db - L.floor_db;
    if (above < 0) {
        above = 0;
    }
    if (above > 40) {
        above = 40;
    }
    lv_bar_set_value(s_level_bar, (int32_t)above, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_level_bar, above >= L.sens_db ? COL_GREEN : COL_DIM, LV_PART_INDICATOR);

    /* network */
    if (net_connected()) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s", net_ip());
        lv_label_set_text(s_net_lbl, buf);
    } else {
        lv_label_set_text(s_net_lbl, strlen(CONFIG_PL_WIFI_SSID) ? LV_SYMBOL_WIFI " ..." : "offline");
    }

    /* backlight: dim when idle and untouched */
    bool idle = L.state == SS_IDLE && lv_display_get_inactive_time(NULL) > DIM_AFTER_MS;
    set_brightness(idle ? 10 : 80);
}

/* ---------- build ---------- */

void ui_init(float sens_db)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);

    /* header */
    s_clock = label(scr, &lv_font_montserrat_20, COL_DIM, "");
    lv_obj_align(s_clock, LV_ALIGN_TOP_LEFT, 20, 20);

    s_status = label(scr, &lv_font_montserrat_14, COL_TEXT, "Listening");
    lv_obj_set_style_bg_opa(s_status, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_status, 12, 0);
    lv_obj_set_style_pad_hor(s_status, 12, 0);
    lv_obj_set_style_pad_ver(s_status, 5, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -20, 18);

    /* session timer */
    s_timer = label(scr, &lv_font_montserrat_48, COL_DIM, "0:00");
    lv_obj_align(s_timer, LV_ALIGN_TOP_MID, 0, 62);
    s_caption = label(scr, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_align(s_caption, LV_ALIGN_TOP_MID, 0, 122);

    s_end_btn = lv_button_create(scr);
    lv_obj_set_size(s_end_btn, 72, 36);
    lv_obj_set_style_bg_color(s_end_btn, lv_color_hex(0x3A3F45), 0);
    lv_obj_align(s_end_btn, LV_ALIGN_TOP_RIGHT, -20, 72);
    lv_obj_t *el = label(s_end_btn, &lv_font_montserrat_14, COL_TEXT, "End");
    lv_obj_center(el);
    lv_obj_add_event_cb(s_end_btn, on_end, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_end_btn, true);

    /* instrument selector */
    s_btnm = lv_buttonmatrix_create(scr);
    lv_buttonmatrix_set_map(s_btnm, s_instr_map);
    lv_buttonmatrix_set_button_ctrl_all(s_btnm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(s_btnm, true);
    lv_obj_set_size(s_btnm, 440, 64);
    lv_obj_align(s_btnm, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_bg_opa(s_btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btnm, 0, 0);
    lv_obj_set_style_pad_all(s_btnm, 0, 0);
    lv_obj_set_style_pad_column(s_btnm, 12, 0);
    lv_obj_set_style_bg_color(s_btnm, COL_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_btnm, COL_DIM, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_btnm, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_btnm, 12, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_btnm, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_btnm, lv_color_hex(0x2E3A46), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_btnm, COL_TEXT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_btnm, on_instrument, LV_EVENT_VALUE_CHANGED, NULL);

    /* today panel */
    lv_obj_t *t = label(scr, &lv_font_montserrat_20, COL_TEXT, "Today");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 20, 234);
    s_today_total = label(scr, &lv_font_montserrat_20, COL_TEXT, "");
    lv_obj_align(s_today_total, LV_ALIGN_TOP_RIGHT, -20, 234);

    s_goal_bar = lv_bar_create(scr);
    lv_obj_set_size(s_goal_bar, 440, 10);
    lv_obj_align(s_goal_bar, LV_ALIGN_TOP_MID, 0, 266);
    lv_bar_set_range(s_goal_bar, 0, CONFIG_PL_DAILY_GOAL_MIN);
    lv_obj_set_style_bg_color(s_goal_bar, COL_PANEL, LV_PART_MAIN);

    s_today_split = label(scr, &lv_font_montserrat_14, COL_DIM, "");
    lv_label_set_recolor(s_today_split, true);   /* doubles as the chart legend */
    lv_obj_align(s_today_split, LV_ALIGN_TOP_LEFT, 20, 284);

    /* last 7 days */
    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, 440, 96);
    lv_obj_align(s_chart, LV_ALIGN_TOP_MID, 0, 314);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_chart, 7);
    lv_chart_set_div_line_count(s_chart, 0, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    lv_obj_set_style_pad_column(s_chart, 20, LV_PART_MAIN);  /* gap between days    */
    lv_obj_set_style_pad_column(s_chart, 3, LV_PART_ITEMS);  /* gap cello <-> piano */
    lv_obj_set_style_radius(s_chart, 3, LV_PART_ITEMS);
    s_ser[INSTR_CELLO] = lv_chart_add_series(s_chart, COL_CELLO, LV_CHART_AXIS_PRIMARY_Y);
    s_ser[INSTR_PIANO] = lv_chart_add_series(s_chart, COL_PIANO, LV_CHART_AXIS_PRIMARY_Y);

    for (int i = 0; i < 7; i++) {
        s_day_lbl[i] = label(scr, &lv_font_montserrat_14, COL_DIM, "");
        /* center each label under its bar group (440 px / 7 groups) */
        lv_obj_set_width(s_day_lbl[i], 440 / 7);
        lv_obj_set_style_text_align(s_day_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_day_lbl[i], LV_ALIGN_TOP_LEFT, 20 + i * 440 / 7, 414);
    }

    /* footer: mic level, sensitivity, network */
    lv_obj_t *m = label(scr, &lv_font_montserrat_14, COL_DIM, LV_SYMBOL_AUDIO);
    lv_obj_align(m, LV_ALIGN_BOTTOM_LEFT, 20, -18);
    s_level_bar = lv_bar_create(scr);
    lv_obj_set_size(s_level_bar, 90, 8);
    lv_bar_set_range(s_level_bar, 0, 40);
    lv_obj_set_style_bg_color(s_level_bar, COL_PANEL, LV_PART_MAIN);
    lv_obj_align(s_level_bar, LV_ALIGN_BOTTOM_LEFT, 42, -22);

    lv_obj_t *sl = label(scr, &lv_font_montserrat_14, COL_DIM, "Sens.");
    lv_obj_align(sl, LV_ALIGN_BOTTOM_LEFT, 148, -18);
    s_sens_slider = lv_slider_create(scr);
    lv_obj_set_size(s_sens_slider, 110, 8);
    lv_slider_set_range(s_sens_slider, 6, 30);
    lv_slider_set_value(s_sens_slider, 36 - (int)sens_db, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s_sens_slider, 16);
    lv_obj_align(s_sens_slider, LV_ALIGN_BOTTOM_LEFT, 200, -22);
    lv_obj_add_event_cb(s_sens_slider, on_sens, LV_EVENT_RELEASED, NULL);

    s_net_lbl = label(scr, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_align(s_net_lbl, LV_ALIGN_BOTTOM_RIGHT, -20, -18);

    lv_timer_create(ui_tick, 200, NULL);
}
