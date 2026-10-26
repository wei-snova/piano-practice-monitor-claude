#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "store";

#define NVS_NS        "practice"
#define KEEP_DAYS     730

typedef enum { MSG_COMMIT, MSG_RELABEL, MSG_CHECKPOINT, MSG_CLEAR_CKPT, MSG_SAVE_MODEL } msg_type_t;

typedef struct {
    msg_type_t type;
    time_t start;
    uint32_t secs;
    instrument_t instr;
    cls_model_t model;      /* MSG_SAVE_MODEL only */
} store_msg_t;

typedef struct {
    int64_t start;
    uint32_t secs;
    uint8_t instr;
} ckpt_t;

/* the most recent commit, so it can be relabelled */
typedef struct {
    bool valid;
    uint32_t date;
    time_t start;
    uint32_t secs;
    instrument_t instr;
    long csv_offset;        /* where its CSV line starts, -1 if not written */
} last_commit_t;

static nvs_handle_t s_nvs;
static QueueHandle_t s_q;
static volatile uint32_t s_version;
static bool s_spiffs_ok;
static last_commit_t s_last;

/* day record <-> u64: [0..23] cello s, [24..47] piano s, [48..63] sessions */
static uint64_t pack(const day_rec_t *r)
{
    return ((uint64_t)(r->secs[INSTR_CELLO] & 0xFFFFFF)) |
           ((uint64_t)(r->secs[INSTR_PIANO] & 0xFFFFFF) << 24) |
           ((uint64_t)r->sessions << 48);
}

static void unpack(uint64_t v, day_rec_t *r)
{
    r->secs[INSTR_CELLO] = (uint32_t)(v & 0xFFFFFF);
    r->secs[INSTR_PIANO] = (uint32_t)((v >> 24) & 0xFFFFFF);
    r->sessions = (uint16_t)(v >> 48);
}

static void day_key(uint32_t date, char key[16])
{
    snprintf(key, 16, "d%08lu", (unsigned long)date);
}

bool store_get_day(uint32_t date, day_rec_t *out)
{
    memset(out, 0, sizeof(*out));
    char key[16];
    day_key(date, key);
    uint64_t v = 0;
    if (nvs_get_u64(s_nvs, key, &v) != ESP_OK) {
        return false;
    }
    unpack(v, out);
    return true;
}

static void put_day(uint32_t date, const day_rec_t *r)
{
    char key[16];
    day_key(date, key);
    esp_err_t err = nvs_set_u64(s_nvs, key, pack(r));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u64 failed: %s", esp_err_to_name(err));
    }
}

uint32_t store_version(void)
{
    return s_version;
}

/* ---------- CSV ---------- */

static void csv_line(char *buf, size_t n, time_t start, uint32_t secs, instrument_t instr)
{
    char when[32] = "unknown,unknown";
    if (app_clock_valid() && start > 1700000000) {
        struct tm tm;
        localtime_r(&start, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d,%H:%M", &tm);
    }
    snprintf(buf, n, "%s,%s,%.1f\n", when, INSTR_NAMES[instr], secs / 60.0);
}

/* returns the offset the line was written at, or -1 */
static long append_csv(time_t start, uint32_t secs, instrument_t instr)
{
    if (!s_spiffs_ok) {
        return -1;
    }
    struct stat st;
    bool fresh = stat(STORE_CSV_PATH, &st) != 0;
    FILE *f = fopen(STORE_CSV_PATH, "a");
    if (!f) {
        ESP_LOGW(TAG, "cannot open %s", STORE_CSV_PATH);
        return -1;
    }
    if (fresh) {
        fputs("date,start,instrument,minutes\n", f);
    }
    fseek(f, 0, SEEK_END);
    long off = ftell(f);
    char line[64];
    csv_line(line, sizeof(line), start, secs, instr);
    fputs(line, f);
    fclose(f);
    return off;
}

/* rewrite the file with its last line replaced */
static void rewrite_last_csv_line(long offset, time_t start, uint32_t secs, instrument_t instr)
{
    if (!s_spiffs_ok || offset <= 0) {
        return;
    }
    char *head = heap_caps_malloc((size_t)offset, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!head) {
        head = malloc((size_t)offset);
    }
    if (!head) {
        ESP_LOGW(TAG, "no memory to rewrite CSV");
        return;
    }
    FILE *f = fopen(STORE_CSV_PATH, "r");
    size_t got = f ? fread(head, 1, (size_t)offset, f) : 0;
    if (f) {
        fclose(f);
    }
    if (got == (size_t)offset) {
        f = fopen(STORE_CSV_PATH, "w");
        if (f) {
            fwrite(head, 1, got, f);
            char line[64];
            csv_line(line, sizeof(line), start, secs, instr);
            fputs(line, f);
            fclose(f);
        }
    }
    free(head);
}

/* ---------- operations (run in the store task) ---------- */

static void do_commit(time_t start, uint32_t secs, instrument_t instr)
{
    uint32_t date = app_date_of(start);
    day_rec_t r;
    store_get_day(date, &r);
    r.secs[instr] += secs;
    r.sessions++;
    put_day(date, &r);
    if (date != 0) {
        /* prune the day that just fell out of the retention window */
        char old[16];
        day_key(app_date_minus(date, KEEP_DAYS), old);
        nvs_erase_key(s_nvs, old);
    }
    nvs_commit(s_nvs);

    s_last = (last_commit_t){
        .valid = true, .date = date, .start = start, .secs = secs, .instr = instr,
        .csv_offset = append_csv(start, secs, instr),
    };
    s_version++;
    ESP_LOGI(TAG, "logged %s %lu s on %lu (day total: cello %lu s, piano %lu s)",
             INSTR_NAMES[instr], (unsigned long)secs, (unsigned long)date,
             (unsigned long)r.secs[INSTR_CELLO], (unsigned long)r.secs[INSTR_PIANO]);
}

static void do_relabel(instrument_t to)
{
    if (!s_last.valid || s_last.instr == to) {
        return;
    }
    day_rec_t r;
    store_get_day(s_last.date, &r);
    uint32_t moved = r.secs[s_last.instr] < s_last.secs ? r.secs[s_last.instr] : s_last.secs;
    r.secs[s_last.instr] -= moved;
    r.secs[to] += moved;
    put_day(s_last.date, &r);
    nvs_commit(s_nvs);
    rewrite_last_csv_line(s_last.csv_offset, s_last.start, s_last.secs, to);
    ESP_LOGI(TAG, "relabelled last session (%lu s): %s -> %s", (unsigned long)s_last.secs,
             INSTR_NAMES[s_last.instr], INSTR_NAMES[to]);
    s_last.instr = to;
    s_version++;
}

static void store_task(void *arg)
{
    static store_msg_t m;   /* large: keep off the stack */
    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (m.type) {
        case MSG_COMMIT:
            do_commit(m.start, m.secs, m.instr);
            nvs_erase_key(s_nvs, "ckpt");
            nvs_commit(s_nvs);
            break;
        case MSG_RELABEL:
            do_relabel(m.instr);
            break;
        case MSG_CHECKPOINT: {
            ckpt_t c = { .start = m.start, .secs = m.secs, .instr = (uint8_t)m.instr };
            nvs_set_blob(s_nvs, "ckpt", &c, sizeof(c));
            nvs_commit(s_nvs);
            break;
        }
        case MSG_CLEAR_CKPT:
            nvs_erase_key(s_nvs, "ckpt");
            nvs_commit(s_nvs);
            break;
        case MSG_SAVE_MODEL:
            nvs_set_blob(s_nvs, "model", &m.model, sizeof(m.model));
            nvs_commit(s_nvs);
            break;
        }
    }
}

/* The senders below use static message buffers (the model makes them too big
 * for comfortable stacks). They are only called from the audio task. */
static void post(const store_msg_t *m)
{
    if (xQueueSend(s_q, m, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "store queue full, dropped message %d", m->type);
    }
}

void store_commit_session(time_t start, uint32_t secs, instrument_t instr)
{
    static store_msg_t m;
    m = (store_msg_t){ .type = MSG_COMMIT, .start = start, .secs = secs, .instr = instr };
    post(&m);
}

void store_relabel_last(instrument_t instr)
{
    static store_msg_t m;
    m = (store_msg_t){ .type = MSG_RELABEL, .instr = instr };
    post(&m);
}

void store_checkpoint(time_t start, uint32_t secs, instrument_t instr)
{
    static store_msg_t m;
    m = (store_msg_t){ .type = MSG_CHECKPOINT, .start = start, .secs = secs, .instr = instr };
    post(&m);
}

void store_clear_checkpoint(void)
{
    static store_msg_t m;
    m = (store_msg_t){ .type = MSG_CLEAR_CKPT };
    post(&m);
}

void store_save_model(const cls_model_t *model)
{
    static store_msg_t m;
    m = (store_msg_t){ .type = MSG_SAVE_MODEL };
    m.model = *model;
    post(&m);
}

void store_load_model(cls_model_t *model)
{
    size_t len = sizeof(*model);
    if (nvs_get_blob(s_nvs, "model", model, &len) == ESP_OK && len == sizeof(*model) &&
        cls_model_valid(model)) {
        ESP_LOGI(TAG, "loaded learned instrument model");
        return;
    }
    cls_model_default(model);
}

int store_get_setting(const char *key, int def)
{
    int32_t v;
    if (nvs_get_i32(s_nvs, key, &v) == ESP_OK) {
        return (int)v;
    }
    return def;
}

void store_set_setting(const char *key, int value)
{
    nvs_set_i32(s_nvs, key, value);
    nvs_commit(s_nvs);
}

esp_err_t store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &s_nvs));

    s_spiffs_ok = (bsp_spiffs_mount() == ESP_OK);
    if (!s_spiffs_ok) {
        ESP_LOGW(TAG, "SPIFFS not mounted; session CSV disabled");
    }

    /* recover a session that was running when power was lost */
    ckpt_t c;
    size_t len = sizeof(c);
    if (nvs_get_blob(s_nvs, "ckpt", &c, &len) == ESP_OK && len == sizeof(c)) {
        if (c.secs >= CONFIG_PL_MIN_SESSION_S && c.instr < INSTR_COUNT) {
            ESP_LOGI(TAG, "recovering interrupted session: %lu s", (unsigned long)c.secs);
            do_commit((time_t)c.start, c.secs, (instrument_t)c.instr);
            s_last.valid = false;   /* not correctable after a reboot */
        }
        nvs_erase_key(s_nvs, "ckpt");
        nvs_commit(s_nvs);
    }

    s_q = xQueueCreate(8, sizeof(store_msg_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreatePinnedToCore(store_task, "store", 4096, NULL, 2, NULL, 0);
    return ESP_OK;
}
