#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"


#define LED1_PIN    GPIO_NUM_16
#define LED2_PIN    GPIO_NUM_17
#define LED3_PIN    GPIO_NUM_18
#define BUTTON_PIN  GPIO_NUM_23   


static const char *PREEMPT_TAG = "PREEMPTIVE";
static TaskHandle_t sEmergencyTask = NULL;
static volatile uint32_t s_press_tick = 0;
static volatile uint32_t s_last_isr_tick = 0;   // debounce
static uint32_t s_max_response_ms = 0;


static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    uint32_t now = xTaskGetTickCountFromISR();

    // debounce ~30ms
    if ((now - s_last_isr_tick) < pdMS_TO_TICKS(30)) {
        return;
    }
    s_last_isr_tick = now;
    s_press_tick = now;

    BaseType_t hpw = pdFALSE;
    vTaskNotifyGiveFromISR(sEmergencyTask, &hpw);
    if (hpw == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ========= งานปกติ 1 (priority 2) ========= */
static void preemptive_task1(void *pvParameters)
{
    (void)pvParameters;
    uint32_t count = 0;
    while (1) {
        ESP_LOGI(PREEMPT_TAG, "Preempt Task1: %u", count++);
        gpio_set_level(LED1_PIN, 1);

        // ทำงานหนักแบบไม่ yield ให้เห็นการ preempt
        for (int i = 0; i < 5; i++) {
            for (int j = 0; j < 50000; j++) {
                volatile int dummy = j * 2; (void)dummy;
            }
        }

        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ========= งานปกติ 2 (priority 1) ========= */
static void preemptive_task2(void *pvParameters)
{
    (void)pvParameters;
    uint32_t count = 0;
    while (1) {
        ESP_LOGI(PREEMPT_TAG, "Preempt Task2: %u", count++);
        gpio_set_level(LED2_PIN, 1);

        // งานยาวกว่าเล็กน้อย
        for (int i = 0; i < 20; i++) {
            for (int j = 0; j < 30000; j++) {
                volatile int dummy = j + i; (void)dummy;
            }
        }

        gpio_set_level(LED2_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

/* ========= งานฉุกเฉิน (priority 5 สูง) =========
   รอ notification จาก ISR → คำนวณ latency จาก tick ที่ ISR จับไว้ */
static void preemptive_emergency_task(void *pvParameters)
{
    (void)pvParameters;
    while (1) {
        // บล็อกจนถูกปลุกจาก ISR
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t now_tick = xTaskGetTickCount();
        uint32_t diff_tick = now_tick - s_press_tick;
        uint32_t ms = diff_tick * portTICK_PERIOD_MS;

        if (ms > s_max_response_ms) s_max_response_ms = ms;

        ESP_LOGW(PREEMPT_TAG, "IMMEDIATE EMERGENCY! Response=%u ms (Max=%u ms)",
                 ms, s_max_response_ms);

        gpio_set_level(LED3_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(LED3_PIN, 0);
    }
}

/* ========= สร้างงานทั้งหมด ========= */
static void test_preemptive_multitasking(void)
{
    ESP_LOGI(PREEMPT_TAG, "=== Preemptive Multitasking Demo ===");
    ESP_LOGI(PREEMPT_TAG, "Press BUTTON (GPIO23→GND) to trigger emergency.");

    xTaskCreate(preemptive_task1,          "PreTask1",  3072, NULL, 2, NULL);
    xTaskCreate(preemptive_task2,          "PreTask2",  3072, NULL, 1, NULL);
    xTaskCreate(preemptive_emergency_task, "Emergency", 3072, NULL, 5, &sEmergencyTask);
}

/* ========= app_main: ตั้ง GPIO, ติด ISR, แล้วไม่ return ========= */
void app_main(void)
{
    // LED outputs
    gpio_config_t io = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN) | (1ULL << LED3_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io);

    // BUTTON input with pull-up + interrupt on falling edge
    io.intr_type = GPIO_INTR_NEGEDGE;
    io.mode = GPIO_MODE_INPUT;
    io.pin_bit_mask = (1ULL << BUTTON_PIN);
    io.pull_up_en = 1;
    io.pull_down_en = 0;
    gpio_config(&io);

    // สร้างงานก่อน แล้วค่อยติด ISR (กัน ISR ปลุกตอน handle ยังไม่พร้อม)
    test_preemptive_multitasking();

    // ติดตั้ง ISR service แล้วผูก handler
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(BUTTON_PIN, button_isr, NULL);

    ESP_LOGI("MAIN", "Pins: LED1=16, LED2=17, LED3=18, BUTTON=23 (Active-LOW w/ ISR)");
    ESP_LOGI("MAIN", "Ready. Press the button.");

    // สำคัญ: อย่าปล่อยให้ app_main() return
    vTaskDelete(NULL);
}