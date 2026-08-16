idf_component_register(
    SRCS
        "main.c"
        "router_core.c"
        "web_server.c"
        "system_metrics.c"
        "router_log.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        esp_wifi
        esp_netif
        nvs_flash
        esp_http_server
        app_update
        esp_partition
        esp_timer
        esp_system
        esp_hw_support
        esp_driver_gpio
        esp_driver_tsens
        esp_psram
        mbedtls
        lwip
        littlefs
)
