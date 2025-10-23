#include <stdio.h>
#include <inttypes.h>
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

// Task states/IDs
typedef enum {
    TASK_SENSOR,
    TASK_PROCESS,
    TASK_ACTUATOR,
    TASK_DISPLAY,
    TASK_COUNT
} task_id_t;

// Fixed time slice (ส่วน Part 1)
#define TIME_SLICE_MS 50

// Metrics (เหมือน Part 1/2)
static uint32_t task_counter = 0;
static uint64_t context_switch_time = 0;  // us: เวลาที่ใช้ “งาน + overhead สลับ”
static uint32_t context_switches = 0;

// ---------------- งานจำลองแต่ละ task ----------------
static void simulate_sensor_task(void)
{
    static uint32_t sensor_count = 0;
    ESP_LOGI(TAG, "Sensor Task %u", sensor_count++);
    gpio_set_level(LED1_PIN, 1);
    for (int i = 0; i < 10000; i++) { volatile int dummy = i; (void)dummy; }
    gpio_set_level(LED1_PIN, 0);
}

static void simulate_processing_task(void)
{
    static uint32_t process_count = 0;
    ESP_LOGI(TAG, "Processing Task %u", process_count++);
    gpio_set_level(LED2_PIN, 1);
    for (int i = 0; i < 100000; i++) { volatile int dummy = i * i; (void)dummy; }
    gpio_set_level(LED2_PIN, 0);
}

static void simulate_actuator_task(void)
{
    static uint32_t actuator_count = 0;
    ESP_LOGI(TAG, "Actuator Task %u", actuator_count++);
    gpio_set_level(LED3_PIN, 1);
    for (int i = 0; i < 50000; i++) { volatile int dummy = i + 100; (void)dummy; }
    gpio_set_level(LED3_PIN, 0);
}

static void simulate_display_task(void)
{
    static uint32_t display_count = 0;
    ESP_LOGI(TAG, "Display Task %u", display_count++);
    gpio_set_level(LED4_PIN, 1);
    for (int i = 0; i < 20000; i++) { volatile int dummy = i >> 1; (void)dummy; }
    gpio_set_level(LED4_PIN, 0);
}

// ---------------- manual scheduler (Part 1/2 เหมือนเดิม) ----------------
static void manual_scheduler(void)
{
    uint64_t start_time = esp_timer_get_time();

    context_switches++;

    // จำลอง overhead สลับงาน
    for (int i = 0; i < 1000; i++) { volatile int d = i; (void)d; }

    switch (task_counter % TASK_COUNT) {
        case TASK_SENSOR:   simulate_sensor_task();     break;
        case TASK_PROCESS:  simulate_processing_task(); break;
        case TASK_ACTUATOR: simulate_actuator_task();   break;
        case TASK_DISPLAY:  simulate_display_task();    break;
    }

    // overhead อีกฝั่ง
    for (int i = 0; i < 1000; i++) { volatile int d = i; (void)d; }

    uint64_t end_time = esp_timer_get_time();
    context_switch_time += (end_time - start_time);

    task_counter++;
}

// ---------------- Part 2: Variable time slice experiment ----------------
static void variable_time_slice_experiment(void)
{
    ESP_LOGI(TAG, "\n=== Variable Time Slice Experiment ===");

    uint32_t time_slices[] = {10, 25, 50, 100, 200};
    int num_slices = sizeof(time_slices) / sizeof(time_slices[0]);

    for (int i = 0; i < num_slices; i++) {
        ESP_LOGI(TAG, "Testing time slice: %u ms", time_slices[i]);

        // reset metrics
        context_switches = 0;
        context_switch_time = 0;
        task_counter = 0;

        uint64_t test_start = esp_timer_get_time();

        // รัน 50 รอบต่อค่า (ตามโค้ดต้นฉบับที่คุณส่งมา)
        for (int j = 0; j < 50; j++) {
            manual_scheduler();
            vTaskDelay(pdMS_TO_TICKS(time_slices[i]));
        }

        uint64_t test_end = esp_timer_get_time();
        uint64_t test_duration = test_end - test_start;

        float efficiency = (test_duration > 0)
                         ? ((float)context_switch_time / (float)test_duration) * 100.0f
                         : 0.0f;

        ESP_LOGI(TAG, "Time slice %u ms: Efficiency %.1f%%", time_slices[i], efficiency);
        ESP_LOGI(TAG, "Context switches: %u", context_switches);

        vTaskDelay(pdMS_TO_TICKS(1000)); // พักระหว่างการทดสอบ
    }
}

// ---------------- Part 3: Problem demonstration (เพิ่มส่วนนี้) ----------------
static void demonstrate_problems(void)
{
    ESP_LOGI(TAG, "\n=== Demonstrating Time-Sharing Problems ===");

    // Problem 1: Priority Inversion
    ESP_LOGI(TAG, "Problem 1: No priority support");
    ESP_LOGI(TAG, "Critical task must wait for less important tasks");

    // Problem 2: Fixed time slice issues
    ESP_LOGI(TAG, "Problem 2: Fixed time slice problems");
    ESP_LOGI(TAG, "Short tasks waste time, long tasks get interrupted");

    // Problem 3: Context switching overhead
    ESP_LOGI(TAG, "Problem 3: Context switching overhead");
    ESP_LOGI(TAG, "Time wasted in switching between tasks");

    // Problem 4: No inter-task communication
    ESP_LOGI(TAG, "Problem 4: No proper inter-task communication");
    ESP_LOGI(TAG, "Tasks cannot communicate safely");
}

void app_main(void)
{
    // ตั้งค่า GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN) |
                        (1ULL << LED3_PIN) | (1ULL << LED4_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Time-Sharing System Started (Part 1+2+3)");
    ESP_LOGI(TAG, "Base time slice: %d ms", TIME_SLICE_MS);

    // อุ่นเครื่องแบบ Part 1 สั้น ๆ แล้วสรุปสถิติรอบหนึ่ง
    context_switches = 0;
    context_switch_time = 0;
    task_counter = 0;

    uint64_t start_time = esp_timer_get_time();
    for (int warm = 0; warm < 40; ++warm) { // ~2 วินาทีถ้า TIME_SLICE_MS=50
        manual_scheduler();
        vTaskDelay(pdMS_TO_TICKS(TIME_SLICE_MS));
    }
    uint64_t end_time = esp_timer_get_time();
    uint64_t total_time = end_time - start_time;
    float cpu_util = (total_time > 0)
                   ? ((float)context_switch_time / (float)total_time) * 100.0f
                   : 0.0f;

    ESP_LOGI(TAG, "Warmup stats: utilization=%.1f%%, switches=%u, time=%" PRIu64 "us",
             cpu_util, context_switches, total_time);

    // Part 2: ทดลอง time slice ต่าง ๆ
    variable_time_slice_experiment();

    // Part 3: สรุปปัญหาของ time-sharing แบบ manual
    demonstrate_problems();

    // จบ: กระพริบ LED4 ช้า ๆ
    while (1) {
        gpio_set_level(LED4_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        gpio_set_level(LED4_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
}