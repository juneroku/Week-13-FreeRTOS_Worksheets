#include <stdio.h> 
#include <stdint.h>
#include <string.h>
#include <stdbool.h>            // ใช้ชนิด bool ใน user_input_t
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "QUEUE_SETS";

// LED indicators
#define LED_SENSOR     GPIO_NUM_2
#define LED_USER       GPIO_NUM_4
#define LED_NETWORK    GPIO_NUM_5
#define LED_TIMER      GPIO_NUM_18
#define LED_PROCESSOR  GPIO_NUM_19

// Queue handles
QueueHandle_t xSensorQueue;
QueueHandle_t xUserQueue;
QueueHandle_t xNetworkQueue;
SemaphoreHandle_t xTimerSemaphore;

// Queue Set handle
QueueSetHandle_t xQueueSet;

// ========== Data structures ==========
typedef struct {
    int sensor_id;
    float temperature;
    float humidity;
    uint32_t timestamp;
} sensor_data_t;

typedef struct {
    int button_id;
    bool pressed;
    uint32_t duration_ms;
} user_input_t;

typedef struct {
    char source[20];
    char message[100];
    int priority;
} network_message_t;

typedef enum { MSG_SENSOR, MSG_USER, MSG_NETWORK, MSG_TIMER } message_type_t;

typedef struct {
    uint32_t sensor_count;
    uint32_t user_count;
    uint32_t network_count;
    uint32_t timer_count;
} message_stats_t;

message_stats_t stats = {0, 0, 0, 0};

// ========== Tasks ==========

// (ยังมีให้เผื่อเปิดใช้ภายหลัง แต่ในทดลองนี้ปิดไม่สร้าง task)
void sensor_task(void *pvParameters) {
    sensor_data_t sensor_data;
    int sensor_id = 1;
    ESP_LOGI(TAG, "Sensor task started");
    while (1) {
        sensor_data.sensor_id = sensor_id;
        sensor_data.temperature = 20.0 + (esp_random() % 200) / 10.0; // 20-40°C
        sensor_data.humidity    = 30.0 + (esp_random() % 400) / 10.0; // 30-70%
        sensor_data.timestamp   = xTaskGetTickCount();

        if (xQueueSend(xSensorQueue, &sensor_data, pdMS_TO_TICKS(100)) == pdPASS) {
            ESP_LOGI(TAG, "📊 Sensor: T=%.1f°C, H=%.1f%%, ID=%d",
                     sensor_data.temperature, sensor_data.humidity, sensor_id);
            gpio_set_level(LED_SENSOR, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(LED_SENSOR, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 3000)));
    }
}

void user_input_task(void *pvParameters) {
    user_input_t user_input;
    ESP_LOGI(TAG, "User input task started");
    while (1) {
        user_input.button_id  = 1 + (esp_random() % 3);        // 1..3
        user_input.pressed    = true;
        user_input.duration_ms= 100 + (esp_random() % 1000);   // 100..1100
        if (xQueueSend(xUserQueue, &user_input, pdMS_TO_TICKS(100)) == pdPASS) {
            ESP_LOGI(TAG, "🔘 User: Button %d pressed for %dms",
                     user_input.button_id, user_input.duration_ms);
            gpio_set_level(LED_USER, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_USER, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 5000)));
    }
}

void network_task(void *pvParameters) {
    network_message_t network_msg;
    const char* sources[]  = {"WiFi", "Bluetooth", "LoRa", "Ethernet"};
    const char* messages[] = {
        "Status update received",
        "Configuration changed",
        "Alert notification",
        "Data synchronization",
        "Heartbeat signal"
    };
    ESP_LOGI(TAG, "Network task started (freq=0.5s)");

    while (1) {
        strcpy(network_msg.source,  sources[esp_random() % 4]);
        strcpy(network_msg.message, messages[esp_random() % 5]);
        network_msg.priority = 1 + (esp_random() % 5); // 1..5

        if (xQueueSend(xNetworkQueue, &network_msg, pdMS_TO_TICKS(100)) == pdPASS) {
            ESP_LOGI(TAG, "🌐 Network [%s]: %s (P:%d)",
                     network_msg.source, network_msg.message, network_msg.priority);
            gpio_set_level(LED_NETWORK, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(LED_NETWORK, 0);
        }

        // ★ ทดลองที่ 3: เพิ่มความถี่ — ส่งทุก 0.5 วินาที
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void timer_task(void *pvParameters) {
    ESP_LOGI(TAG, "Timer task started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // Every 10s
        if (xSemaphoreGive(xTimerSemaphore) == pdPASS) {
            ESP_LOGI(TAG, "⏰ Timer: Periodic timer fired");
            gpio_set_level(LED_TIMER, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_TIMER, 0);
        }
    }
}

void processor_task(void *pvParameters) {
    QueueSetMemberHandle_t xActivatedMember;
    sensor_data_t      sensor_data;
    user_input_t       user_input;
    network_message_t  network_msg;

    ESP_LOGI(TAG, "Processor task started - waiting for events...");
    while (1) {
        xActivatedMember = xQueueSelectFromSet(xQueueSet, portMAX_DELAY);
        if (xActivatedMember != NULL) {
            gpio_set_level(LED_PROCESSOR, 1);

            if (xActivatedMember == xSensorQueue) {
                if (xQueueReceive(xSensorQueue, &sensor_data, 0) == pdPASS) {
                    stats.sensor_count++;
                    ESP_LOGI(TAG, "→ SENSOR: T=%.1f°C, H=%.1f%%",
                             sensor_data.temperature, sensor_data.humidity);
                    if (sensor_data.temperature > 35.0) ESP_LOGW(TAG, "⚠️  High temperature!");
                    if (sensor_data.humidity    > 60.0) ESP_LOGW(TAG, "⚠️  High humidity!");
                }
            } else if (xActivatedMember == xUserQueue) {
                if (xQueueReceive(xUserQueue, &user_input, 0) == pdPASS) {
                    stats.user_count++;
                    ESP_LOGI(TAG, "→ USER: Button %d (%dms)",
                             user_input.button_id, user_input.duration_ms);
                    switch (user_input.button_id) {
                        case 1: ESP_LOGI(TAG, "💡 Toggle LED"); break;
                        case 2: ESP_LOGI(TAG, "📊 Show status"); break;
                        case 3: ESP_LOGI(TAG, "⚙️  Settings menu"); break;
                    }
                }
            } else if (xActivatedMember == xNetworkQueue) {
                if (xQueueReceive(xNetworkQueue, &network_msg, 0) == pdPASS) {
                    stats.network_count++;
                    ESP_LOGI(TAG, "→ NETWORK: [%s] %s (P:%d)",
                             network_msg.source, network_msg.message, network_msg.priority);
                    if (network_msg.priority >= 4) ESP_LOGW(TAG, "🚨 High priority network!");
                }
            } else if (xActivatedMember == xTimerSemaphore) {
                if (xSemaphoreTake(xTimerSemaphore, 0) == pdPASS) {
                    stats.timer_count++;
                    ESP_LOGI(TAG, "→ TIMER: Periodic maintenance");
                    ESP_LOGI(TAG, "📈 Stats - Sensor:%lu, User:%lu, Network:%lu, Timer:%lu",
                             stats.sensor_count, stats.user_count,
                             stats.network_count, stats.timer_count);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(200));  // simulate processing
            gpio_set_level(LED_PROCESSOR, 0);
        }
    }
}

void monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "System monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000)); // Every 15s
        ESP_LOGI(TAG, "\n═══ SYSTEM MONITOR ═══");
        ESP_LOGI(TAG, "Queue States:");
        ESP_LOGI(TAG, "  Sensor Queue:  %d/%d",  uxQueueMessagesWaiting(xSensorQueue), 5);
        ESP_LOGI(TAG, "  User Queue:    %d/%d",  uxQueueMessagesWaiting(xUserQueue),   3);
        ESP_LOGI(TAG, "  Network Queue: %d/%d",  uxQueueMessagesWaiting(xNetworkQueue),8);
        ESP_LOGI(TAG, "Message Statistics:");
        ESP_LOGI(TAG, "  Sensor:  %lu messages",  stats.sensor_count);
        ESP_LOGI(TAG, "  User:    %lu messages",  stats.user_count);
        ESP_LOGI(TAG, "  Network: %lu messages",  stats.network_count);
        ESP_LOGI(TAG, "  Timer:   %lu events",    stats.timer_count);
        ESP_LOGI(TAG, "═══════════════════════\n");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Queue Sets Lab — Experiment 3: Increase Network Frequency (0.5s) [Sensor disabled]");

    // Configure LED pins
    gpio_set_direction(LED_SENSOR,     GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_USER,       GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_NETWORK,    GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TIMER,      GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PROCESSOR,  GPIO_MODE_OUTPUT);

    // Turn off all LEDs
    gpio_set_level(LED_SENSOR, 0);
    gpio_set_level(LED_USER, 0);
    gpio_set_level(LED_NETWORK, 0);
    gpio_set_level(LED_TIMER, 0);
    gpio_set_level(LED_PROCESSOR, 0);

    // Create queues & semaphore
    xSensorQueue     = xQueueCreate(5, sizeof(sensor_data_t));
    xUserQueue       = xQueueCreate(3, sizeof(user_input_t));
    xNetworkQueue    = xQueueCreate(8, sizeof(network_message_t));
    xTimerSemaphore  = xSemaphoreCreateBinary();

    // Queue set capacity = sum of member capacities
    xQueueSet = xQueueCreateSet(5 + 3 + 8 + 1);

    if (xSensorQueue && xUserQueue && xNetworkQueue && xTimerSemaphore && xQueueSet) {
        // Add members into the set
        if (xQueueAddToSet(xSensorQueue,    xQueueSet) != pdPASS ||
            xQueueAddToSet(xUserQueue,      xQueueSet) != pdPASS ||
            xQueueAddToSet(xNetworkQueue,   xQueueSet) != pdPASS ||
            xQueueAddToSet(xTimerSemaphore, xQueueSet) != pdPASS) {
            ESP_LOGE(TAG, "Failed to add queues to queue set!");
            return;
        }

        ESP_LOGI(TAG, "Queue set created and configured successfully");

        // === Create sources (Sensor ปิดไว้) ===
        // xTaskCreate(sensor_task, "Sensor", 2048, NULL, 3, NULL);   // ← ปิดใช้งานตามทดลอง 2
        xTaskCreate(user_input_task, "UserInput", 2048, NULL, 3, NULL);
        xTaskCreate(network_task,    "Network",   2048, NULL, 3, NULL);
        xTaskCreate(timer_task,      "Timer",     2048, NULL, 2, NULL);

        // Processor & Monitor
        xTaskCreate(processor_task,  "Processor", 3072, NULL, 4, NULL);
        xTaskCreate(monitor_task,    "Monitor",   2048, NULL, 1, NULL);

        ESP_LOGI(TAG, "All tasks created. System operational.");

        // Simple LED startup animation
        for (int i = 0; i < 3; i++) {
            gpio_set_level(LED_SENSOR, 1);   vTaskDelay(pdMS_TO_TICKS(100)); gpio_set_level(LED_SENSOR, 0);
            gpio_set_level(LED_USER, 1);     vTaskDelay(pdMS_TO_TICKS(100)); gpio_set_level(LED_USER, 0);
            gpio_set_level(LED_NETWORK, 1);  vTaskDelay(pdMS_TO_TICKS(100)); gpio_set_level(LED_NETWORK, 0);
            gpio_set_level(LED_TIMER, 1);    vTaskDelay(pdMS_TO_TICKS(100)); gpio_set_level(LED_TIMER, 0);
            gpio_set_level(LED_PROCESSOR, 1);vTaskDelay(pdMS_TO_TICKS(100)); gpio_set_level(LED_PROCESSOR, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    } else {
        ESP_LOGE(TAG, "Failed to create queues or queue set!");
    }
}