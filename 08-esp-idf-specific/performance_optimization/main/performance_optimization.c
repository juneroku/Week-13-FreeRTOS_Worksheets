// Performance-Optimization-and-Monitoring.c — ESP-IDF v5.5.1 (ESP32)
// แก้แล้ว:
//  - ป้องกัน assert จาก xTaskNotifyGive() ด้วย EventGroup ซิงก์ก่อนเริ่ม ping/pong
//  - แก้ TWDT: ถ้า init แล้วจะไม่เรียกซ้ำ และหลัง benchmark จบ "ถอด PONG ออกจาก TWDT"
//  - feed TWDT ให้ทุก task ที่อยู่ในรายการ TWDT อย่างเหมาะสม
//  - คง idle hooks/monitor/benchmark เดิม

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_freertos_hooks.h"
#include "sdkconfig.h"
#include "esp_random.h"

#define TAG   "PERF"
#define CORE0 0
#define CORE1 1

/* ----------------- Config ----------------- */
#define MON_PERIOD_MS       2000
#define WDT_TIMEOUT_S       5
#define PINGPONG_ROUNDS     2000
#define STATIC_STACK_WORDS  2048

/* ----------------- Shared ----------------- */
static SemaphoreHandle_t s_lock;

/* Idle counters per core */
static volatile uint32_t s_idle0_ticks = 0;
static volatile uint32_t s_idle1_ticks = 0;

/* Task handles */
static TaskHandle_t s_tPerf = NULL;
static TaskHandle_t s_tPing = NULL;
static TaskHandle_t s_tPong = NULL;
static TaskHandle_t s_tBG   = NULL;

/* Notify partners for ping-pong */
static TaskHandle_t s_notify_ping = NULL;
static TaskHandle_t s_notify_pong = NULL;

/* Start synchronization */
static EventGroupHandle_t s_start_evt;
#define START_RDY_PING   (1<<0)
#define START_RDY_PONG   (1<<1)
#define START_GO         (1<<2)

/* ---------- Idle hooks ---------- */
static bool idle_hook_core0(void) { s_idle0_ticks++; return false; }
static bool idle_hook_core1(void) { s_idle1_ticks++; return false; }

/* ---------- Health snapshot ---------- */
typedef struct {
    size_t free_8b, free_int;
    size_t min_free_8b;
    size_t largest_8b, largest_int;
    uint32_t idle0, idle1;
    uint64_t t_us;
} health_t;

static void take_health(health_t *h) {
    h->t_us        = esp_timer_get_time();
    h->free_8b     = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    h->free_int    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    h->min_free_8b = esp_get_minimum_free_heap_size();
    h->largest_8b  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    h->largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    h->idle0       = s_idle0_ticks;
    h->idle1       = s_idle1_ticks;
}

/* ---------- Pong ---------- */
static void pong_task(void *arg) {
    /* ประกาศพร้อม แล้วรอ GO เพื่อให้มั่นใจว่า s_notify_ping ถูกตั้งค่าแล้ว */
    xEventGroupSetBits(s_start_evt, START_RDY_PONG);
    xEventGroupWaitBits(s_start_evt, START_GO, pdFALSE, pdTRUE, portMAX_DELAY);

    for (;;) {
        /* รอ ping (ไม่ต้อง timeout เพราะช่วง benchmark ping จะยิงต่อเนื่อง) */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* ตอบกลับ */
        xTaskNotifyGive(s_notify_ping);
        /* feed TWDT ระหว่าง benchmark */
        esp_task_wdt_reset();
    }
}

/* ---------- Ping ---------- */
static void ping_task(void *arg) {
    ESP_LOGI(TAG, "Context-switch benchmark start on cores: ping=%d, pong on the other", xPortGetCoreID());

    /* ประกาศพร้อม แล้วรอ GO */
    xEventGroupSetBits(s_start_evt, START_RDY_PING);
    xEventGroupWaitBits(s_start_evt, START_GO, pdFALSE, pdTRUE, portMAX_DELAY);

    /* warm-up */
    for (int i=0;i<10;i++){
        xTaskNotifyGive(s_notify_pong);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }

    uint64_t t0 = esp_timer_get_time();
    for (int i=0; i<PINGPONG_ROUNDS; ++i) {
        xTaskNotifyGive(s_notify_pong);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if ((i & 255) == 0) esp_task_wdt_reset();
    }
    uint64_t t1 = esp_timer_get_time();

    double total_us = (double)(t1 - t0);
    double per_half_switch_us = total_us / (2.0 * PINGPONG_ROUNDS);
    ESP_LOGI(TAG, "PingPong: rounds=%d total=%.3f ms, avg half-switch=%.3f us",
             PINGPONG_ROUNDS, total_us/1000.0, per_half_switch_us);

    /* latency ของ vTaskDelay(1) */
    uint64_t t2 = esp_timer_get_time();
    vTaskDelay(1);
    uint64_t t3 = esp_timer_get_time();
    ESP_LOGI(TAG, "vTaskDelay(1) latency: %.3f ms (tick=%dms)",
             (t3 - t2)/1000.0, (int)(1000/configTICK_RATE_HZ));

    /* ✅ จบ benchmark แล้ว: ถอด PONG ออกจาก TWDT เพื่อไม่ให้ WDT ฟ้อง */
    if (s_tPong) {
        esp_err_t e = esp_task_wdt_delete(s_tPong);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "esp_task_wdt_delete(PONG) = %d", (int)e);
        }
    }

    /* ping อยู่ต่อและ feed TWDT เอง */
    for(;;){
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------- Performance monitor ---------- */
static void perf_monitor_task(void *arg) {
    ESP_LOGI(TAG, "Performance monitor start on Core %d", xPortGetCoreID());
    health_t prev = {0}, now = {0};

    vTaskDelay(pdMS_TO_TICKS(1000));
    take_health(&prev);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MON_PERIOD_MS));
        take_health(&now);

        uint32_t d_idle0 = now.idle0 - prev.idle0;
        uint32_t d_idle1 = now.idle1 - prev.idle1;
        uint64_t dt_us   = now.t_us - prev.t_us;

        double core0_rel = d_idle0 / (double)(d_idle0 + d_idle1 + 1);
        double core1_rel = d_idle1 / (double)(d_idle0 + d_idle1 + 1);
        double load0_pct = 100.0 * (1.0 - core0_rel);
        double load1_pct = 100.0 * (1.0 - core1_rel);

        ESP_LOGI(TAG,
                 "HEAP: free8=%uB freeINT=%uB min8=%uB largest8=%uB largestINT=%uB | "
                 "IDLE(d%u,d%u,%.1fms) ~ load0≈%.1f%% load1≈%.1f%%",
                 (unsigned)now.free_8b, (unsigned)now.free_int,
                 (unsigned)now.min_free_8b,
                 (unsigned)now.largest_8b, (unsigned)now.largest_int,
                 d_idle0, d_idle1, dt_us/1000.0, load0_pct, load1_pct);

        if (s_tPerf) { ESP_LOGI(TAG, "Stack HW perf_mon: %u words", (unsigned)uxTaskGetStackHighWaterMark(s_tPerf)); }
        if (s_tPing) { ESP_LOGI(TAG, "Stack HW ping: %u words", (unsigned)uxTaskGetStackHighWaterMark(s_tPing)); }
        if (s_tPong) { ESP_LOGI(TAG, "Stack HW pong: %u words", (unsigned)uxTaskGetStackHighWaterMark(s_tPong)); }
        if (s_tBG)   { ESP_LOGI(TAG, "Stack HW bg: %u words", (unsigned)uxTaskGetStackHighWaterMark(s_tBG)); }

        if (now.largest_8b < 16*1024) {
            ESP_LOGW(TAG, "Fragmentation warning: largest 8-bit block < 16KB (=%uB)", (unsigned)now.largest_8b);
        }

        esp_task_wdt_reset();
        prev = now;
    }
}

/* ---------- Background workload (จำลอง alloc) ---------- */
typedef struct { uint8_t *buf; size_t len; } blob_t;

static void background_task(void *arg) {
    ESP_LOGI(TAG, "BG start on Core %d (simulate workload/alloc)", xPortGetCoreID());

    blob_t big = {0};
    big.len = 24 * 1024;
    big.buf = (uint8_t*)heap_caps_malloc(big.len, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!big.buf) { ESP_LOGW(TAG, "big alloc failed, fallback default heap"); big.buf = (uint8_t*)malloc(big.len); }

    for (;;) {
        size_t small_len = 1500 + (esp_random() % 1500);
        void *tmp = heap_caps_malloc(small_len, MALLOC_CAP_32BIT);
        if (tmp) {
            memset(tmp, 0xA5, small_len > 32 ? 32 : small_len);
            vTaskDelay(pdMS_TO_TICKS(10));
            heap_caps_free(tmp);
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------- Static stack for perf monitor ---------- */
static StaticTask_t s_perf_tcb;
static StackType_t  s_perf_stack[STATIC_STACK_WORDS];

/* ------------------- app_main ------------------- */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    s_lock = xSemaphoreCreateMutex();
    s_start_evt = xEventGroupCreate();
    configASSERT(s_start_evt);

    /* Idle hooks */
    ESP_ERROR_CHECK(esp_register_freertos_idle_hook_for_cpu(idle_hook_core0, CORE0));
    ESP_ERROR_CHECK(esp_register_freertos_idle_hook_for_cpu(idle_hook_core1, CORE1));

    /* TWDT init (ถ้าเคย init แล้วจะข้าม) */
    const esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms     = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = (1 << CORE0) | (1 << CORE1),
        .trigger_panic  = true,
    };
    esp_err_t e = esp_task_wdt_init(&twdt_cfg);
    if (e == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "TWDT already enabled by sdkconfig, skip esp_task_wdt_init()");
    } else {
        ESP_ERROR_CHECK(e);
    }

    /* สร้าง tasks */
    s_tPerf = xTaskCreateStaticPinnedToCore(
        perf_monitor_task, "PERF_MON",
        STATIC_STACK_WORDS, NULL, tskIDLE_PRIORITY+3,
        s_perf_stack, &s_perf_tcb, CORE0);
    configASSERT(s_tPerf);
    ESP_ERROR_CHECK(esp_task_wdt_add(s_tPerf));

    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(pong_task, "PONG", 3072, NULL, tskIDLE_PRIORITY+2, &s_tPong, CORE1);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(ping_task, "PING", 4096, NULL, tskIDLE_PRIORITY+2, &s_tPing, CORE0);
    configASSERT(ok == pdPASS);

    ok = xTaskCreatePinnedToCore(background_task, "BG", 4096, NULL, tskIDLE_PRIORITY+1, &s_tBG, CORE1);
    configASSERT(ok == pdPASS);

    /* ตั้ง notify targets ให้ชัดก่อนสตาร์ต และ add ลง TWDT เฉพาะช่วงใช้งาน */
    s_notify_ping = s_tPing;
    s_notify_pong = s_tPong;

    ESP_ERROR_CHECK(esp_task_wdt_add(s_tPing));
    ESP_ERROR_CHECK(esp_task_wdt_add(s_tPong));
    ESP_ERROR_CHECK(esp_task_wdt_add(s_tBG));

    /* ปล่อยสัญญาณ GO ให้ ping/pong เริ่มพร้อมกัน (กันเคส notify บน NULL handle) */
    xEventGroupSetBits(s_start_evt, START_GO);

    ESP_LOGI(TAG, "Performance Optimization demo started.");
}