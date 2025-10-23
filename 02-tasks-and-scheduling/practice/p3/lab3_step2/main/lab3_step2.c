#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#define LED_OK       GPIO_NUM_2       // Stack OK indicator
#define LED_WARNING  GPIO_NUM_4       // Stack warning/critical indicator

static const char *TAG = "STACK_MONITOR";

// Stack monitoring thresholds (bytes)
#define STACK_WARNING_THRESHOLD   512
#define STACK_CRITICAL_THRESHOLD  256

// Task handles
TaskHandle_t light_task_handle  = NULL;
TaskHandle_t medium_task_handle = NULL;
TaskHandle_t heavy_task_handle  = NULL;

// ---------------- Stack monitor ----------------
static void stack_monitor_task(void *pv)
{
    ESP_LOGI(TAG, "Stack Monitor Task started");
    vTaskDelay(pdMS_TO_TICKS(300)); // soften start burst

    while (1) {
        ESP_LOGI(TAG, "\n=== STACK USAGE REPORT ===");

        TaskHandle_t tasks[] = {
            light_task_handle,
            medium_task_handle,
            heavy_task_handle,
            xTaskGetCurrentTaskHandle() // monitor itself
        };
        const char* names[] = { "LightTask", "MediumTask", "HeavyTask", "StackMonitor" };

        bool warn = false, crit = false;

        for (int i = 0; i < 4; i++) {
            if (tasks[i]) {
                UBaseType_t rem = uxTaskGetStackHighWaterMark(tasks[i]);
                uint32_t bytes  = rem * sizeof(StackType_t);
                ESP_LOGI(TAG, "%s: %u bytes remaining", names[i], (unsigned)bytes);

                if (bytes < STACK_CRITICAL_THRESHOLD) { crit = true; ESP_LOGE(TAG, "CRITICAL: %s stack very low!", names[i]); }
                else if (bytes < STACK_WARNING_THRESHOLD) { warn = true; ESP_LOGW(TAG, "WARNING: %s stack low", names[i]); }
            }
        }

        if (crit) {
            // Fast blink warning LED
            for (int i = 0; i < 10; i++) {
                gpio_set_level(LED_WARNING, 1); vTaskDelay(pdMS_TO_TICKS(50));
                gpio_set_level(LED_WARNING, 0); vTaskDelay(pdMS_TO_TICKS(50));
            }
            gpio_set_level(LED_OK, 0);
        } else if (warn) {
            gpio_set_level(LED_WARNING, 1); gpio_set_level(LED_OK, 0);
        } else {
            gpio_set_level(LED_OK, 1); gpio_set_level(LED_WARNING, 0);
        }

        ESP_LOGI(TAG, "Free heap: %u bytes", (unsigned)esp_get_free_heap_size());
        ESP_LOGI(TAG, "Min free heap: %u bytes", (unsigned)esp_get_minimum_free_heap_size());

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ---------------- Light task (minimal stack) ----------------
static void light_stack_task(void *pv)
{
    ESP_LOGI(TAG, "Light Stack Task started (minimal usage)");
    vTaskDelay(pdMS_TO_TICKS(150)); // stagger start
    int counter = 0;

    while (1) {
        counter++;
        ESP_LOGI(TAG, "Light task cycle: %d", counter);
        UBaseType_t rem = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGD(TAG, "Light task stack: %u bytes", (unsigned)(rem * sizeof(StackType_t)));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ---------------- Medium task (moderate stack) ----------------
static void medium_stack_task(void *pv)
{
    ESP_LOGI(TAG, "Medium Stack Task started (moderate usage)");
    vTaskDelay(pdMS_TO_TICKS(200)); // stagger start

    while (1) {
        char buffer[256];
        int  numbers[50];

        memset(buffer, 'A', sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        for (int i = 0; i < 50; i++) numbers[i] = i * i;

        ESP_LOGI(TAG, "Medium task: buffer[0]=%c, numbers[49]=%d", buffer[0], numbers[49]);

        UBaseType_t rem = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGD(TAG, "Medium task stack: %u bytes", (unsigned)(rem * sizeof(StackType_t)));

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ---------------- Heavy task (SAFE: use heap, not big stack arrays) ----------------
static void heavy_stack_task(void *pv)
{
    ESP_LOGI(TAG, "Heavy Stack Task started (heap-backed buffers to avoid overflow)");
    vTaskDelay(pdMS_TO_TICKS(250)); // stagger start

    // Allocate once, reuse
    char *large_buffer   = (char*)malloc(1024);
    int  *large_numbers  = (int*) malloc(200 * sizeof(int));
    char *another_buffer = (char*)malloc(512);

    if (!large_buffer || !large_numbers || !another_buffer) {
        ESP_LOGE(TAG, "Heap allocation failed for HeavyTask");
        free(large_buffer); free(large_numbers); free(another_buffer);
        vTaskDelete(NULL); return;
    }

    int cycle = 0;

    while (1) {
        cycle++;

        memset(large_buffer, 'X', 1023);
        large_buffer[1023] = '\0';
        for (int i = 0; i < 200; i++) large_numbers[i] = i * cycle;
        snprintf(another_buffer, 512, "Cycle %d with large data processing", cycle);

        ESP_LOGW(TAG, "Heavy task cycle %d: Using HEAP buffers", cycle);
        ESP_LOGI(TAG, "Heavy task: %s", another_buffer);
        ESP_LOGI(TAG, "Large buffer length: %d", (int)strlen(large_buffer));
        ESP_LOGI(TAG, "Last number: %d", large_numbers[199]);

        UBaseType_t rem = uxTaskGetStackHighWaterMark(NULL);
        uint32_t bytes  = rem * sizeof(StackType_t);
        if (bytes < STACK_CRITICAL_THRESHOLD) {
            ESP_LOGE(TAG, "DANGER: Heavy task stack critically low: %u bytes!", (unsigned)bytes);
        } else if (bytes < STACK_WARNING_THRESHOLD) {
            ESP_LOGW(TAG, "Heavy task stack: %u bytes remaining", (unsigned)bytes);
        } else {
            ESP_LOGI(TAG, "Heavy task stack: %u bytes remaining", (unsigned)bytes);
        }

        vTaskDelay(pdMS_TO_TICKS(4000));
    }

    // (unreachable in demo)
    free(large_buffer); free(large_numbers); free(another_buffer);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== FreeRTOS Stack Monitoring Demo (Step 1) ===");

    // LEDs
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_OK) | (1ULL << LED_WARNING),
        .pull_down_en = 0,
        .pull_up_en   = 0,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_OK, 0);
    gpio_set_level(LED_WARNING, 0);

    ESP_LOGI(TAG, "LED: GPIO2=OK, GPIO4=Warning/Critical");

    // Safe stack sizes (+staggered start) -> no reboot loop
    xTaskCreate(light_stack_task,   "LightTask",    2048, NULL, 2, &light_task_handle);
    xTaskCreate(medium_stack_task,  "MediumTask",   3072, NULL, 2, &medium_task_handle);
    xTaskCreate(heavy_stack_task,   "HeavyTask",    3072, NULL, 2, &heavy_task_handle);
    xTaskCreate(stack_monitor_task, "StackMonitor", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "All tasks created. Monitor will report every 3 seconds.");
}


void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    // หมายเหตุ: ESP-IDF จะเรียก hook นี้อัตโนมัติเมื่อพบ overflow (Mode 2)
    ESP_LOGE("STACK_OVERFLOW", "Task %s has overflowed its stack!", pcTaskName);
    ESP_LOGE("STACK_OVERFLOW", "System will restart...");

    for (int i = 0; i < 10; i++) {
        gpio_set_level(LED_WARNING, 1); vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(LED_WARNING, 0); vTaskDelay(pdMS_TO_TICKS(50));
    }
    esp_restart();
}