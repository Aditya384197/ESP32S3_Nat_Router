#include "router_core.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "lwip/lwip_napt.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "router_log.h"
#include "system_metrics.h"


#define NS "router"
#define KEY_STA_SSID "Airtel_2.4GHz"
#define KEY_STA_PASS "Kgf@0987"
#define KEY_AP_SSID "ESP32s3_router"
#define KEY_AP_PASS "ak@12345"
#define KEY_AP_CH "ap_ch"
#define DEFAULT_AP_SSID "ESP32s3_router"
#define DEFAULT_AP_PASS "ak@12345"
#define AP_MAX_CONNECTIONS 8

static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static volatile bool s_sta_connected;
static volatile bool s_sta_has_ip;
static volatile uint32_t s_sta_ip_uptime;
static char s_last_reason[48] = "Not connected";
static bool s_napt;
static esp_timer_handle_t s_reconnect_timer;
static uint8_t s_retry_count;
static uint8_t s_performance = 100;
static void connect_sta(void);

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    connect_sta();
}

static void router_nvs_get_str(const char *key, char *out, size_t cap, const char *fallback)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = cap;
        if (nvs_get_str(h, key, out, &len) != ESP_OK) {
            strlcpy(out, fallback, cap);
        }
        nvs_close(h);
    } else {
        strlcpy(out, fallback, cap);
    }
}

static void load_config(char *sta_ssid, char *sta_pass, char *ap_ssid, char *ap_pass, uint8_t *channel)
{
    router_nvs_get_str(KEY_STA_SSID, sta_ssid, 33, "");
    router_nvs_get_str(KEY_STA_PASS, sta_pass, 65, "");
    router_nvs_get_str(KEY_AP_SSID, ap_ssid, 33, DEFAULT_AP_SSID);
    router_nvs_get_str(KEY_AP_PASS, ap_pass, 65, DEFAULT_AP_PASS);
    nvs_handle_t h;
    *channel = 1;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t c;
        if (nvs_get_u8(h, KEY_AP_CH, &c) == ESP_OK && c >= 1 && c <= 13) *channel = c;
        nvs_close(h);
    }
}

static void set_ap_network(void)
{
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap));
}

static void set_napt(bool enable)
{
    if (!s_ap || enable == s_napt) return;
    esp_err_t e = enable ? esp_netif_napt_enable(s_ap) : esp_netif_napt_disable(s_ap);
    if (e == ESP_OK) s_napt = enable;
}

static void configure_radio(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW40));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW40));
}

static void apply_wifi_config(void)
{
    char sta_ssid[33], sta_pass[65], ap_ssid[33], ap_pass[65];
    uint8_t ap_channel;
    load_config(sta_ssid, sta_pass, ap_ssid, ap_pass, &ap_channel);

    wifi_config_t sta = {0};
    wifi_config_t ap = {0};
    strlcpy((char *)sta.sta.ssid, sta_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, sta_pass, sizeof(sta.sta.password));
    sta.sta.scan_method = WIFI_FAST_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.failure_retry_cnt = 5;
    sta.sta.threshold.rssi = -127;
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;

    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, ap_pass, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(ap_ssid);
    ap.ap.channel = ap_channel;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = AP_MAX_CONNECTIONS;
    ap.ap.beacon_interval = 100;
    ap.ap.pmf_cfg.capable = true;
    ap.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    configure_radio();
}

static void connect_sta(void)
{
    wifi_config_t sta = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &sta) != ESP_OK) return;
    if (sta.sta.ssid[0] == 0) return;
    esp_wifi_connect();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_STA_START) {
        connect_sta();
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        s_sta_connected = true;
        router_log_write("INFO", "STA connected");
        s_retry_count = 0;
        if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
        const wifi_event_sta_connected_t *e = data;
        if (e) {
            wifi_config_t ap = {0};
            if (esp_wifi_get_config(WIFI_IF_AP, &ap) == ESP_OK && ap.ap.channel != e->channel) {
                ap.ap.channel = e->channel;
                esp_wifi_set_config(WIFI_IF_AP, &ap);
            }
            snprintf(s_last_reason, sizeof(s_last_reason), "Connected on channel %u", e->channel);
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        router_log_write("WARN", "STA disconnected");
        s_sta_has_ip = false;
        set_napt(false);
        const wifi_event_sta_disconnected_t *e = data;
        if (e) snprintf(s_last_reason, sizeof(s_last_reason), "Wi-Fi reason %u", e->reason);
        wifi_config_t cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            cfg.sta.channel = 0;
            cfg.sta.bssid_set = false;
            esp_wifi_set_config(WIFI_IF_STA, &cfg);
        }
        wifi_config_t ap_cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) == ESP_OK) {
            uint8_t stored_channel = 1;
            nvs_handle_t nh;
            if (nvs_open(NS, NVS_READONLY, &nh) == ESP_OK) {
                nvs_get_u8(nh, KEY_AP_CH, &stored_channel);
                nvs_close(nh);
            }
            if (stored_channel >= 1 && stored_channel <= 13) {
                ap_cfg.ap.channel = stored_channel;
                esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
            }
        }
        if (s_reconnect_timer) {
            uint32_t delay_ms = s_retry_count < 6 ? (500U << s_retry_count) : 15000U;
            if (delay_ms > 15000U) delay_ms = 15000U;
            if (s_retry_count < 6) ++s_retry_count;
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
        }
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != IP_EVENT) return;
    if (id == IP_EVENT_STA_GOT_IP) {
        s_sta_has_ip = true;
        router_log_write("INFO", "STA got IP");
        s_sta_ip_uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        set_napt(true);
        strlcpy(s_last_reason, "Internet uplink ready", sizeof(s_last_reason));
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_sta_has_ip = false;
        set_napt(false);
    }
}

esp_err_t router_set_performance(uint8_t percent)
{
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;
    wifi_ps_type_t ps = percent >= 80 ? WIFI_PS_NONE : (percent >= 50 ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM);
    int8_t tx = (int8_t)(32 + ((uint16_t)(percent - 10) * 46) / 90);
    esp_err_t e = esp_wifi_set_ps(ps);
    if (e == ESP_OK) e = esp_wifi_set_max_tx_power(tx);
    if (e == ESP_OK) { s_performance = percent; system_metrics_set_performance(percent); }
    return e;
}
uint8_t router_get_performance(void){ return s_performance; }
uint8_t router_get_tx_power_quarter_dbm(void)
{
    int8_t tx = 0;
    if (esp_wifi_get_max_tx_power(&tx) != ESP_OK) {
        return 0;
    }
    return tx > 0 ? (uint8_t)tx : 0;
}

void router_core_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    if (!s_sta || !s_ap) abort();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.wifi_task_core_id = 0;
    cfg.static_rx_buf_num = 24;
    cfg.dynamic_rx_buf_num = 96;
    cfg.static_tx_buf_num = 32;
    cfg.cache_tx_buf_num = 32;
    cfg.dynamic_tx_buf_num = 48;
    cfg.rx_ba_win = 32;
    cfg.ampdu_rx_enable = 1;
    cfg.ampdu_tx_enable = 1;
    cfg.amsdu_tx_enable = 1;
    cfg.nvs_enable = 0;
    cfg.sta_disconnected_pm = false;

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    system_metrics_init();
    s_performance = system_metrics_get_performance();
    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sta_reconnect"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event, NULL));
    apply_wifi_config();
    ESP_ERROR_CHECK(router_set_performance(s_performance));
    set_ap_network();
    ESP_ERROR_CHECK(esp_wifi_start());
}

esp_netif_t *router_get_sta_netif(void) { return s_sta; }
esp_netif_t *router_get_ap_netif(void) { return s_ap; }
bool router_sta_has_ip(void) { return s_sta_has_ip; }
bool router_sta_connected(void) { return s_sta_connected; }
bool router_napt_enabled(void) { return s_napt; }

int8_t router_sta_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : -127;
}

uint8_t router_sta_signal_percent(void)
{
    int r = router_sta_rssi();
    if (r <= -100) return 0;
    if (r >= -50) return 100;
    return (uint8_t)((r + 100) * 2);
}

float router_sta_distance_m(void)
{
    int r = router_sta_rssi();
    if (r <= -100) return 0.0f;
    const float tx_dbm = 20.0f;
    const float path_loss_n = 2.7f;
    float d = powf(10.0f, (tx_dbm - (float)r) / (10.0f * path_loss_n));
    if (d < 0.1f) d = 0.1f;
    if (d > 999.0f) d = 999.0f;
    return d;
}

uint8_t router_ap_client_count(void)
{
    wifi_sta_list_t list = {0};
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? (uint8_t)list.num : 0;
}

void router_get_sta_config(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    if (ssid && ssid_len) {
        wifi_config_t sta = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &sta) == ESP_OK)
            strlcpy(ssid, (const char *)sta.sta.ssid, ssid_len);
        else
            ssid[0] = 0;
    }
    if (password && password_len) password[0] = 0;
}

esp_err_t router_set_sta_config(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32 || !password || strlen(password) > 63) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h; if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t e = nvs_set_str(h, KEY_STA_SSID, ssid);
    if (e == ESP_OK) e = nvs_set_str(h, KEY_STA_PASS, password);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return e;
    wifi_config_t sta = {0};
    strlcpy((char*)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char*)sta.sta.password, password, sizeof(sta.sta.password));
    sta.sta.scan_method = WIFI_FAST_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.failure_retry_cnt = 5;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
    if (s_sta_connected) esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    return esp_wifi_connect();
}

void router_get_ap_config(char *ssid, size_t ssid_len, char *password, size_t password_len, uint8_t *channel)
{
    router_nvs_get_str(KEY_AP_SSID, ssid, ssid_len, DEFAULT_AP_SSID);
    if (password && password_len) password[0] = 0;
    if (channel) {
        *channel = 1; nvs_handle_t h;
        if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, KEY_AP_CH, channel); nvs_close(h); }
    }
}

esp_err_t router_set_ap_config(const char *ssid, const char *password, uint8_t channel)
{
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32 || !password || strlen(password) < 8 || strlen(password) > 63 || channel < 1 || channel > 13) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h; if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t e = nvs_set_str(h, KEY_AP_SSID, ssid);
    if (e == ESP_OK) e = nvs_set_str(h, KEY_AP_PASS, password);
    if (e == ESP_OK) e = nvs_set_u8(h, KEY_AP_CH, channel);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return e;
    wifi_config_t ap = {0};
    strlcpy((char*)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    strlcpy((char*)ap.ap.password, password, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(ssid);
    uint8_t active_channel = channel;
    if (s_sta_connected) {
        wifi_ap_record_t sta_ap = {0};
        if (esp_wifi_sta_get_ap_info(&sta_ap) == ESP_OK && sta_ap.primary >= 1 && sta_ap.primary <= 13) {
            active_channel = sta_ap.primary;
        }
    }
    ap.ap.channel = active_channel;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = AP_MAX_CONNECTIONS;
    ap.ap.beacon_interval = 100;
    ap.ap.pmf_cfg.capable = true;
    ap.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    return ESP_OK;
}

const char *router_get_last_disconnect_reason(void) { return s_last_reason; }
uint32_t router_get_sta_uptime_s(void) { return s_sta_has_ip ? (uint32_t)((esp_timer_get_time() / 1000000ULL) - s_sta_ip_uptime) : 0; }
void router_restart(void) { vTaskDelay(pdMS_TO_TICKS(250)); esp_restart(); }

void router_factory_reset(void)
{
    nvs_flash_erase();
    router_log_clear();
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}
