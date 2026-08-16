#pragma once
#include <stddef.h>
#include "esp_err.h"
void router_log_init(void);
esp_err_t router_log_write(const char *level, const char *message);
size_t router_log_read(char *out, size_t cap);
esp_err_t router_log_clear(void);
