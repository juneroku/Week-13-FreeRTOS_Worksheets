// main.c — Step 2 = Step 1 (Basic Tasks) + Step 2 (Task Manager)
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"

#define LED1_PIN GPIO_NUM_2
#define LED2_PIN GPIO_NUM_4
static const char *TAG = "STEP2_1PLUS2";

#define PRINT_HEAP(tag, msg, val) \
    ESP_LOGI(tag, msg " %" PRIu32 " bytes", (uint32_t)(val))

// ---------- Step 1: Basic tasks ----------
void led1_task(void *pvParameters)
{
    int *task_id = (int *)pvParameters;
    ESP_LOGI(TAG, "LED1 Task started with ID: %d", *task_id);
    while (1) {
        ESP_LOGI(TAG, "LED1 ON");
        gpio_set_level(LED1_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "LED1 OFF");
        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void led2_task(void *pvParameters)
{
    char *task_name = (char *)pvParameters;
    ESP_LOGI(TAG, "LED2 Task started: %s", task_name);
    while (1) {
        ESP_LOGI(TAG, "LED2 Blink Fast");
        for (int i = 0; i < 5; i++) {
            gpio_set_level(LED2_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED2_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void system_info_task(void *pvParameters)
{
    ESP_LOGI(TAG, "System Info Task started");
    while (1) {
        ESP_LOGI(TAG, "=== System Information ===");
        PRINT_HEAP(TAG, "Free heap:",        esp_get_free_heap_size());
        PRINT_HEAP(TAG, "Min free heap:",    esp_get_minimum_free_heap_size());
        UBaseType_t n = uxTaskGetNumberOfTasks();
        ESP_LOGI(TAG, "Number of tasks: %lu", (unsigned long)n);
        TickType_t tick = xTaskGetTickCount();
        uint32_t up = (uint32_t)(tick * portTICK_PERIOD_MS / 1000);
        ESP_LOGI(TAG, "Uptime: %" PRIu32 " seconds", up);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ---------- Step 2: Task manager ----------
static const char* state_to_str(eTaskState s)
{
    switch (s) {
        case eRunning:   return "Running";
        case eReady:     return "Ready";
        case eBlocked:   return "Blocked";
        case eSuspended: return "Suspended";
        case eDeleted:   return "Deleted";
        default:         return "Unknown";
    }
}

void task_manager(void *pvParameters)
{
    ESP_LOGI(TAG, "Task Manager started");
    TaskHandle_t *handles = (TaskHandle_t *)pvParameters;
    TaskHandle_t led1_handle = handles[0];
    TaskHandle_t led2_handle = handles[1];

    int cmd = 0;
    while (1) {
        cmd++;
        switch (cmd % 6) {
            case 1:
                ESP_LOGI(TAG, "Manager: Suspending LED1");
                vTaskSuspend(led1_handle);
                break;
            case 2:
                ESP_LOGI(TAG, "Manager: Resuming LED1");
                vTaskResume(led1_handle);
                break;
            case 3:
                ESP_LOGI(TAG, "Manager: Suspending LED2");
                vTaskSuspend(led2_handle);
                break;
            case 4:
                ESP_LOGI(TAG, "Manager: Resuming LED2");
                vTaskResume(led2_handle);
                break;
            case 5: {
                eTaskState s1 = eTaskGetState(led1_handle);
                eTaskState s2 = eTaskGetState(led2_handle);
                ESP_LOGI(TAG, "LED1 State: %s", state_to_str(s1));
                ESP_LOGI(TAG, "LED2 State: %s", state_to_str(s2));
                break;
            }
            case 0:
                ESP_LOGI(TAG, "Manager: Reset cycle");
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Step 2: (1+2) Basic + Manager ===");

    // GPIO
    gpio_config_t io = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io);
    gpio_set_level(LED1_PIN, 0);
    gpio_set_level(LED2_PIN, 0);

    // Step 1 tasks
    static int  led1_id     = 1;
    static char led2_name[] = "FastBlinker";
    TaskHandle_t h1 = NULL, h2 = NULL, hinfo = NULL;

    // หมายเหตุ: stack size เป็น "words" (2048 words ≈ 8KB บน ESP32)
    xTaskCreate(led1_task,        "LED1_Task",   2048, &led1_id,   2, &h1);
    xTaskCreate(led2_task,        "LED2_Task",   2048, led2_name,  2, &h2);
    xTaskCreate(system_info_task, "SysInfo_Task",3072, NULL,       1, &hinfo);

    // Step 2 task
    TaskHandle_t pack[2] = {h1, h2};
    xTaskCreate(task_manager, "TaskManager", 2048, pack, 3, NULL);

    // main task heartbeat
    while (1) {
        PRINT_HEAP(TAG, "Free heap:", esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}