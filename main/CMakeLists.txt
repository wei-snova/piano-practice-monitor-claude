idf_component_register(
    SRCS "main.c" "detector.c" "session.c" "classifier.c" "store.c" "net.c" "ui.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer spiffs
)
