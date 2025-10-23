
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "COUNTING_SEM_EXP3";

/* ===== LED pins ===== */
#define LED_RESOURCE_1 GPIO_NUM_2
#define LED_RESOURCE_2 GPIO_NUM_4
#define LED_RESOURCE_3 GPIO_NUM_5
#define LED_RESOURCE_4 GPIO_NUM_21
#define LED_RESOURCE_5 GPIO_NUM_22
#define LED_PRODUCER   GPIO_NUM_18
#define LED_SYSTEM     GPIO_NUM_19

/* ===== Config ===== */
#define MAX_RESOURCES 5        // ใช้ค่าเดิมจากทดลองที่ 2
#define NUM_PRODUCERS 8        // ★ เพิ่มเป็น 8
#define NUM_CONSUMERS 3

/* ===== Counting semaphore ===== */
static SemaphoreHandle_t xCountingSemaphore;

/* ===== Resource management ===== */
typedef struct {
    int resource_id;
    bool in_use;
    char current_user[20];
    uint32_t usage_count;
    uint32_t total_usage_time;
} resource_t;

static resource_t resources[MAX_RESOURCES] = {
    {1, false, "", 0, 0},
    {2, false, "", 0, 0},
    {3, false, "", 0, 0},
    {4, false, "", 0, 0},
    {5, false, "", 0, 0}
};

/* ===== System stats ===== */
typedef struct {
    uint32_t total_requests;
    uint32_t successful_acquisitions;
    uint32_t failed_acquisitions;
    uint32_t resources_in_use;
} system_stats_t;

static system_stats_t stats = {0, 0, 0, 0};

/* ===== Helpers ===== */
static inline void set_resource_led(int idx, int on) {
    switch (idx) {
        case 0: gpio_set_level(LED_RESOURCE_1, on); break;
        case 1: gpio_set_level(LED_RESOURCE_2, on); break;
        case 2: gpio_set_level(LED_RESOURCE_3, on); break;
        case 3: gpio_set_level(LED_RESOURCE_4, on); break;
        case 4: gpio_set_level(LED_RESOURCE_5, on); break;
        default: break;
    }
}

static int acquire_resource(const char* user_name) {
    for (int i = 0; i < MAX_RESOURCES; i++) {
        if (!resources[i].in_use) {
            resources[i].in_use = true;
            strncpy(resources[i].current_user, user_name, sizeof(resources[i].current_user)-1);
            resources[i].current_user[sizeof(resources[i].current_user)-1] = '\0';
            resources[i].usage_count++;
            set_resource_led(i, 1);
            stats.resources_in_use++;
            return i;
        }
    }
    return -1;
}

static void release_resource(int resource_index, uint32_t usage_time_ms) {
    if (resource_index >= 0 && resource_index < MAX_RESOURCES) {
        resources[resource_index].in_use = false;
        resources[resource_index].total_usage_time += usage_time_ms;
        resources[resource_index].current_user[0] = '\0';
        set_resource_led(resource_index, 0);
        if (stats.resources_in_use > 0) stats.resources_in_use--;
    }
}

/* ===== Tasks ===== */
static void producer_task(void *pvParameters) {
    int producer_id = *((int*)pvParameters);
    char task_name[20];
    snprintf(task_name, sizeof(task_name), "Producer%d", producer_id);
    ESP_LOGI(TAG, "%s started", task_name);

    while (1) {
        stats.total_requests++;

        // pulse producer LED
        gpio_set_level(LED_PRODUCER, 1);
        vTaskDelay(pdMS_TO_TICKS(40));
        gpio_set_level(LED_PRODUCER, 0);

        ESP_LOGI(TAG, "🏭 %s: Requesting resource...", task_name);
        TickType_t t0 = xTaskGetTickCount();

        if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(8000)) == pdTRUE) {
            uint32_t wait_ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
            stats.successful_acquisitions++;

            int res_idx = acquire_resource(task_name);
            if (res_idx >= 0) {
                ESP_LOGI(TAG, "✓ %s: Acquired resource %d (wait: %lums)",
                         task_name, res_idx + 1, wait_ms);

                uint32_t use_ms = 1000 + (esp_random() % 3000); // 1–4s
                ESP_LOGI(TAG, "🔧 %s: Using resource %d for %lums",
                         task_name, res_idx + 1, use_ms);

                vTaskDelay(pdMS_TO_TICKS(use_ms));

                release_resource(res_idx, use_ms);
                ESP_LOGI(TAG, "✓ %s: Released resource %d", task_name, res_idx + 1);
            } else {
                ESP_LOGE(TAG, "✗ %s: Semaphore acquired but no resource free!", task_name);
            }
            xSemaphoreGive(xCountingSemaphore);
        } else {
            stats.failed_acquisitions++;
            ESP_LOGW(TAG, "⏰ %s: Timeout waiting for resource", task_name);
        }

        // เพิ่ม producers เยอะขึ้น → random delay เล็กน้อยเพื่อกระจายโหลด
        vTaskDelay(pdMS_TO_TICKS(1500 + (esp_random() % 2500)));
    }
}

static void resource_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Resource monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        int available = uxSemaphoreGetCount(xCountingSemaphore);
        int used = MAX_RESOURCES - available;

        ESP_LOGI(TAG, "\n📊 RESOURCE POOL STATUS");
        ESP_LOGI(TAG, "Available: %d/%d  In use: %d", available, MAX_RESOURCES, used);

        for (int i = 0; i < MAX_RESOURCES; i++) {
            if (resources[i].in_use) {
                ESP_LOGI(TAG, "  Resource %d: BUSY (User: %s, Uses: %lu)",
                         i + 1, resources[i].current_user, resources[i].usage_count);
            } else {
                ESP_LOGI(TAG, "  Resource %d: FREE (Uses: %lu)",
                         i + 1, resources[i].usage_count);
            }
        }

        printf("Pool: [");
        for (int i = 0; i < MAX_RESOURCES; i++) printf(resources[i].in_use ? "■" : "□");
        printf("] Available: %d\n", available);
        ESP_LOGI(TAG, "═══════════════════════════\n");
    }
}

static void statistics_task(void *pvParameters) {
    ESP_LOGI(TAG, "Statistics task started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(12000));

        ESP_LOGI(TAG, "\n📈 SYSTEM STATISTICS");
        ESP_LOGI(TAG, "Total requests           : %lu", stats.total_requests);
        ESP_LOGI(TAG, "Successful acquisitions  : %lu", stats.successful_acquisitions);
        ESP_LOGI(TAG, "Failed acquisitions      : %lu", stats.failed_acquisitions);
        ESP_LOGI(TAG, "Current resources in use : %lu", stats.resources_in_use);

        if (stats.total_requests > 0) {
            float success_rate = (float)stats.successful_acquisitions / stats.total_requests * 100.f;
            ESP_LOGI(TAG, "Success rate             : %.1f%%", success_rate);
        }

        uint32_t total_uses = 0, total_time = 0;
        for (int i = 0; i < MAX_RESOURCES; i++) {
            total_uses += resources[i].usage_count;
            total_time += resources[i].total_usage_time;
            ESP_LOGI(TAG, "  Resource %d -> uses: %lu, total time: %lums",
                     i + 1, resources[i].usage_count, resources[i].total_usage_time);
        }
        ESP_LOGI(TAG, "Total usage events       : %lu, total time: %lums",
                 total_uses, total_time);
        ESP_LOGI(TAG, "════════════════════════════\n");
    }
}

static void load_generator_task(void *pvParameters) {
    ESP_LOGI(TAG, "Load generator started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        ESP_LOGW(TAG, "🚀 LOAD GENERATOR: Creating burst of requests...");
        gpio_set_level(LED_SYSTEM, 1);

        for (int burst = 0; burst < 3; burst++) {
            ESP_LOGI(TAG, "Load burst %d/3", burst + 1);
            for (int i = 0; i < MAX_RESOURCES + 2; i++) {
                if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
                    int res_idx = acquire_resource("LoadGen");
                    if (res_idx >= 0) {
                        ESP_LOGI(TAG, "LoadGen: Acquired resource %d", res_idx + 1);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        release_resource(res_idx, 500);
                        ESP_LOGI(TAG, "LoadGen: Released resource %d", res_idx + 1);
                    }
                    xSemaphoreGive(xCountingSemaphore);
                } else {
                    ESP_LOGW(TAG, "LoadGen: Resource pool exhausted");
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        gpio_set_level(LED_SYSTEM, 0);
        ESP_LOGI(TAG, "Load burst completed\n");
    }
}

/* ===== app_main ===== */
void app_main(void) {
    ESP_LOGI(TAG, "Counting Semaphores Lab (EXP3: NUM_PRODUCERS=8) Starting...");

    // GPIO init
    gpio_set_direction(LED_RESOURCE_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RESOURCE_2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RESOURCE_3, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RESOURCE_4, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RESOURCE_5, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PRODUCER,   GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SYSTEM,     GPIO_MODE_OUTPUT);

    gpio_set_level(LED_RESOURCE_1, 0);
    gpio_set_level(LED_RESOURCE_2, 0);
    gpio_set_level(LED_RESOURCE_3, 0);
    gpio_set_level(LED_RESOURCE_4, 0);
    gpio_set_level(LED_RESOURCE_5, 0);
    gpio_set_level(LED_PRODUCER,   0);
    gpio_set_level(LED_SYSTEM,     0);

    // Counting semaphore (initial = MAX_RESOURCES)
    xCountingSemaphore = xSemaphoreCreateCounting(MAX_RESOURCES, MAX_RESOURCES);
    if (!xCountingSemaphore) {
        ESP_LOGE(TAG, "Failed to create counting semaphore!");
        return;
    }
    ESP_LOGI(TAG, "Counting semaphore created (max count: %d)", MAX_RESOURCES);

    // ★ Producer IDs ต้องเป็น static และยาวเท่าจำนวน producers
    static int producer_ids[NUM_PRODUCERS] = {1,2,3,4,5,6,7,8};

    // สร้าง producer tasks 8 ตัว (prio=3)
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        char tname[20];
        snprintf(tname, sizeof(tname), "Producer%d", i + 1);
        xTaskCreate(producer_task, tname, 3072, &producer_ids[i], 3, NULL);
    }

    // Monitoring / stats / load
    xTaskCreate(resource_monitor_task, "ResMonitor", 3072, NULL, 2, NULL);
    xTaskCreate(statistics_task,       "Statistics", 3072, NULL, 1, NULL);
    xTaskCreate(load_generator_task,   "LoadGen",    2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "System created with: Resources=%d, Producers=%d", MAX_RESOURCES, NUM_PRODUCERS);

    // LED startup sequence
    for (int cycle = 0; cycle < 2; cycle++) {
        gpio_set_level(LED_RESOURCE_1, 1); vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(LED_RESOURCE_2, 1); vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(LED_RESOURCE_3, 1); vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(LED_RESOURCE_4, 1); vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(LED_RESOURCE_5, 1); vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(LED_PRODUCER,   1);
        gpio_set_level(LED_SYSTEM,     1);
        vTaskDelay(pdMS_TO_TICKS(250));
        gpio_set_level(LED_RESOURCE_1, 0);
        gpio_set_level(LED_RESOURCE_2, 0);
        gpio_set_level(LED_RESOURCE_3, 0);
        gpio_set_level(LED_RESOURCE_4, 0);
        gpio_set_level(LED_RESOURCE_5, 0);
        gpio_set_level(LED_PRODUCER,   0);
        gpio_set_level(LED_SYSTEM,     0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "System operational — watch contention when 8 producers share 5 resources.");
}
