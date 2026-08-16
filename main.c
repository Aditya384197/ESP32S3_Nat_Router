#include "router_core.h"
#include "web_server.h"
#include "nvs_flash.h"
#include "router_log.h"

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(e);
    }
    router_log_init();
    router_core_start();
    web_server_start();
}
