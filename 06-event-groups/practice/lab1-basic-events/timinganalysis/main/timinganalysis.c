
// main.c — Event Groups Lab (Experiment 4: Timing Analysis)
// อิงโค้ดเดิม เพิ่มชุด CONFIG สำหรับตั้งเวลา init/timeout เพื่อทดสอบ timeout behaviors

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_random.h"
#include "driver/gpio.h"

static const char *TAG = "EVENT_GROUPS_TIMING";

// ===================== EXPERIMENT CONFIG =====================
// ปรับ “เวลาเริ่มต้นย่อยแต่ละ subsystem” เพื่อกระตุ้น/ไม่กระตุ้น timeout
#define NETWORK_INIT_MS      5000   // ← เพิ่มเป็น 5 วินาที ตามโจทย์
#define SENSOR_INIT_MS       3500
#define CONFIG_INIT_MS       1200
#define STORAGE_INIT_MS      3300

// ปรับ “เวลา timeout ของ coordinator”
#define PHASE1_TIMEOUT_MS    3000   // รอ Network+Config (ตั้งให้ timeout แน่ เพราะ Network=5000)
#define PHASE2_TIMEOUT_MS    4000   // รอทุก subsystem (ก็จะ timeout เพราะ Network=5000)

// ช่วง Heartbeat/Monitoring
#define NET_HEARTBEAT_MS     5000
#define SENSOR_POLL_MS       3000
#define CONFIG_MON_MS        8000
#define STORAGE_MAINT_MS     10000
#define SYS_MONITOR_MS       5000
#define EVT_MONITOR_MS       8000

// ===================== GPIO =====================
#define LED_NETWORK_READY   GPIO_NUM_2
#define LED_SENSOR_READY    GPIO_NUM_4
#define LED_CONFIG_READY    GPIO_NUM_5
#define LED_STORAGE_READY   GPIO_NUM_18
#define LED_SYSTEM_READY    GPIO_NUM_19

// ===================== Event Groups =====================
EventGroupHandle_t system_events;

#define NETWORK_READY_BIT   (1 << 0)
#define SENSOR_READY_BIT    (1 << 1)
#define CONFIG_READY_BIT    (1 << 2)
#define STORAGE_READY_BIT   (1 << 3)
#define SYSTEM_READY_BIT    (1 << 4)

#define BASIC_SYSTEM_BITS   (NETWORK_READY_BIT | CONFIG_READY_BIT)
#define ALL_SUBSYSTEM_BITS  (NETWORK_READY_BIT | SENSOR_READY_BIT | CONFIG_READY_BIT | STORAGE_READY_BIT)
#define FULL_SYSTEM_BITS    (ALL_SUBSYSTEM_BITS | SYSTEM_READY_BIT)

// ===================== Stats =====================
typedef struct {
    // init times
    uint32_t network_init_time;
    uint32_t sensor_init_time;
    uint32_t config_init_time;
    uint32_t storage_init_time;
    uint32_t total_init_time;

    // timeouts counters
    uint32_t phase1_timeouts;
    uint32_t phase2_timeouts;

    // late arrivals (ms) หลัง timeout
    uint32_t network_late_ms;
    uint32_t sensor_late_ms;
    uint32_t config_late_ms;
    uint32_t storage_late_ms;

    uint32_t event_notifications;
} system_stats_t;

static system_stats_t stats = {0};

// helper: millis since boot (approx from tick)
static inline uint32_t ms_now(void) {
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// ===================== Network =====================
void network_init_task(void *pvParameters) {
    ESP_LOGI(TAG, "🌐 Network initialization started");
    uint32_t t0 = ms_now();

    ESP_LOGI(TAG, "Initializing WiFi driver...");
    vTaskDelay(pdMS_TO_TICKS(NETWORK_INIT_MS)); // ← ใช้ค่าทดลอง

    stats.network_init_time = ms_now() - t0;
    gpio_set_level(LED_NETWORK_READY, 1);
    xEventGroupSetBits(system_events, NETWORK_READY_BIT);
    ESP_LOGI(TAG, "✅ Network ready! (took %lu ms)", stats.network_init_time);

    while (1) {
        // heartbeat เฉย ๆ เพื่อคงพฤติกรรมเดิม
        ESP_LOGI(TAG, "📡 Network heartbeat");
        gpio_set_level(LED_NETWORK_READY, 1);
        vTaskDelay(pdMS_TO_TICKS(NET_HEARTBEAT_MS));
    }
}

// ===================== Sensor =====================
void sensor_init_task(void *pvParameters) {
    ESP_LOGI(TAG, "🌡️ Sensor initialization started");
    uint32_t t0 = ms_now();

    // รวมดีเลย์จำลองในครั้งเดียวเพื่อให้อ่าน timing ชัด
    vTaskDelay(pdMS_TO_TICKS(SENSOR_INIT_MS));

    stats.sensor_init_time = ms_now() - t0;
    gpio_set_level(LED_SENSOR_READY, 1);
    xEventGroupSetBits(system_events, SENSOR_READY_BIT);
    ESP_LOGI(TAG, "✅ Sensors ready! (took %lu ms)", stats.sensor_init_time);

    while (1) {
        float temperature = 25.0f + (esp_random() % 200) / 10.0f;
        float humidity    = 40.0f + (esp_random() % 400) / 10.0f;
        ESP_LOGI(TAG, "🌡️ Sensor readings: %.1f°C, %.1f%% RH", temperature, humidity);
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_MS));
    }
}

// ===================== Config =====================
void config_load_task(void *pvParameters) {
    ESP_LOGI(TAG, "⚙️ Configuration loading started");
    uint32_t t0 = ms_now();

    vTaskDelay(pdMS_TO_TICKS(CONFIG_INIT_MS));

    stats.config_init_time = ms_now() - t0;
    gpio_set_level(LED_CONFIG_READY, 1);
    xEventGroupSetBits(system_events, CONFIG_READY_BIT);
    ESP_LOGI(TAG, "✅ Configuration loaded! (took %lu ms)", stats.config_init_time);

    while (1) {
        ESP_LOGI(TAG, "⚙️ Configuration OK");
        vTaskDelay(pdMS_TO_TICKS(CONFIG_MON_MS));
    }
}

// ===================== Storage =====================
void storage_init_task(void *pvParameters) {
    ESP_LOGI(TAG, "💾 Storage initialization started");
    uint32_t t0 = ms_now();

    vTaskDelay(pdMS_TO_TICKS(STORAGE_INIT_MS));

    stats.storage_init_time = ms_now() - t0;
    gpio_set_level(LED_STORAGE_READY, 1);
    xEventGroupSetBits(system_events, STORAGE_READY_BIT);
    ESP_LOGI(TAG, "✅ Storage ready! (took %lu ms)", stats.storage_init_time);

    while (1) {
        ESP_LOGI(TAG, "💾 Storage OK");
        vTaskDelay(pdMS_TO_TICKS(STORAGE_MAINT_MS));
    }
}

// ===================== Coordinator (Timing Focus) =====================
void system_coordinator_task(void *pvParameters) {
    ESP_LOGI(TAG, "🎛️ Coordinator: Timing Analysis mode");
    uint32_t t_start = ms_now();

    // -------- Phase 1: BASIC (Network + Config) --------
    ESP_LOGI(TAG, "📋 Phase 1: Wait BASIC (Network+Config), timeout=%ums", PHASE1_TIMEOUT_MS);
    EventBits_t bits = xEventGroupWaitBits(
        system_events, BASIC_SYSTEM_BITS, pdFALSE, pdTRUE, pdMS_TO_TICKS(PHASE1_TIMEOUT_MS));

    if ((bits & BASIC_SYSTEM_BITS) == BASIC_SYSTEM_BITS) {
        ESP_LOGI(TAG, "✅ Phase 1 complete in %lu ms", ms_now() - t_start);
        stats.event_notifications++;
    } else {
        stats.phase1_timeouts++;
        EventBits_t missing = BASIC_SYSTEM_BITS & ~bits;
        ESP_LOGW(TAG, "⏰ Phase 1 TIMEOUT after %lu ms; missing bits=0x%02X",
                 ms_now() - t_start, (unsigned)missing);

        // เก็บเวลา “late arrival” ของแต่ละบิตหลัง timeout
        uint32_t p1_to = ms_now();
        // รอทีละตัวแบบ non-blocking polling ที่เบามาก (ทุก 100ms) จนกว่าจะมา
        while (missing) {
            EventBits_t cur = xEventGroupGetBits(system_events);
            EventBits_t newly = missing & cur;
            if (newly) {
                if (newly & NETWORK_READY_BIT && stats.network_late_ms == 0)
                    stats.network_late_ms = ms_now() - p1_to;
                if (newly & CONFIG_READY_BIT && stats.config_late_ms == 0)
                    stats.config_late_ms  = ms_now() - p1_to;

                missing &= ~newly;
                ESP_LOGI(TAG, "🕒 Late arrival after P1 timeout: bits=0x%02X (Δ=%lums)",
                         (unsigned)newly, ms_now() - p1_to);
            }
            if (!missing) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    // -------- Phase 2: ALL SUBSYSTEMS --------
    ESP_LOGI(TAG, "📋 Phase 2: Wait ALL subsystem, timeout=%ums", PHASE2_TIMEOUT_MS);
    bits = xEventGroupWaitBits(
        system_events, ALL_SUBSYSTEM_BITS, pdFALSE, pdTRUE, pdMS_TO_TICKS(PHASE2_TIMEOUT_MS));

    if ((bits & ALL_SUBSYSTEM_BITS) == ALL_SUBSYSTEM_BITS) {
        ESP_LOGI(TAG, "✅ Phase 2 complete in %lu ms", ms_now() - t_start);
        xEventGroupSetBits(system_events, SYSTEM_READY_BIT);
        gpio_set_level(LED_SYSTEM_READY, 1);
        stats.total_init_time = ms_now() - t_start;
        stats.event_notifications++;
    } else {
        stats.phase2_timeouts++;
        EventBits_t missing = ALL_SUBSYSTEM_BITS & ~bits;
        ESP_LOGW(TAG, "⏰ Phase 2 TIMEOUT after %lu ms; missing bits=0x%02X",
                 ms_now() - t_start, (unsigned)missing);

        // เก็บ late arrival หลัง P2 timeout
        uint32_t p2_to = ms_now();
        while (missing) {
            EventBits_t cur = xEventGroupGetBits(system_events);
            EventBits_t newly = missing & cur;
            if (newly) {
                if (newly & NETWORK_READY_BIT && stats.network_late_ms == 0)
                    stats.network_late_ms = ms_now() - p2_to;
                if (newly & SENSOR_READY_BIT && stats.sensor_late_ms == 0)
                    stats.sensor_late_ms  = ms_now() - p2_to;
                if (newly & CONFIG_READY_BIT && stats.config_late_ms == 0)
                    stats.config_late_ms  = ms_now() - p2_to;
                if (newly & STORAGE_READY_BIT && stats.storage_late_ms == 0)
                    stats.storage_late_ms = ms_now() - p2_to;

                missing &= ~newly;
                ESP_LOGI(TAG, "🕒 Late arrival after P2 timeout: bits=0x%02X (Δ=%lums)",
                         (unsigned)newly, ms_now() - p2_to);
            }
            if (!missing) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // เมื่อครบในภายหลัง ให้ตั้ง SYSTEM_READY
        if ((xEventGroupGetBits(system_events) & ALL_SUBSYSTEM_BITS) == ALL_SUBSYSTEM_BITS) {
            xEventGroupSetBits(system_events, SYSTEM_READY_BIT);
            gpio_set_level(LED_SYSTEM_READY, 1);
            stats.total_init_time = ms_now() - t_start;
            ESP_LOGI(TAG, "🟢 All subsystems eventually READY (late). Total=%lums", stats.total_init_time);
        }
    }

    // -------- รายงานสรุป Timing --------
    ESP_LOGI(TAG, "\n═══ TIMING ANALYSIS REPORT ═══");
    ESP_LOGI(TAG, "Init times (ms): NET=%lu, SNS=%lu, CFG=%lu, STR=%lu",
             stats.network_init_time, stats.sensor_init_time, stats.config_init_time, stats.storage_init_time);
    ESP_LOGI(TAG, "Phase1 timeouts=%lu, Phase2 timeouts=%lu", stats.phase1_timeouts, stats.phase2_timeouts);
    ESP_LOGI(TAG, "Late after timeout (ms): NET=%lu, SNS=%lu, CFG=%lu, STR=%lu",
             stats.network_late_ms, stats.sensor_late_ms, stats.config_late_ms, stats.storage_late_ms);
    ESP_LOGI(TAG, "Total init (when finally READY): %lu ms", stats.total_init_time);
    ESP_LOGI(TAG, "══════════════════════════════\n");

    // -------- Monitoring loop (คงของเดิม) --------
    while (1) {
        EventBits_t current_bits = xEventGroupGetBits(system_events);
        ESP_LOGI(TAG, "Status: 0x%02X  NET:%s SNS:%s CFG:%s STR:%s SYS:%s",
                 (unsigned)current_bits,
                 (current_bits & NETWORK_READY_BIT) ? "✅" : "❌",
                 (current_bits & SENSOR_READY_BIT)  ? "✅" : "❌",
                 (current_bits & CONFIG_READY_BIT)  ? "✅" : "❌",
                 (current_bits & STORAGE_READY_BIT) ? "✅" : "❌",
                 (current_bits & SYSTEM_READY_BIT)  ? "✅" : "❌");
        vTaskDelay(pdMS_TO_TICKS(SYS_MONITOR_MS));
    }
}

// ===================== Event Monitor (อ้างอิงของเดิม) =====================
void event_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "👁️ Event monitor started");
    while (1) {
        ESP_LOGI(TAG, "🔍 Monitoring ANY subsystem event (5s)...");
        EventBits_t bits = xEventGroupWaitBits(
            system_events, ALL_SUBSYSTEM_BITS, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));

        if (bits) {
            if (bits & NETWORK_READY_BIT) ESP_LOGI(TAG, "  🌐 Network active");
            if (bits & SENSOR_READY_BIT)  ESP_LOGI(TAG, "  🌡️ Sensor active");
            if (bits & CONFIG_READY_BIT)  ESP_LOGI(TAG, "  ⚙️ Config active");
            if (bits & STORAGE_READY_BIT) ESP_LOGI(TAG, "  💾 Storage active");
            stats.event_notifications++;
        } else {
            ESP_LOGI(TAG, "⏰ No events within window");
        }

        // รอ FULL system ready (สั้น ๆ)
        if (!(bits & SYSTEM_READY_BIT)) {
            bits = xEventGroupWaitBits(system_events, FULL_SYSTEM_BITS, pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
            if ((bits & FULL_SYSTEM_BITS) == FULL_SYSTEM_BITS) {
                ESP_LOGI(TAG, "🎉 Full system ready detected by monitor");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(EVT_MONITOR_MS));
    }
}

// ===================== App Main =====================
void app_main(void) {
    ESP_LOGI(TAG, "🚀 Timing Analysis Experiment Starting...");

    // GPIO init
    gpio_set_direction(LED_NETWORK_READY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SENSOR_READY,  GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CONFIG_READY,  GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_STORAGE_READY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SYSTEM_READY,  GPIO_MODE_OUTPUT);
    gpio_set_level(LED_NETWORK_READY, 0);
    gpio_set_level(LED_SENSOR_READY,  0);
    gpio_set_level(LED_CONFIG_READY,  0);
    gpio_set_level(LED_STORAGE_READY, 0);
    gpio_set_level(LED_SYSTEM_READY,  0);

    // Event group
    system_events = xEventGroupCreate();
    if (!system_events) { ESP_LOGE(TAG, "Failed to create event group!"); return; }
    ESP_LOGI(TAG, "Event group created");

    // สร้าง init tasks (ดีเลย์ตาม CONFIG ข้างบน)
    xTaskCreate(network_init_task, "NetworkInit", 3072, NULL, 6, NULL);
    xTaskCreate(sensor_init_task,  "SensorInit",  2048, NULL, 5, NULL);
    xTaskCreate(config_load_task,  "ConfigLoad",  2048, NULL, 4, NULL);
    xTaskCreate(storage_init_task, "StorageInit", 2048, NULL, 4, NULL);

    // Coordinator + Monitor
    xTaskCreate(system_coordinator_task, "SysCoord", 4096, NULL, 8, NULL);
    xTaskCreate(event_monitor_task,      "EventMon", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "\n🎯 LEDs:");
    ESP_LOGI(TAG, "  GPIO2  - Network Ready");
    ESP_LOGI(TAG, "  GPIO4  - Sensor Ready");
    ESP_LOGI(TAG, "  GPIO5  - Config Ready");
    ESP_LOGI(TAG, "  GPIO18 - Storage Ready");
    ESP_LOGI(TAG, "  GPIO19 - System Ready");
    ESP_LOGI(TAG, "⏱  P1 timeout=%ums, P2 timeout=%ums | NET init=%ums", PHASE1_TIMEOUT_MS, PHASE2_TIMEOUT_MS, NETWORK_INIT_MS);
    ESP_LOGI(TAG, "🔄 Watch for Phase1/Phase2 TIMEOUT, then late-arrival messages");
}