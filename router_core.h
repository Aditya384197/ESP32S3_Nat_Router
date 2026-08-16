#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#define ROUTER_AP_IP "192.168.4.1"

void router_core_start(void);
esp_netif_t *router_get_sta_netif(void);
esp_netif_t *router_get_ap_netif(void);
bool router_sta_has_ip(void);
bool router_sta_connected(void);
bool router_napt_enabled(void);
int8_t router_sta_rssi(void);
uint8_t router_sta_signal_percent(void);
float router_sta_distance_m(void);
uint8_t router_ap_client_count(void);
void router_get_sta_config(char *ssid, size_t ssid_len, char *password, size_t password_len);
esp_err_t router_set_sta_config(const char *ssid, const char *password);
void router_get_ap_config(char *ssid, size_t ssid_len, char *password, size_t password_len, uint8_t *channel);
esp_err_t router_set_ap_config(const char *ssid, const char *password, uint8_t channel);
const char *router_get_last_disconnect_reason(void);
uint32_t router_get_sta_uptime_s(void);
void router_restart(void);
void router_factory_reset(void);

esp_err_t router_set_performance(uint8_t percent);
uint8_t router_get_performance(void);
uint8_t router_get_tx_power_quarter_dbm(void);
