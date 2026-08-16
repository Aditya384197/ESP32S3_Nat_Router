#include "system_metrics.h"

#include "esp_timer.h"
#include "esp_system.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"

#include "driver/temperature_sensor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs.h"

#include <string.h>

#define NS "router"
#define KEY_PERF "perf_pct"

static temperature_sensor_handle_t s_sensor = NULL;
static float s_temp = -1.0f;

static uint8_t s_perf = 100;

static TaskHandle_t s_task = NULL;

static uint32_t s_last_total = 0;
static uint32_t s_last_idle[2] = {0, 0};

static uint8_t s_cpu[2] = {0, 0};


static void metrics_task(void *arg)
{
    (void)arg;

    /*
     * ESP32-S3 has two CPU cores in the normal SMP configuration.
     * Temperature sensor availability is hardware/configuration dependent,
     * therefore failure to initialize it must not stop the metrics task.
     */
    temperature_sensor_config_t cfg =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

    if (temperature_sensor_install(&cfg, &s_sensor) == ESP_OK) {
        if (temperature_sensor_enable(s_sensor) != ESP_OK) {
            s_sensor = NULL;
        }
    }

    TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0);
    TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);

    s_last_total = 0;
    s_last_idle[0] = 0;
    s_last_idle[1] = 0;

    for (;;) {

        /*
         * Temperature is informational only.
         * Never let a sensor failure terminate the metrics task.
         */
        if (s_sensor != NULL) {
            float temperature = -1.0f;

            if (temperature_sensor_get_celsius(
                    s_sensor,
                    &temperature) == ESP_OK) {
                s_temp = temperature;
            }
        }

        UBaseType_t task_count = uxTaskGetNumberOfTasks();

        if (task_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        TaskStatus_t *status =
            pvPortMalloc(task_count * sizeof(TaskStatus_t));

        uint32_t total_runtime = 0;
        uint32_t idle0_runtime = 0;
        uint32_t idle1_runtime = 0;

        if (status != NULL) {

            UBaseType_t count =
                uxTaskGetSystemState(
                    status,
                    task_count,
                    &total_runtime
                );

            for (UBaseType_t i = 0; i < count; ++i) {

                if (status[i].xHandle == idle0) {
                    idle0_runtime =
                        status[i].ulRunTimeCounter;
                }

                if (status[i].xHandle == idle1) {
                    idle1_runtime =
                        status[i].ulRunTimeCounter;
                }
            }

            vPortFree(status);
        }

        /*
         * Calculate CPU utilization from the idle-time delta.
         *
         * The first sample is intentionally ignored because there is
         * no previous reference interval yet.
         */
        if (s_last_total != 0 &&
            total_runtime > s_last_total) {

            uint32_t delta_total =
                total_runtime - s_last_total;

            uint32_t delta_idle0 =
                idle0_runtime - s_last_idle[0];

            uint32_t delta_idle1 =
                idle1_runtime - s_last_idle[1];

            if (delta_total != 0) {

                uint32_t idle_percent0 =
                    (delta_idle0 >= delta_total)
                        ? 100
                        : ((delta_idle0 * 100U) /
                           delta_total);

                uint32_t idle_percent1 =
                    (delta_idle1 >= delta_total)
                        ? 100
                        : ((delta_idle1 * 100U) /
                           delta_total);

                s_cpu[0] =
                    (uint8_t)(
                        idle_percent0 >= 100
                            ? 0
                            : 100 - idle_percent0
                    );

                s_cpu[1] =
                    (uint8_t)(
                        idle_percent1 >= 100
                            ? 0
                            : 100 - idle_percent1
                    );
            }
        }

        s_last_total = total_runtime;
        s_last_idle[0] = idle0_runtime;
        s_last_idle[1] = idle1_runtime;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


void system_metrics_init(void)
{
    nvs_handle_t handle;

    if (nvs_open(
            NS,
            NVS_READONLY,
            &handle) == ESP_OK) {

        uint8_t performance = 0;

        if (nvs_get_u8(
                handle,
                KEY_PERF,
                &performance) == ESP_OK) {

            if (performance >= 10 &&
                performance <= 100) {

                s_perf = performance;
            }
        }

        nvs_close(handle);
    }

    BaseType_t result =
        xTaskCreatePinnedToCore(
            metrics_task,
            "metrics",
            4096,
            NULL,
            2,
            &s_task,
            1
        );

    if (result != pdPASS) {
        s_task = NULL;
    }
}


float system_metrics_temperature(void)
{
    return s_temp;
}


uint8_t system_metrics_cpu_load(void)
{
    return (s_cpu[0] > s_cpu[1])
        ? s_cpu[0]
        : s_cpu[1];
}


uint8_t system_metrics_cpu0_load(void)
{
    return s_cpu[0];
}


uint8_t system_metrics_cpu1_load(void)
{
    return s_cpu[1];
}


uint32_t system_metrics_free_heap(void)
{
    return esp_get_free_heap_size();
}


uint32_t system_metrics_min_heap(void)
{
    return esp_get_minimum_free_heap_size();
}


uint32_t system_metrics_free_psram(void)
{
    if (!esp_psram_is_initialized()) {
        return 0;
    }

    return heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM
    );
}


uint32_t system_metrics_min_free_psram(void)
{
    if (!esp_psram_is_initialized()) {
        return 0;
    }

    return heap_caps_get_minimum_free_size(
        MALLOC_CAP_SPIRAM
    );
}


uint32_t system_metrics_uptime_s(void)
{
    return (uint32_t)(
        esp_timer_get_time() /
        1000000ULL
    );
}


void system_metrics_set_performance(uint8_t percent)
{
    if (percent < 10) {
        percent = 10;
    }

    if (percent > 100) {
        percent = 100;
    }

    s_perf = percent;

    nvs_handle_t handle;

    if (nvs_open(
            NS,
            NVS_READWRITE,
            &handle) == ESP_OK) {

        esp_err_t err =
            nvs_set_u8(
                handle,
                KEY_PERF,
                percent
            );

        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }

        (void)err;

        nvs_close(handle);
    }
}


uint8_t system_metrics_get_performance(void)
{
    return s_perf;
}
