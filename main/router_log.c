#include "router_log.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MOUNT "/storage"
#define LOGFILE MOUNT "/router.log"
#define MAX_LOG (128*1024)

static SemaphoreHandle_t s_log_mutex;
static bool s_littlefs_mounted;

static void rotate_if_needed(void){
    struct stat st; if(stat(LOGFILE,&st)!=0 || st.st_size < MAX_LOG) return;
    remove(MOUNT "/router.2.log");
    rename(MOUNT "/router.1.log", MOUNT "/router.2.log");
    rename(LOGFILE, MOUNT "/router.1.log");
}
void router_log_init(void){
    if (!s_log_mutex) s_log_mutex = xSemaphoreCreateMutex();
    esp_vfs_littlefs_conf_t c={.base_path=MOUNT,.partition_label="storage",.format_if_mount_failed=false,.dont_mount=false};
    esp_err_t e = esp_vfs_littlefs_register(&c);
    s_littlefs_mounted = (e == ESP_OK);
    if (!s_littlefs_mounted) {
        return;
    }
    FILE *f = fopen(LOGFILE, "a");
    if (f) {
        fclose(f);
    }
}
esp_err_t router_log_write(const char *level,const char *message){
    if (!s_littlefs_mounted) return ESP_ERR_INVALID_STATE;
    if (!s_log_mutex || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    rotate_if_needed(); FILE *f=fopen(LOGFILE,"a");
    if(!f){ xSemaphoreGive(s_log_mutex); return ESP_FAIL; }
    fprintf(f,"%llu %s %s\n",(unsigned long long)(esp_timer_get_time()/1000000ULL),level?level:"I",message?message:"");
    fclose(f); xSemaphoreGive(s_log_mutex); return ESP_OK;
}
size_t router_log_read(char *out,size_t cap){
    if(!s_littlefs_mounted) { if (out && cap) out[0] = 0; return 0; }
    if(!out||cap<2)return 0;
    if (!s_log_mutex || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) { out[0]=0; return 0; }
    FILE *f=fopen(LOGFILE,"r"); if(!f){out[0]=0; xSemaphoreGive(s_log_mutex); return 0;}
    size_t n=fread(out,1,cap-1,f); fclose(f); out[n]=0; xSemaphoreGive(s_log_mutex); return n;
}
esp_err_t router_log_clear(void){
    if (!s_littlefs_mounted) return ESP_ERR_INVALID_STATE;
    if (!s_log_mutex || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    int r=remove(LOGFILE); xSemaphoreGive(s_log_mutex); return r==0?ESP_OK:ESP_FAIL;
}
