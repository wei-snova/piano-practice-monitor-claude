#include "net.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "store.h"

static const char *TAG = "net";

static volatile bool s_connected;
static char s_ip[16];
static httpd_handle_t s_httpd;

bool net_connected(void)
{
    return s_connected;
}

const char *net_ip(void)
{
    return s_ip;
}

/* ---------- HTTP ---------- */

static esp_err_t csv_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"practice_sessions.csv\"");
    FILE *f = fopen(STORE_CSV_PATH, "r");
    if (!f) {
        httpd_resp_sendstr(req, "date,start,instrument,minutes\n");
        return ESP_OK;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>Practice log</title><style>body{font-family:system-ui;margin:24px;max-width:520px}"
        "table{border-collapse:collapse;width:100%}td,th{padding:6px 8px;border-bottom:1px solid #ddd;text-align:right}"
        "td:first-child,th:first-child{text-align:left}</style>"
        "<h2>Practice, last 30 days</h2><table><tr><th>Date</th><th>Cello</th><th>Piano</th><th>Sessions</th></tr>");
    time_t now = time(NULL);
    uint32_t today = app_date_of(now);
    char row[160];
    for (int i = 0; i < 30 && today; i++) {
        uint32_t d = app_date_minus(today, i);
        day_rec_t r;
        if (!store_get_day(d, &r)) {
            continue;
        }
        snprintf(row, sizeof(row),
                 "<tr><td>%04lu-%02lu-%02lu</td><td>%lu min</td><td>%lu min</td><td>%u</td></tr>",
                 (unsigned long)(d / 10000), (unsigned long)(d / 100 % 100), (unsigned long)(d % 100),
                 (unsigned long)(r.secs[INSTR_CELLO] / 60), (unsigned long)(r.secs[INSTR_PIANO] / 60),
                 r.sessions);
        httpd_resp_sendstr_chunk(req, row);
    }
    httpd_resp_sendstr_chunk(req, "</table><p><a href='/sessions.csv'>Download every session (CSV)</a></p>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void http_start(void)
{
    if (s_httpd) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 0;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "http server failed to start");
        return;
    }
    static const httpd_uri_t idx = { .uri = "/", .method = HTTP_GET, .handler = index_get };
    static const httpd_uri_t csv = { .uri = "/sessions.csv", .method = HTTP_GET, .handler = csv_get };
    httpd_register_uri_handler(s_httpd, &idx);
    httpd_register_uri_handler(s_httpd, &csv);
}

/* ---------- Wi-Fi ---------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = '\0';
        esp_wifi_connect();   /* keep retrying; the logger works offline meanwhile */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "connected, ip %s", s_ip);
        http_start();
    }
}

static void on_time_sync(struct timeval *tv)
{
    ESP_LOGI(TAG, "clock synchronised");
}

void net_start(void)
{
    setenv("TZ", CONFIG_PL_TIMEZONE, 1);
    tzset();

    if (strlen(CONFIG_PL_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no Wi-Fi configured (idf.py menuconfig -> Practice Logger)");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, CONFIG_PL_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_PL_WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = strlen(CONFIG_PL_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.sync_cb = on_time_sync;
    esp_netif_sntp_init(&sntp);
}
