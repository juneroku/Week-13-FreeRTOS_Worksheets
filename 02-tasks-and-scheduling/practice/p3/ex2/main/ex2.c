#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"

#define LED_OK       GPIO_NUM_2
#define LED_WARNING  GPIO_NUM_4
static const char *TAG = "STACK_MONITOR_EX2";

#define STACK_WARNING_THRESHOLD   512u
#define STACK_CRITICAL_THRESHOLD  256u
#define CHANGE_NOTIFY_THRESHOLD   64u   // แจ้งเตือนเมื่อเปลี่ยน > 64 bytes

#ifndef BYTES_TO_WORDS
#define BYTES_TO_WORDS(b)  ((b) / sizeof(StackType_t))
#endif

static TaskHandle_t light_task_handle  = NULL;
static TaskHandle_t medium_task_handle = NULL;
static TaskHandle_t heavy_task_handle  = NULL;
static TaskHandle_t dynmon_task_handle = NULL;

// ---- Demo tasks (เหมือน Ex1) ----
static void light_stack_task(void *pv) {
    ESP_LOGI(TAG, "Light Task started");
    vTaskDelay(pdMS_TO_TICKS(150));
    int c=0; TickType_t last=xTaskGetTickCount(); const TickType_t T=pdMS_TO_TICKS(2000);
    while(1){ c++; ESP_LOGI(TAG,"Light cycle:%d",c); vTaskDelayUntil(&last,T); }
}
static void medium_stack_task(void *pv){
    ESP_LOGI(TAG, "Medium Task started");
    vTaskDelay(pdMS_TO_TICKS(180));
    TickType_t last=xTaskGetTickCount(); const TickType_t T=pdMS_TO_TICKS(3000);
    while(1){
        char b[256]; int n[50]; memset(b,'A',255); b[255]='\0';
        for(int i=0;i<50;i++) n[i]=i*i;
        ESP_LOGI(TAG,"Medium: buf0=%c, n49=%d",b[0],n[49]);
        vTaskDelayUntil(&last,T);
    }
}
static void heavy_stack_task(void *pv){
    ESP_LOGI(TAG, "Heavy (optimized) started");
    vTaskDelay(pdMS_TO_TICKS(200));
    char *B=(char*)malloc(1024); int *N=(int*)malloc(200*sizeof(int)); char *S=(char*)malloc(512);
    if(!B||!N||!S){ ESP_LOGE(TAG,"Heap alloc fail"); free(B); free(N); free(S); vTaskDelete(NULL); return; }
    int cyc=0; TickType_t last=xTaskGetTickCount(); const TickType_t T=pdMS_TO_TICKS(4000);
    while(1){
        cyc++; memset(B,'Y',1023); B[1023]='\0'; for(int i=0;i<200;i++) N[i]=i*cyc; snprintf(S,512,"Opt cyc %d",cyc);
        ESP_LOGI(TAG,"Heavy: %s, last=%d",S,N[199]);
        UBaseType_t remw=uxTaskGetStackHighWaterMark(NULL); uint32_t remb=remw*sizeof(StackType_t);
        ESP_LOGI(TAG,"Heavy stack: %" PRIu32 " bytes",remb);
        vTaskDelayUntil(&last,T);
    }
}

// ---- Exercise 2: Dynamic Stack Monitor ----
typedef struct {
    TaskHandle_t h;
    const char  *name;
    UBaseType_t  prev_words;   // ค่า HWM ครั้งก่อน
} watch_t;

#define MAX_WATCH 6
static watch_t watch_list[MAX_WATCH];
static uint8_t watch_count=0;

static void watch_add(TaskHandle_t h,const char*name){
    if(!h||!name||watch_count>=MAX_WATCH) return;
    for(uint8_t i=0;i<watch_count;i++) if(watch_list[i].h==h) return;
    watch_list[watch_count].h=h; watch_list[watch_count].name=name; watch_list[watch_count].prev_words=0; watch_count++;
}

static void dynamic_monitor_task(void *pv){
    ESP_LOGI(TAG,"Dynamic Monitor started");
    vTaskDelay(pdMS_TO_TICKS(300));
    TickType_t last=xTaskGetTickCount(); const TickType_t T=pdMS_TO_TICKS(3000);

    while(1){
        bool warn=false,crit=false;
        ESP_LOGI(TAG,"\n=== DYNAMIC STACK MONITOR TICK ===");
        for(uint8_t i=0;i<watch_count;i++){
            TaskHandle_t h=watch_list[i].h; if(!h) continue;
            UBaseType_t curw=uxTaskGetStackHighWaterMark(h); uint32_t curb=curw*sizeof(StackType_t);

            if(watch_list[i].prev_words!=0){
                if(curw < watch_list[i].prev_words){
                    uint32_t inc=(watch_list[i].prev_words-curw)*sizeof(StackType_t);
                    if(inc>=CHANGE_NOTIFY_THRESHOLD)
                        ESP_LOGW(TAG,"%s stack usage +%" PRIu32 " bytes",watch_list[i].name,inc);
                }else if(curw > watch_list[i].prev_words){
                    uint32_t dec=(curw-watch_list[i].prev_words)*sizeof(StackType_t);
                    if(dec>=CHANGE_NOTIFY_THRESHOLD)
                        ESP_LOGI(TAG,"%s stack usage -%" PRIu32 " bytes",watch_list[i].name,dec);
                }
            }else{
                ESP_LOGI(TAG,"%s initial remaining: %" PRIu32 " bytes",watch_list[i].name,curb);
            }

            watch_list[i].prev_words=curw;

            if(curb<STACK_CRITICAL_THRESHOLD){crit=true; ESP_LOGE(TAG,"CRITICAL: %s",watch_list[i].name);}
            else if(curb<STACK_WARNING_THRESHOLD){warn=true; ESP_LOGW(TAG,"WARNING: %s",watch_list[i].name);}
        }

        if(crit){ gpio_set_level(LED_WARNING,1); gpio_set_level(LED_OK,0);}
        else if(warn){ gpio_set_level(LED_WARNING,1); gpio_set_level(LED_OK,0);}
        else { gpio_set_level(LED_WARNING,0); gpio_set_level(LED_OK,1); }

        ESP_LOGI(TAG,"Free heap: %" PRIu32 ", Min heap: %" PRIu32,
                 (uint32_t)esp_get_free_heap_size(), (uint32_t)esp_get_minimum_free_heap_size());
        vTaskDelayUntil(&last,T);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG,"=== Lab3 — Ex2 (Dynamic Stack Monitoring) ===");

    gpio_config_t io={
        .intr_type=GPIO_INTR_DISABLE,.mode=GPIO_MODE_OUTPUT,
        .pin_bit_mask=(1ULL<<LED_OK)|(1ULL<<LED_WARNING),
        .pull_down_en=0,.pull_up_en=0,
    };
    gpio_config(&io); gpio_set_level(LED_OK,0); gpio_set_level(LED_WARNING,0);

    // สร้าง demo tasks (จำนวนพอประมาณ, ไม่เยอะ)
    xTaskCreate(light_stack_task,  "LightTask",  (configSTACK_DEPTH_TYPE)BYTES_TO_WORDS(2048), NULL, 2, &light_task_handle);
    xTaskCreate(medium_stack_task, "MediumTask", (configSTACK_DEPTH_TYPE)BYTES_TO_WORDS(3072), NULL, 2, &medium_task_handle);
    xTaskCreate(heavy_stack_task,  "HeavyTask",  (configSTACK_DEPTH_TYPE)BYTES_TO_WORDS(3072), NULL, 2, &heavy_task_handle);

    xTaskCreate(dynamic_monitor_task,"DynMonitor",(configSTACK_DEPTH_TYPE)BYTES_TO_WORDS(4096), NULL, 3, &dynmon_task_handle);

    // ลงทะเบียนตัวที่จะเฝ้าดู
    watch_add(light_task_handle,"LightTask");
    watch_add(medium_task_handle,"MediumTask");
    watch_add(heavy_task_handle,"HeavyTask");
    watch_add(dynmon_task_handle,"DynMonitor");

    ESP_LOGI(TAG,"All tasks created. Dynamic monitor every 3s.");
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    ESP_LOGE("STACK_OVERFLOW","Task %s overflow!", pcTaskName);
    gpio_set_level(LED_WARNING,1);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}