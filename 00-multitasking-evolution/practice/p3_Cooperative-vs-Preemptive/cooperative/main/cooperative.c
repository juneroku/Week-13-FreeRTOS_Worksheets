#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

// ====================== เลือกโหมดทดสอบ ======================
#define DEMO_MODE_PREEMPTIVE  1   // 1 = Preemptive, 0 = Cooperative

// ====================== Pin mapping ==========================
#define LED1_PIN    GPIO_NUM_2
#define LED2_PIN    GPIO_NUM_4
#define LED3_PIN    GPIO_NUM_5
#define BUTTON_PIN  GPIO_NUM_33   // ใช้ GPIO33 (มี pull-up ในตัว, ไม่ใช่ขา strap)

// ====================== Debounce =============================
#define DEBOUNCE_MS 50

static inline bool button_pressed_edge(void)
{
    static int last = 1; // pull-up: 1=not pressed, 0=pressed
    static uint64_t last_ts = 0;

    int now = gpio_get_level(BUTTON_PIN);
    uint64_t t  = esp_timer_get_time();         // us
    uint64_t dt = (t - last_ts) / 1000U;        // ms

    bool edge = false;
    if (last == 1 && now == 0 && dt > DEBOUNCE_MS) {
        edge = true;            // ติดขอบตก พร้อม debounce
        last_ts = t;
    }
    if (now != last) {
        last_ts = t;
        last    = now;
    }
    return edge;
}

// ====================== Cooperative ==========================
static const char *COOP_TAG = "COOPERATIVE";
static volatile bool coop_emergency = false;
static uint64_t coop_start_time = 0;
static uint32_t coop_max_resp_ms = 0;

static void cooperative_task1(void)
{
    static uint32_t count = 0;
    ESP_LOGI(COOP_TAG, "Coop Task1 running: %u", count++);
    gpio_set_level(LED1_PIN, 1);

    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 50000; j++) { volatile int d = j * 2; (void)d; }
        if (coop_emergency) { ESP_LOGW(COOP_TAG, "Task1 yield for emergency"); gpio_set_level(LED1_PIN, 0); return; }
        vTaskDelay(1); // ยอมสละ CPU
    }
    gpio_set_level(LED1_PIN, 0);
}

static void cooperative_task2(void)
{
    static uint32_t count = 0;
    ESP_LOGI(COOP_TAG, "Coop Task2 running: %u", count++);
    gpio_set_level(LED2_PIN, 1);

    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 30000; j++) { volatile int d = j + i; (void)d; }
        if (coop_emergency) { ESP_LOGW(COOP_TAG, "Task2 yield for emergency"); gpio_set_level(LED2_PIN, 0); return; }
        vTaskDelay(1);
    }
    gpio_set_level(LED2_PIN, 0);
}

static void cooperative_task3_emergency(void)
{
    if (!coop_emergency) return;

    uint64_t dt_us = esp_timer_get_time() - coop_start_time;
    uint32_t dt_ms = (uint32_t)(dt_us / 1000U);
    if (dt_ms > coop_max_resp_ms) coop_max_resp_ms = dt_ms;

    ESP_LOGW(COOP_TAG, "EMERGENCY! Response: %u ms (Max: %u ms)", dt_ms, coop_max_resp_ms);

    gpio_set_level(LED3_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(LED3_PIN, 0);

    coop_emergency = false;
}

static void coop_scheduler_task(void *arg)
{
    typedef struct { void (*fn)(void); const char* name; bool ready; } coop_task_t;
    coop_task_t tasks[] = {
        { cooperative_task1,           "Task1",     true },
        { cooperative_task2,           "Task2",     true },
        { cooperative_task3_emergency, "Emergency", true },
    };
    const int N = sizeof(tasks)/sizeof(tasks[0]);
    int cur = 0;

    ESP_LOGI(COOP_TAG, "=== Cooperative Multitasking Demo ===");
    ESP_LOGI(COOP_TAG, "Tasks yield voluntarily. Press button (GPIO33→GND).");

    for (;;) {
        if (button_pressed_edge() && !coop_emergency) {
            coop_emergency = true;
            coop_start_time = esp_timer_get_time();
            ESP_LOGW(COOP_TAG, "Emergency button pressed!");
        }

        if (tasks[cur].ready) tasks[cur].fn();
        cur = (cur + 1) % N;

        vTaskDelay(pdMS_TO_TICKS(10)); // กัน WDT และลดภาระ
    }
}

// ====================== Preemptive ===========================
static const char *PREEMPT_TAG = "PREEMPTIVE";
static uint64_t preempt_start_time = 0;
static uint32_t preempt_max_resp_ms = 0;

static void preemptive_task1(void *arg)
{
    uint32_t count = 0;
    for (;;) {
        ESP_LOGI(PREEMPT_TAG, "Preempt Task1: %u", count++);
        gpio_set_level(LED1_PIN, 1);

        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 50000; j++) { volatile int d = j * 2; (void)d; }

        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void preemptive_task2(void *arg)
{
    uint32_t count = 0;
    for (;;) {
        ESP_LOGI(PREEMPT_TAG, "Preempt Task2: %u", count++);
        gpio_set_level(LED2_PIN, 1);

        for (int i = 0; i < 20; i++)
            for (int j = 0; j < 30000; j++) { volatile int d = j + i; (void)d; }

        gpio_set_level(LED2_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void preemptive_emergency_task(void *arg)
{
    for (;;) {
        if (button_pressed_edge()) {
            preempt_start_time = esp_timer_get_time();

            // งานนี้ priority สูง — จะวิ่งทันทีที่เกิด tick ถัดไป
            uint64_t dt_us = esp_timer_get_time() - preempt_start_time;
            uint32_t dt_ms = (uint32_t)(dt_us / 1000U);
            if (dt_ms > preempt_max_resp_ms) preempt_max_resp_ms = dt_ms;

            ESP_LOGW(PREEMPT_TAG, "IMMEDIATE EMERGENCY! Response: %u ms (Max: %u ms)",
                     dt_ms, preempt_max_resp_ms);

            gpio_set_level(LED3_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED3_PIN, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(5)); // polling ช่วงสั้น ๆ
    }
}

static void start_preemptive(void)
{
    ESP_LOGI(PREEMPT_TAG, "=== Preemptive Multitasking Demo ===");
    ESP_LOGI(PREEMPT_TAG, "Press button (GPIO33→GND) for emergency.");

    // priority: emergency(5) > task1(2) > task2(1)
    xTaskCreate(preemptive_task1,           "pre_t1",  2048, NULL, 2, NULL);
    xTaskCreate(preemptive_task2,           "pre_t2",  2048, NULL, 1, NULL);
    xTaskCreate(preemptive_emergency_task,  "pre_emg", 2048, NULL, 5, NULL);
}

// ====================== app_main ============================
void app_main(void)
{
    // LED outputs
    gpio_config_t io = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN) | (1ULL << LED3_PIN),
        .pull_down_en = 0,
        .pull_up_en   = 0,
    };
    gpio_config(&io);
    gpio_set_level(LED1_PIN, 0);
    gpio_set_level(LED2_PIN, 0);
    gpio_set_level(LED3_PIN, 0);

    // Button input (pull-up internal, active-low)
    io.mode         = GPIO_MODE_INPUT;
    io.pin_bit_mask = (1ULL << BUTTON_PIN);
    io.pull_up_en   = 1;
    io.pull_down_en = 0;
    gpio_config(&io);

    ESP_LOGI("MAIN", "Multitasking Comparison Demo (BTN on GPIO33)");
    ESP_LOGI("MAIN", "Mode: %s (change DEMO_MODE_PREEMPTIVE to switch)",
             DEMO_MODE_PREEMPTIVE ? "PREEMPTIVE" : "COOPERATIVE");

#if DEMO_MODE_PREEMPTIVE
    start_preemptive();
#else
    // รัน scheduler แบบ cooperative ใน FreeRTOS task (กัน app_main คืนค่า)
    xTaskCreate(coop_scheduler_task, "coop", 4096, NULL, 2, NULL);
#endif

    // กัน app_main() คืนค่า — อยู่หลับตลอดไป
    for (;;)
        vTaskDelay(portMAX_DELAY);
}