#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LED1_PIN GPIO_NUM_2
#define LED2_PIN GPIO_NUM_4
#define LED3_PIN GPIO_NUM_5
#define LED4_PIN GPIO_NUM_18

static const char *TAG = "TIME_SHARING";

// Task states
typedef enum {
    TASK_SENSOR,
    TASK_PROCESS,
    TASK_ACTUATOR,
    TASK_DISPLAY,
    TASK_COUNT
} task_id_t;

// Time slice in milliseconds
#define TIME_SLICE_MS 50

// Global variables
static uint32_t task_counter = 0;
static uint64_t context_switch_time = 0;
static uint32_t context_switches = 0;

// Simulate different task workloads
void simulate_sensor_task(void)
{
    static uint32_t sensor_count = 0;
    ESP_LOGI(TAG, "Sensor Task %d", sensor_count++);
    
    gpio_set_level(LED1_PIN, 1);
    
    // Simulate sensor reading (light work)
    for (int i = 0; i < 10000; i++) {
        volatile int dummy = i;
    }
    
    gpio_set_level(LED1_PIN, 0);
}

void simulate_processing_task(void)
{
    static uint32_t process_count = 0;
    ESP_LOGI(TAG, "Processing Task %d", process_count++);
    
    gpio_set_level(LED2_PIN, 1);
    
    // Simulate heavy computation
    for (int i = 0; i < 100000; i++) {
        volatile int dummy = i * i;
    }
    
    gpio_set_level(LED2_PIN, 0);
}

void simulate_actuator_task(void)
{
    static uint32_t actuator_count = 0;
    ESP_LOGI(TAG, "Actuator Task %d", actuator_count++);
    
    gpio_set_level(LED3_PIN, 1);
    
    // Simulate actuator control (medium work)
    for (int i = 0; i < 50000; i++) {
        volatile int dummy = i + 100;
    }
    
    gpio_set_level(LED3_PIN, 0);
}

void simulate_display_task(void)
{
    static uint32_t display_count = 0;
    ESP_LOGI(TAG, "Display Task %d", display_count++);
    
    gpio_set_level(LED4_PIN, 1);
    
    // Simulate display update (quick work)
    for (int i = 0; i < 20000; i++) {
        volatile int dummy = i / 2;
    }
    
    gpio_set_level(LED4_PIN, 0);
}

// Manual context switching
void manual_scheduler(void)
{
    uint64_t start_time = esp_timer_get_time();
    
    // Save context (simulated)
    context_switches++;
    
    // Context switch overhead simulation
    for (int i = 0; i < 1000; i++) {
        volatile int dummy = i;
    }
    
    // Execute current task
    switch (task_counter % TASK_COUNT) {
        case TASK_SENSOR:
            simulate_sensor_task();
            break;
        case TASK_PROCESS:
            simulate_processing_task();
            break;
        case TASK_ACTUATOR:
            simulate_actuator_task();
            break;
        case TASK_DISPLAY:
            simulate_display_task();
            break;
    }
    
    // More context switch overhead
    for (int i = 0; i < 1000; i++) {
        volatile int dummy = i;
    }
    
    uint64_t end_time = esp_timer_get_time();
    context_switch_time += (end_time - start_time);
    
    task_counter++;
}

void app_main(void)
{
    // GPIO Configuration
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN) | 
                        (1ULL << LED3_PIN) | (1ULL << LED4_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Time-Sharing System Started");
    ESP_LOGI(TAG, "Time slice: %d ms", TIME_SLICE_MS);

    uint64_t start_time = esp_timer_get_time();
    uint32_t round_count = 0;

    while (1) {
        manual_scheduler();
        
        // Wait for time slice
        vTaskDelay(pdMS_TO_TICKS(TIME_SLICE_MS));
        
        // Report statistics every 20 context switches
        if (context_switches % 20 == 0) {
            round_count++;
            uint64_t current_time = esp_timer_get_time();
            uint64_t total_time = current_time - start_time;
            
            float cpu_utilization = ((float)(context_switch_time) / (float)(total_time)) * 100.0f;
            float overhead_percentage = 100.0f - cpu_utilization;
            
            ESP_LOGI(TAG, "=== Round %d Statistics ===", round_count);
            ESP_LOGI(TAG, "Context switches: %d", context_switches);
            ESP_LOGI(TAG, "Total time: %lld us", total_time);
            ESP_LOGI(TAG, "Task execution time: %lld us", context_switch_time);
            ESP_LOGI(TAG, "CPU utilization: %.1f%%", cpu_utilization);
            ESP_LOGI(TAG, "Overhead: %.1f%%", overhead_percentage);
            ESP_LOGI(TAG, "Avg time per task: %lld us", context_switch_time / context_switches);
        }
    }
}