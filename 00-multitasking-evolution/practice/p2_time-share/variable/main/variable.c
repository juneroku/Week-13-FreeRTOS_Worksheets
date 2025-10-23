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

// Global variables (เหมือน Part 1)
static uint32_t task_counter = 0;
static uint64_t context_switch_time = 0;  // รวมเวลาทำงาน + overhead switch (us)
static uint32_t context_switches = 0;

// ---------- งานจำลอง ----------
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

// ---------- manual scheduler (เหมือน Part 1) ----------
static void manual_scheduler(void)
{
    uint64_t start_time = esp_timer_get_time();

    context_switches++;

    // จำลอง overhead ของ context switch
    for (int i = 0; i < 1000; i++) { volatile int d = i; (void)d; }

    switch (task_counter % TASK_COUNT) {
        case TASK_SENSOR:   simulate_sensor_task();     break;
        case TASK_PROCESS:  simulate_processing_task(); break;
        case TASK_ACTUATOR: simulate_actuator_task();   break;
        case TASK_DISPLAY:  simulate_display_task();    break;
    }

    for (int i = 0; i < 1000; i++) { volatile int d = i; (void)d; }

    uint64_t end_time = esp_timer_get_time();
    context_switch_time += (end_time - start_time);

    task_counter++;
}

// ---------- Part 2: เพิ่มฟังก์ชันทดลอง time slice ----------
static void variable_time_slice_experiment(void)
{
    ESP_LOGI(TAG, "\n=== Variable Time Slice Experiment ===");

    // คุณเวอร์ชันเดิม: รัน 50 รอบต่อ time slice  (หมายเหตุ: จะไม่ได้เท่ากับ 5 วินาทีเสมอ)
    // ถ้าอยากให้ ~5 วินาทีทุกค่า ให้คำนวณ steps = 5000 / time_slice (ดูคอมเมนต์ด้านล่าง)
    uint32_t time_slices[] = {10, 25, 50, 100, 200};
    int num_slices = sizeof(time_slices) / sizeof(time_slices[0]);

    for (int i = 0; i < num_slices; i++) {
        ESP_LOGI(TAG, "Testing time slice: %u ms", time_slices[i]);

        // Reset counters
        context_switches = 0;
        context_switch_time = 0;
        task_counter = 0;

        uint64_t test_start = esp_timer_get_time();

        // Run for 50 รอบ (ตามโค้ดที่คุณส่งมา)
        for (int j = 0; j < 50; j++) {
            manual_scheduler();
            vTaskDelay(pdMS_TO_TICKS(time_slices[i]));
        }

        uint64_t test_end = esp_timer_get_time();
        uint64_t test_duration = test_end - test_start;

        // ประสิทธิภาพโดยประมาณ = เวลาที่ใช้รันงาน/สลับงาน ÷ เวลารวม
        float efficiency = (test_duration > 0)
                         ? ((float)context_switch_time / (float)test_duration) * 100.0f
                         : 0.0f;

        ESP_LOGI(TAG, "Time slice %u ms: Efficiency %.1f%%", time_slices[i], efficiency);
        ESP_LOGI(TAG, "Context switches: %u", context_switches);

        vTaskDelay(pdMS_TO_TICKS(1000)); // พักระหว่างการทดสอบ
    }

}

void app_main(void)
{
    // GPIO Configuration (เหมือน Part 1)
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

    // ======= ส่วน Part 1 (ทำงานปกติพร้อมสรุปทุก 20 สลับ) =======
    for (int warm = 0; warm < 40; ++warm) { // อุ่นเครื่อง ~2 วินาที (40 * 50 ms)
        manual_scheduler();
        vTaskDelay(pdMS_TO_TICKS(TIME_SLICE_MS));

        if (context_switches != 0 && (context_switches % 20) == 0) {
            round_count++;
            uint64_t now = esp_timer_get_time();
            uint64_t total_time = now - start_time;

            float cpu_utilization = (total_time > 0)
                                  ? ((float)context_switch_time / (float)total_time) * 100.0f
                                  : 0.0f;
            float overhead_percentage = 100.0f - cpu_utilization;
            uint64_t avg_time = (context_switches > 0)
                              ? (context_switch_time / context_switches) : 0;

            ESP_LOGI(TAG, "=== Round %u Statistics ===", round_count);
            ESP_LOGI(TAG, "Context switches: %u", context_switches);
            ESP_LOGI(TAG, "Total time: %" PRIu64 " us", total_time);
            ESP_LOGI(TAG, "Task execution time: %" PRIu64 " us", context_switch_time);
            ESP_LOGI(TAG, "CPU utilization: %.1f%%", cpu_utilization);
            ESP_LOGI(TAG, "Overhead: %.1f%%", overhead_percentage);
            ESP_LOGI(TAG, "Avg time per task: %" PRIu64 " us", avg_time);
        }
    }

    // ======= เรียก Part 2: ทดลอง time slice ต่าง ๆ =======
    variable_time_slice_experiment();

    // จบการทดลอง: กระพริบ LED4 ช้า ๆ
    while (1) {
        gpio_set_level(LED4_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        gpio_set_level(LED4_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
}