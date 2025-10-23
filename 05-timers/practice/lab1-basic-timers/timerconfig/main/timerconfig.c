#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "sdkconfig.h"  // <-- ใช้พิมพ์ค่า CONFIG_FREERTOS_TIMER_*

static const char *TAG = "SW_TIMERS_EXP2";

/* prototype ที่ต้องใช้ก่อนเรียกใช้ */
static void dynamic_timer_callback(TimerHandle_t xTimer);
static void timer_control_task(void *pvParameters);   // <-- แทน lambda

/* ===== LED pins ===== */
#define LED_BLINK     GPIO_NUM_2
#define LED_HEARTBEAT GPIO_NUM_4
#define LED_STATUS    GPIO_NUM_5
#define LED_ONESHOT   GPIO_NUM_18

/* ===== Timer handles ===== */
static TimerHandle_t xBlinkTimer;
static TimerHandle_t xHeartbeatTimer;
static TimerHandle_t xStatusTimer;
static TimerHandle_t xOneShotTimer;
static TimerHandle_t xDynamicTimer;

/* ===== Periods (ms) ===== */
#define BLINK_PERIOD      500
#define HEARTBEAT_PERIOD  2000
#define STATUS_PERIOD     5000
#define ONESHOT_DELAY     3000

/* ===== Statistics ===== */
typedef struct {
    uint32_t blink_count;
    uint32_t heartbeat_count;
    uint32_t status_count;
    uint32_t oneshot_count;
    uint32_t dynamic_count;
} timer_stats_t;

static timer_stats_t stats = {0};

static bool led_blink_state = false;
static bool led_heartbeat_state = false;

/* ===== Callbacks ===== */
static void blink_timer_callback(TimerHandle_t xTimer) {
    stats.blink_count++;
    led_blink_state = !led_blink_state;
    gpio_set_level(LED_BLINK, led_blink_state);
    ESP_LOGI(TAG, "💫 Blink Timer: Toggle #%lu (LED %s)",
             stats.blink_count, led_blink_state ? "ON" : "OFF");

    // ทุก ๆ 20 ครั้ง สั่ง start one-shot
    if (stats.blink_count % 20 == 0) {
        ESP_LOGI(TAG, "🚀 Start One-shot (delay 3s)");
        if (xTimerStart(xOneShotTimer, 0) != pdPASS) {
            ESP_LOGW(TAG, "One-shot start FAILED (queue full?)");
        }
    }
}

static void heartbeat_timer_callback(TimerHandle_t xTimer) {
    stats.heartbeat_count++;
    ESP_LOGI(TAG, "💓 Heartbeat #%lu", stats.heartbeat_count);

    // double-blink
    gpio_set_level(LED_HEARTBEAT, 1); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 0); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 1); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 0);

    // 25% ปรับคาบ blink แบบสุ่ม
    if (esp_random() % 4 == 0) {
        uint32_t new_period = 300 + (esp_random() % 400); // 300–700ms
        ESP_LOGI(TAG, "🔧 Change blink period -> %lums", new_period);
        if (xTimerChangePeriod(xBlinkTimer, pdMS_TO_TICKS(new_period), 0) != pdPASS) {
            ESP_LOGW(TAG, "ChangePeriod FAILED (queue full?)");
        }
    }
}

static void status_timer_callback(TimerHandle_t xTimer) {
    stats.status_count++;
    ESP_LOGI(TAG, "📊 Status #%lu", stats.status_count);

    // flash
    gpio_set_level(LED_STATUS, 1); vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(LED_STATUS, 0);

    // พิมพ์สถิติ + สถานะ timer
    ESP_LOGI(TAG, "═══ TIMER STATS ═══");
    ESP_LOGI(TAG, "Blink: %lu  Heartbeat: %lu  Status: %lu  OneShot: %lu  Dynamic: %lu",
             stats.blink_count, stats.heartbeat_count, stats.status_count,
             stats.oneshot_count, stats.dynamic_count);
    ESP_LOGI(TAG, "Timer states:");
    ESP_LOGI(TAG, "  Blink     : %s (Period %lu ms)",
             xTimerIsTimerActive(xBlinkTimer) ? "ACTIVE" : "INACTIVE",
             (unsigned long)(xTimerGetPeriod(xBlinkTimer) * portTICK_PERIOD_MS));
    ESP_LOGI(TAG, "  Heartbeat : %s (Period %lu ms)",
             xTimerIsTimerActive(xHeartbeatTimer) ? "ACTIVE" : "INACTIVE",
             (unsigned long)(xTimerGetPeriod(xHeartbeatTimer) * portTICK_PERIOD_MS));
    ESP_LOGI(TAG, "  Status    : %s (Period %lu ms)",
             xTimerIsTimerActive(xStatusTimer) ? "ACTIVE" : "INACTIVE",
             (unsigned long)(xTimerGetPeriod(xStatusTimer) * portTICK_PERIOD_MS));
    ESP_LOGI(TAG, "  One-shot  : %s",
             xTimerIsTimerActive(xOneShotTimer) ? "ACTIVE" : "INACTIVE");
}

static void oneshot_timer_callback(TimerHandle_t xTimer) {
    stats.oneshot_count++;
    ESP_LOGI(TAG, "⚡ One-shot #%lu", stats.oneshot_count);

    for (int i = 0; i < 5; i++) {
        gpio_set_level(LED_ONESHOT, 1); vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(LED_ONESHOT, 0); vTaskDelay(pdMS_TO_TICKS(50));
    }

    // สร้างไดนามิก timer แบบ one-shot
    uint32_t random_period = 1000 + (esp_random() % 3000);
    ESP_LOGI(TAG, "🎲 Create Dynamic (period %lums)", random_period);

    xDynamicTimer = xTimerCreate("DynamicTimer",
                                 pdMS_TO_TICKS(random_period),
                                 pdFALSE, (void*)0,
                                 dynamic_timer_callback);
    if (xDynamicTimer) {
        if (xTimerStart(xDynamicTimer, 0) != pdPASS) {
            ESP_LOGW(TAG, "Dynamic start FAILED (queue full?)");
        }
    }
}

static void dynamic_timer_callback(TimerHandle_t xTimer) {
    stats.dynamic_count++;
    ESP_LOGI(TAG, "🌟 Dynamic #%lu", stats.dynamic_count);

    // flash ทุกดวงสั้น ๆ
    gpio_set_level(LED_BLINK, 1);
    gpio_set_level(LED_HEARTBEAT, 1);
    gpio_set_level(LED_STATUS, 1);
    gpio_set_level(LED_ONESHOT, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
    gpio_set_level(LED_BLINK, led_blink_state);
    gpio_set_level(LED_HEARTBEAT, 0);
    gpio_set_level(LED_STATUS, 0);
    gpio_set_level(LED_ONESHOT, 0);

    if (xTimerDelete(xTimer, 0) != pdPASS) {
        ESP_LOGW(TAG, "Dynamic delete FAILED (queue full?)");
    } else {
        ESP_LOGI(TAG, "Dynamic deleted");
    }
    xDynamicTimer = NULL;
}

/* ===== Stress task: ยิงคำสั่งไปที่ timer command queue เพื่อลองผลของ priority=1/queue=5 ===== */
static void timer_stress_task(void *pv) {
    ESP_LOGW(TAG, "Timer stress task started (will flood timer commands periodically)");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // ทุก 10 วินาที
        ESP_LOGW(TAG, "🚧 Flooding timer commands (no wait) ...");

        int sent = 0, fail = 0;
        for (int i = 0; i < 20; i++) {
            // ใช้ ticksToWait = 0 เพื่อดูว่า queue เล็กจะ drop ได้ไหม
            if (xTimerReset(xBlinkTimer, 0) == pdPASS) sent++; else fail++;
            if (xTimerChangePeriod(xHeartbeatTimer, pdMS_TO_TICKS(HEARTBEAT_PERIOD), 0) == pdPASS) sent++; else fail++;
            if (xTimerReset(xStatusTimer, 0) == pdPASS) sent++; else fail++;
        }

        ESP_LOGW(TAG, "Stress batch done: sent=%d, fail=%d (queue len=%d, timer prio=%d)",
                 sent, fail,
#ifdef CONFIG_FREERTOS_TIMER_QUEUE_LENGTH
                 (int)CONFIG_FREERTOS_TIMER_QUEUE_LENGTH,
#else
                 -1,
#endif
#ifdef CONFIG_FREERTOS_TIMER_TASK_PRIORITY
                 (int)CONFIG_FREERTOS_TIMER_TASK_PRIORITY
#else
                 -1
#endif
        );
    }
}

/* ===== Timer control task (แทน lambda) ===== */
static void timer_control_task(void *pvParameters) {
    (void)pvParameters;
    ESP_LOGI(TAG, "Timer control task started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        int action = esp_random() % 3;
        switch (action) {
            case 0:
                ESP_LOGI(TAG, "⏸ stop heartbeat 5s");
                xTimerStop(xHeartbeatTimer, 0);
                vTaskDelay(pdMS_TO_TICKS(5000));
                ESP_LOGI(TAG, "▶ start heartbeat");
                xTimerStart(xHeartbeatTimer, 0);
                break;
            case 1:
                ESP_LOGI(TAG, "🔄 reset status");
                xTimerReset(xStatusTimer, 0);
                break;
            default: {
                uint32_t np = 200 + (esp_random() % 600);
                ESP_LOGI(TAG, "⚙ change blink -> %lums", np);
                xTimerChangePeriod(xBlinkTimer, pdMS_TO_TICKS(np), 0);
            } break;
        }
    }
}

/* ===== app_main ===== */
void app_main(void) {
    ESP_LOGI(TAG, "Software Timers Lab (EXP2: Timer Configuration)");

#ifdef CONFIG_FREERTOS_USE_TIMERS
    ESP_LOGI(TAG, "FreeRTOS timers ENABLED");
#else
    ESP_LOGE(TAG, "FreeRTOS timers DISABLED (enable CONFIG_FREERTOS_USE_TIMERS)");
#endif

#ifdef CONFIG_FREERTOS_TIMER_TASK_PRIORITY
    ESP_LOGI(TAG, "Timer task priority : %d", (int)CONFIG_FREERTOS_TIMER_TASK_PRIORITY);
#endif
#ifdef CONFIG_FREERTOS_TIMER_QUEUE_LENGTH
    ESP_LOGI(TAG, "Timer queue length  : %d", (int)CONFIG_FREERTOS_TIMER_QUEUE_LENGTH);
#endif
#ifdef CONFIG_FREERTOS_TIMER_TASK_STACK_SIZE
    ESP_LOGI(TAG, "Timer task stack    : %d", (int)CONFIG_FREERTOS_TIMER_TASK_STACK_SIZE);
#endif

    // GPIO init
    gpio_set_direction(LED_BLINK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_HEARTBEAT, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_STATUS, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_ONESHOT, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_BLINK, 0);
    gpio_set_level(LED_HEARTBEAT, 0);
    gpio_set_level(LED_STATUS, 0);
    gpio_set_level(LED_ONESHOT, 0);

    // Create timers
    xBlinkTimer = xTimerCreate("BlinkTimer",
                               pdMS_TO_TICKS(BLINK_PERIOD),
                               pdTRUE, (void*)1, blink_timer_callback);
    xHeartbeatTimer = xTimerCreate("HeartbeatTimer",
                                   pdMS_TO_TICKS(HEARTBEAT_PERIOD),
                                   pdTRUE, (void*)2, heartbeat_timer_callback);
    xStatusTimer = xTimerCreate("StatusTimer",
                                pdMS_TO_TICKS(STATUS_PERIOD),
                                pdTRUE, (void*)3, status_timer_callback);
    xOneShotTimer = xTimerCreate("OneShotTimer",
                                 pdMS_TO_TICKS(ONESHOT_DELAY),
                                 pdFALSE, (void*)4, oneshot_timer_callback);

    if (xBlinkTimer && xHeartbeatTimer && xStatusTimer && xOneShotTimer) {
        ESP_LOGI(TAG, "All timers created. Starting...");
        xTimerStart(xBlinkTimer, 0);
        xTimerStart(xHeartbeatTimer, 0);
        xTimerStart(xStatusTimer, 0);

        // Control & Stress tasks
        xTaskCreate(timer_stress_task,   "TimerStress",  2048, NULL, 1, NULL);
        xTaskCreate(timer_control_task,  "TimerControl", 2048, NULL, 1, NULL);

        ESP_LOGI(TAG, "LED map: GPIO2=blink, GPIO4=heartbeat, GPIO5=status, GPIO18=oneshot");
        ESP_LOGI(TAG, "NOTE: ตั้งค่าใน menuconfig เป็น PRIORITY=1, QUEUE_LEN=5 ตามโจทย์");
    } else {
        ESP_LOGE(TAG, "Create timer FAILED. Check CONFIG_FREERTOS_USE_TIMERS=y");
    }
}