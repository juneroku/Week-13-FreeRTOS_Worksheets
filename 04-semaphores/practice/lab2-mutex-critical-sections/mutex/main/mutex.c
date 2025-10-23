#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "MUTEX_LAB_EXP2";

/* ===== LED pins ===== */
#define LED_TASK1    GPIO_NUM_2
#define LED_TASK2    GPIO_NUM_4
#define LED_TASK3    GPIO_NUM_5
#define LED_CRITICAL GPIO_NUM_18

/* ===== Mutex handle (สร้างไว้แต่ “ไม่ใช้” ในการทดลองนี้) ===== */
SemaphoreHandle_t xMutex;

/* ===== Shared Resource (UNPROTECTED in Exp#2) ===== */
typedef struct {
    uint32_t counter;
    char     shared_buffer[100];
    uint32_t checksum;
    uint32_t access_count;
} shared_resource_t;

static shared_resource_t shared_data = {0, "", 0, 0};

/* ===== Stats ===== */
typedef struct {
    uint32_t successful_access;
    uint32_t failed_access;
    uint32_t corruption_detected;
    uint32_t priority_inversions;
} access_stats_t;

static access_stats_t stats = {0, 0, 0, 0};

/* ===== Helpers ===== */
static uint32_t calculate_checksum(const char* data, uint32_t counter) {
    uint32_t sum = counter;
    for (int i = 0; data[i] != '\0'; i++) {
        sum += (uint32_t)data[i] * (i + 1);
    }
    return sum;
}

/* ===== Critical section (NO MUTEX) ===== */
static void access_shared_resource(int task_id, const char* task_name, gpio_num_t led_pin) {
    char     temp_buffer[100];
    uint32_t temp_counter;
    uint32_t expected_checksum;

    ESP_LOGI(TAG, "[%s] Requesting access to shared resource (NO MUTEX)...", task_name);

    // ▼▼▼ ปิดการใช้ Mutex — เข้าส่วนวิกฤตโดยไม่มีการป้องกัน ▼▼▼
    // if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5000)) == pdTRUE) { ... xSemaphoreGive(xMutex); }
    // ▲▲▲ ---------------------------------------------------- ▲▲▲

    ESP_LOGW(TAG, "[%s] ⚠ ENTERING CRITICAL SECTION WITHOUT MUTEX (UNSAFE)", task_name);
    stats.successful_access++;

    // ไฟแสดงสถานะ
    gpio_set_level(led_pin, 1);
    gpio_set_level(LED_CRITICAL, 1);

    // --- BEGIN "CRITICAL" (UNPROTECTED) ---
    temp_counter      = shared_data.counter;
    strcpy(temp_buffer, shared_data.shared_buffer);
    expected_checksum = shared_data.checksum;

    // ตรวจความถูกต้องก่อนแก้ไข
    uint32_t calc_before = calculate_checksum(temp_buffer, temp_counter);
    if (calc_before != expected_checksum && shared_data.access_count > 0) {
        ESP_LOGE(TAG, "[%s] 🔴 DATA CORRUPTION DETECTED! (pre-modify) Exp#2", task_name);
        ESP_LOGE(TAG, "Expected:%lu  Calculated:%lu", expected_checksum, calc_before);
        stats.corruption_detected++;
    }

    ESP_LOGI(TAG, "[%s] Current - Counter:%lu  Buffer:'%s'", task_name, temp_counter, temp_buffer);

    // เวลาประมวลผลเพื่อเปิดโอกาสให้สลับคอนเท็กซ์
    vTaskDelay(pdMS_TO_TICKS(500 + (esp_random() % 1000)));

    // แก้ไขข้อมูลร่วม (ไม่ปลอดภัย)
    shared_data.counter = temp_counter + 1;
    snprintf(shared_data.shared_buffer, sizeof(shared_data.shared_buffer),
             "Modified by %s #%lu", task_name, shared_data.counter);
    shared_data.checksum = calculate_checksum(shared_data.shared_buffer, shared_data.counter);
    shared_data.access_count++;

    ESP_LOGI(TAG, "[%s] ✓ Modified - Counter:%lu  Buffer:'%s'",
             task_name, shared_data.counter, shared_data.shared_buffer);

    // เวลาประมวลผลต่อ (เพิ่มโอกาส race)
    vTaskDelay(pdMS_TO_TICKS(200 + (esp_random() % 500)));
    // --- END "CRITICAL" ---

    // ปิดไฟ
    gpio_set_level(led_pin, 0);
    gpio_set_level(LED_CRITICAL, 0);

    // ไม่มี xSemaphoreGive ในการทดลองนี้
}

/* ===== Tasks ===== */
static void high_priority_task(void *pv) {
    ESP_LOGI(TAG, "High Priority Task started (prio:%d)", uxTaskPriorityGet(NULL));
    while (1) {
        access_shared_resource(1, "HIGH_PRI", LED_TASK1);
        vTaskDelay(pdMS_TO_TICKS(5000 + (esp_random() % 3000))); // 5–8s
    }
}

static void medium_priority_task(void *pv) {
    ESP_LOGI(TAG, "Medium Priority Task started (prio:%d)", uxTaskPriorityGet(NULL));
    while (1) {
        access_shared_resource(2, "MED_PRI", LED_TASK2);
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 2000))); // 3–5s
    }
}

static void low_priority_task(void *pv) {
    ESP_LOGI(TAG, "Low Priority Task started (prio:%d)", uxTaskPriorityGet(NULL));
    while (1) {
        access_shared_resource(3, "LOW_PRI", LED_TASK3);
        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 1000))); // 2–3s
    }
}

/* งาน CPU หนักเพื่อเร่งให้ race เกิดง่ายขึ้น */
static void cpu_load_task(void *pv) {
    ESP_LOGI(TAG, "CPU Load Task started (prio:%d)", uxTaskPriorityGet(NULL));
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "🔄 Simulating CPU-intensive background work (no mutex)...");
        uint32_t t0 = xTaskGetTickCount();
        for (volatile int i = 0; i < 1000000; i++) { /* busy work */ }
        uint32_t t1 = xTaskGetTickCount();
        ESP_LOGI(TAG, "Background work took %lu ms", (t1 - t0) * portTICK_PERIOD_MS);
    }
}

/* มอนิเตอร์ระบบ */
static void monitor_task(void *pv) {
    ESP_LOGI(TAG, "System monitor started (prio:%d)", uxTaskPriorityGet(NULL));
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGI(TAG, "\n═══ MUTEX SYSTEM MONITOR (EXP#2) ═══");
        ESP_LOGW(TAG, "Mutex State: BYPASSED (NOT USED in this experiment)");
        ESP_LOGI(TAG, "Shared Resource:");
        ESP_LOGI(TAG, "  Counter      : %lu", shared_data.counter);
        ESP_LOGI(TAG, "  Buffer       : '%s'", shared_data.shared_buffer);
        ESP_LOGI(TAG, "  Access Count : %lu", shared_data.access_count);
        ESP_LOGI(TAG, "  Checksum     : %lu", shared_data.checksum);

        uint32_t chk = calculate_checksum(shared_data.shared_buffer, shared_data.counter);
        if (chk != shared_data.checksum && shared_data.access_count > 0) {
            ESP_LOGE(TAG, "🔴 CURRENT DATA CORRUPTION DETECTED! (Exp#2)");
            stats.corruption_detected++;
        }

        ESP_LOGI(TAG, "Access Stats:");
        ESP_LOGI(TAG, "  Successful : %lu  (UNPROTECTED in Exp#2)", stats.successful_access);
        ESP_LOGI(TAG, "  Failed     : %lu  (timeout not applicable)", stats.failed_access);
        ESP_LOGI(TAG, "  Corrupted  : %lu", stats.corruption_detected);
        float rate = (stats.successful_access + stats.failed_access) ?
            (float)stats.successful_access / (stats.successful_access + stats.failed_access) * 100.0f : 0.0f;
        ESP_LOGI(TAG, "  Success Rate: %.1f%%", rate);
        ESP_LOGI(TAG, "══════════════════════════════════\n");
    }
}

/* ===== app_main ===== */
void app_main(void) {
    ESP_LOGW(TAG, "Experiment #2: DISABLE MUTEX to observe race conditions");

    // GPIO init
    gpio_set_direction(LED_TASK1,    GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TASK2,    GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TASK3,    GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CRITICAL, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_TASK1, 0);
    gpio_set_level(LED_TASK2, 0);
    gpio_set_level(LED_TASK3, 0);
    gpio_set_level(LED_CRITICAL, 0);

    // สร้าง mutex (แต่ไม่ใช้)
    xMutex = xSemaphoreCreateMutex();

    // Init shared data
    shared_data.counter = 0;
    strcpy(shared_data.shared_buffer, "Initial state");
    shared_data.checksum = calculate_checksum(shared_data.shared_buffer, shared_data.counter);
    shared_data.access_count = 0;

    // สร้างงาน (priority ชุดอ้างอิง: High=5, CPU=4, Med=3, Low=2, Monitor=1)
    xTaskCreate(high_priority_task, "HighPri", 3072, NULL, 5, NULL);
    xTaskCreate(cpu_load_task,      "CPULoad", 2048, NULL, 4, NULL);
    xTaskCreate(medium_priority_task,"MedPri", 3072, NULL, 3, NULL);
    xTaskCreate(low_priority_task,  "LowPri",  3072, NULL, 2, NULL);
    xTaskCreate(monitor_task,       "Monitor", 3072, NULL, 1, NULL);

    ESP_LOGW(TAG, "⚠ SYSTEM RUNNING WITHOUT MUTEX — EXPECT DATA CORRUPTION");

    // LED startup sequence
    for (int i = 0; i < 2; i++) {
        gpio_set_level(LED_TASK1, 1); vTaskDelay(pdMS_TO_TICKS(200)); gpio_set_level(LED_TASK1, 0);
        gpio_set_level(LED_TASK2, 1); vTaskDelay(pdMS_TO_TICKS(200)); gpio_set_level(LED_TASK2, 0);
        gpio_set_level(LED_TASK3, 1); vTaskDelay(pdMS_TO_TICKS(200)); gpio_set_level(LED_TASK3, 0);
        gpio_set_level(LED_CRITICAL, 1); vTaskDelay(pdMS_TO_TICKS(200)); gpio_set_level(LED_CRITICAL, 0);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}