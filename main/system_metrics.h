#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

void system_metrics_init(void);
float system_metrics_temperature(void);
uint8_t system_metrics_cpu_load(void);
uint8_t system_metrics_cpu0_load(void);
uint8_t system_metrics_cpu1_load(void);
uint32_t system_metrics_free_heap(void);
uint32_t system_metrics_min_heap(void);
uint32_t system_metrics_free_psram(void);
uint32_t system_metrics_min_free_psram(void);
uint32_t system_metrics_uptime_s(void);
void system_metrics_set_performance(uint8_t percent);
uint8_t system_metrics_get_performance(void);
