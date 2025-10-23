#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>           // สำหรับ PRIu32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"

// ===================== Pins & Tags =====================
#define LED_OK       GPIO_NUM_2       // Stack OK indicator
#define LED_WARNING  GPIO_NUM_4       // Stack warning/critical indicator
static const char *TAG = "STACK_MONITOR";

// ===================== Thresholds =====================
#define STACK_WARNING_THRESHOLD   512u   // bytes
#define STACK_CRITICAL_THRESHOLD  256u   // bytes

// แปลง "bytes -> words" สำหรับ xTaskCreate (ESP-IDF ใช้หน่วย words)
#ifndef BYTES_TO_WORDS
#define BYTES_TO_WORDS(b)  ((b) / sizeof(StackType_t))
#endif

// ===================== Handles =====================
static TaskHandle_t light_task_handle   = NULL;
static TaskHandle_t medium_task_handle  = NULL;
static TaskHandle_t monitor_task_handle = NULL;

// Exercise 1: เก็บ handle ของ test tasks
#define MAX_TEST_TASKS  4
static TaskHandle_t test_handles[MAX_TEST_TASKS];
static uint8_t      test_handle_count = 0;

// ===================== Optimized Heavy Task (Step 3) =====================
// ใช้ HEAP สำหรับบัฟเฟอร์ขนาดใหญ่ เพื่อลดโอกาส stack overflow
static void heavy_stack_task(void *pv)
{
    // หมายเหตุ: ใน Ex1 เราจะสร้าง task นี้หลายตัว ด้วย stack size ต่างกัน
    ESP_LOGI(TAG, "Heavy (optimized) started — heap-backed buffers");
    vTaskDelay(pdMS_TO_TICKS(200)); // stagger start

    char *large_buffer   = (char*)malloc(1024);
    int  *large_numbers  = (int*) malloc(200 * sizeof(int));
    char *another_buffer = (char*)malloc(512);

    if (!large_buffer || !large_numbers || !another_buffer) {
        ESP_LOGE(TAG, "Heap allocation failed in Heavy");
        free(large_buffer); free(large_numbers); free(another_buffer);
        vTaskDelete(NULL); return;
    }

    int cycle = 0;
    while (1) {
        cycle++;

        memset(large_buffer, 'Y', 1023);
        large_buffer[1023] = '\0';
        for (int i = 0; i < 200; i++) large_numbers[i] = i * cycle;
        snprintf(another_buffer, 512, "Optimized cycle %d", cycle);

        ESP_LOGI(TAG, "Heavy: %s", another_buffer);
        ESP_LOGI(TAG, "Large buffer len: %d, last number: %d",
                 (int)strlen(large_buffer), large_numbers[199]);

        UBaseType_t rem_words = uxTaskGetStackHighWaterMark(NULL);
        uint32_t    rem_bytes = rem_words * sizeof(StackType_t);
        ESP_LOGI(TAG, "Heavy stack remaining: %" PRIu32 " bytes", rem_bytes);

        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    // not reached ในเดโม่
    free(large_buffer); free(large_numbers); free(another_buffer);
}

// ===================== Light / Medium Tasks =====================
static void light_stack_task(void *pv)
{
    ESP_LOGI(TAG, "Light Stack Task started (minimal usage)");
    vTaskDelay(pdMS_TO_TICKS(150)); // stagger start
    int counter = 0;

    while (1) {
        counter++;
        ESP_LOGI(TAG, "Light task cycle: %d", counter);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void medium_stack_task(void *pv)
{
    ESP_LOGI(TAG, "Medium Stack Task started (moderate usage)");
    vTaskDelay(pdMS_TO_TICKS(180)); // stagger start

    while (1) {
        char buffer[256];
        int  numbers[50];

        memset(buffer, 'A', sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        for (int i = 0; i < 50; i++) numbers[i] = i * i;

        ESP_LOGI(TAG, "Medium: buffer[0]=%c, numbers[49]=%d", buffer[0], numbers[49]);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ===================== Stack Monitor (รายงาน + LED สถานะ) =====================
static void stack_monitor_task(void *pv)
{
    ESP_LOGI(TAG, "Stack Monitor started");
    vTaskDelay(pdMS_TO_TICKS(300)); // soften start burst

    while (1) {
        ESP_LOGI(TAG, "\n=== STACK USAGE REPORT ===");

        // รายงาน tasks หลัก
        TaskHandle_t core_tasks[] = { light_task_handle, medium_task_handle, xTaskGetCurrentTaskHandle() };
        const char*  core_names[] = { "LightTask", "MediumTask", "StackMonitor" };
        bool warn = false, crit = false;

        for (int i = 0; i < 3; i++) {
            if (core_tasks[i]) {
                UBaseType_t rem_words = uxTaskGetStackHighWaterMark(core_tasks[i]);
                uint32_t    rem_bytes = rem_words * sizeof(StackType_t);
                ESP_LOGI(TAG, "%s: %" PRIu32 " bytes remaining", core_names[i], rem_bytes);

                if (rem_bytes < STACK_CRITICAL_THRESHOLD) {
                    crit = true; ESP_LOGE(TAG, "CRITICAL: %s stack very low!", core_names[i]);
                } else if (rem_bytes < STACK_WARNING_THRESHOLD) {
                    warn = true; ESP_LOGW(TAG, "WARNING: %s stack low", core_names[i]);
                }
            }
        }

        // รายงาน test tasks (จาก Ex1)
        for (uint8_t t = 0; t < test_handle_count; t++) {
            TaskHandle_t h = test_handles[t];
            if (!h) continue;
            char namebuf[16];
            snprintf(namebuf, sizeof(namebuf), "Test%u", (unsigned)(t + 1));

            UBaseType_t rem_words = uxTaskGetStackHighWaterMark(h);
            uint32_t    rem_bytes = rem_words * sizeof(StackType_t);
            ESP_LOGI(TAG, "%s: %" PRIu32 " bytes remaining", namebuf, rem_bytes);

            if (rem_bytes < STACK_CRITICAL_THRESHOLD) {
                crit = true; ESP_LOGE(TAG, "CRITICAL: %s stack very low!", namebuf);
            } else if (rem_bytes < STACK_WARNING_THRESHOLD) {
                warn = true; ESP_LOGW(TAG, "WARNING: %s stack low", namebuf);
            }
        }

        // LED สถานะ
        if (crit) {
            for (int i = 0; i < 8; i++) {
                gpio_set_level(LED_WARNING, 1); vTaskDelay(pdMS_TO_TICKS(60));
                gpio_set_level(LED_WARNING, 0); vTaskDelay(pdMS_TO_TICKS(60));
            }
            gpio_set_level(LED_OK, 0);
        } else if (warn) {
            gpio_set_level(LED_WARNING, 1);
            gpio_set_level(LED_OK, 0);
        } else {
            gpio_set_level(LED_OK, 1);
            gpio_set_level(LED_WARNING, 0);
        }

        // Heap summary
        ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", (uint32_t)esp_get_free_heap_size());
        ESP_LOGI(TAG, "Min free heap: %" PRIu32 " bytes", (uint32_t)esp_get_minimum_free_heap_size());

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ===================== Exercise 1: Stack Size Optimization =====================
// ทดสอบสร้าง heavy_stack_task ด้วย stack sizes ต่างกัน
static void test_stack_sizes(void)
{
    // “bytes” ที่อยากทดสอบ → แปลงเป็น “words” ก่อนส่งให้ xTaskCreate
    uint32_t test_sizes_bytes[] = {  2048, 4096 };
    const int N = sizeof(test_sizes_bytes)/sizeof(test_sizes_bytes[0]);

    for (int i = 0; i < N && test_handle_count < MAX_TEST_TASKS; i++) {
        char task_name[20];
        snprintf(task_name, sizeof(task_name), "Test%" PRIu32, test_sizes_bytes[i]);

        configSTACK_DEPTH_TYPE stack_words =
            (configSTACK_DEPTH_TYPE)BYTES_TO_WORDS(test_sizes_bytes[i]);

        // กันเล็กเกินไป (printf/IDF กินสแตกพอควร) — ยกขั้นต่ำ ~768 bytes
        const configSTACK_DEPTH_TYPE min_words = (configSTACK_DEPTH_TYPE)BYTES_TO_WORDS(768);
        if (stack_words < min_words) stack_words = min_words;

        BaseType_t ok = xTaskCreate(heavy_stack_task, task_name,
                                    stack_words, NULL, 1, &test_handles[test_handle_count]);

        ESP_LOGI(TAG, "Create %s with %" PRIu32 " bytes (~%u words): %s",
                 task_name, test_sizes_bytes[i], (unsigned)stack_words,
                 ok==pdPASS ? "OK" : "FAILED");

        if (ok == pdPASS) {
            test_handle_count++;
        }
    }
}

// ===================== app_main =====================
void app_main(void)
{
    ESP_LOGI(TAG, "=== Lab3 — Step 3 + Exercise 1 (Stack Size Optimization) ===");

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
    ESP_LOGI(TAG, "LED: GPIO2=OK, GPIO4=Warn/Critical");

    // Tasks หลัก (ใช้ bytes → words)
    xTaskCreate(light_stack_task,   "LightTask",
                (configSTACK_DEPTH_TYPE)BYTES_TO_WORDS(2048), NULL, 2, &light_task_handle);
    xTaskCreate(medium_stack_task,  "MediumTask",
                (configSTACK_DEPTH_TYPE)BYTES_TO_WORDS(3072), NULL, 2, &medium_task_handle);
    xTaskCreate(stack_monitor_task, "StackMonitor",
                (configSTACK_DEPTH_TYPE)BYTES_TO_WORDS(4096), NULL, 3, &monitor_task_handle);

    // Exercise 1: สร้าง heavy tasks ด้วย stack size ต่างกัน
    test_stack_sizes();

    ESP_LOGI(TAG, "All tasks created. Monitor will report every 3 seconds.");
}

// ===================== Stack Overflow Hook =====================
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    ESP_LOGE("STACK_OVERFLOW", "Task %s has overflowed its stack!", pcTaskName);
    ESP_LOGE("STACK_OVERFLOW", "System will restart...");
    for (int i = 0; i < 10; i++) {
        gpio_set_level(LED_WARNING, 1); vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(LED_WARNING, 0); vTaskDelay(pdMS_TO_TICKS(50));
    }
    esp_restart();
}