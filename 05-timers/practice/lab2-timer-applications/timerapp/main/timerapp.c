#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_random.h"
#include "esp_system.h"

static const char *TAG = "TIMER_APPS";

// Pin Definitions
#define STATUS_LED       GPIO_NUM_2
#define WATCHDOG_LED     GPIO_NUM_4
#define PATTERN_LED_1    GPIO_NUM_5
#define PATTERN_LED_2    GPIO_NUM_18
#define PATTERN_LED_3    GPIO_NUM_19
#define SENSOR_POWER     GPIO_NUM_21
#define SENSOR_PIN       GPIO_NUM_22

// Timer Periods
#define WATCHDOG_TIMEOUT_MS     5000
#define WATCHDOG_FEED_MS        2000
#define PATTERN_BASE_MS         500
#define SENSOR_SAMPLE_MS        1000
#define STATUS_UPDATE_MS        3000

// Pattern Types
typedef enum {
    PATTERN_OFF = 0,
    PATTERN_SLOW_BLINK,
    PATTERN_FAST_BLINK,
    PATTERN_HEARTBEAT,
    PATTERN_SOS,
    PATTERN_RAINBOW,
    PATTERN_MAX
} led_pattern_t;

// ==== Forward declarations (สำคัญสำหรับแก้ error implicit declaration) ====
static void recovery_callback(TimerHandle_t timer);
static void change_led_pattern(led_pattern_t new_pattern);

// Sensor Data Structure
typedef struct {
    float value;
    uint32_t timestamp;
    bool valid;
} sensor_data_t;

// System Health Structure
typedef struct {
    uint32_t watchdog_feeds;
    uint32_t watchdog_timeouts;
    uint32_t pattern_changes;
    uint32_t sensor_readings;
    uint32_t system_uptime_sec;
    bool system_healthy;
} system_health_t;

// Global Variables
static TimerHandle_t watchdog_timer;
static TimerHandle_t feed_timer;
static TimerHandle_t pattern_timer;
static TimerHandle_t sensor_timer;
static TimerHandle_t status_timer;

static QueueHandle_t sensor_queue;
static QueueHandle_t pattern_queue;

static led_pattern_t current_pattern = PATTERN_OFF;
static int pattern_step = 0;
static system_health_t health_stats = {0, 0, 0, 0, 0, true};

// Pattern state
typedef struct {
    int step;
    int direction;
    int intensity;
    bool state;
} pattern_state_t;

static pattern_state_t pattern_state = {0, 1, 0, false};

// ADC calibration
static esp_adc_cal_characteristics_t *adc_chars;

// ================ WATCHDOG SYSTEM ================
static void watchdog_timeout_callback(TimerHandle_t timer) {
    health_stats.watchdog_timeouts++;
    health_stats.system_healthy = false;

    ESP_LOGE(TAG, "🚨 WATCHDOG TIMEOUT! System may be hung!");
    ESP_LOGE(TAG, "System stats: Feeds=%lu, Timeouts=%lu",
             health_stats.watchdog_feeds, health_stats.watchdog_timeouts);

    for (int i = 0; i < 10; i++) {
        gpio_set_level(WATCHDOG_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(WATCHDOG_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGW(TAG, "In production: esp_restart() would be called here");

    xTimerReset(watchdog_timer, 0);
    health_stats.system_healthy = true;
}

static void feed_watchdog_callback(TimerHandle_t timer) {
    static int feed_count = 0;
    feed_count++;

    if (feed_count == 15) {
        ESP_LOGW(TAG, "🐛 Simulating system hang - stopping watchdog feeds for 8 seconds");
        xTimerStop(feed_timer, 0);

        TimerHandle_t recovery_timer = xTimerCreate("Recovery",
                                                   pdMS_TO_TICKS(8000),
                                                   pdFALSE,
                                                   (void*)0,
                                                   recovery_callback);
        xTimerStart(recovery_timer, 0);
        return;
    }

    health_stats.watchdog_feeds++;
    ESP_LOGI(TAG, "🍖 Feeding watchdog (feed #%lu)", health_stats.watchdog_feeds);

    xTimerReset(watchdog_timer, 0);

    gpio_set_level(STATUS_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(STATUS_LED, 0);
}

static void recovery_callback(TimerHandle_t timer) {
    ESP_LOGI(TAG, "🔄 System recovered - resuming watchdog feeds");
    xTimerStart(feed_timer, 0);
    xTimerDelete(timer, 0);
}

// ================ LED PATTERN SYSTEM ================
static void set_pattern_leds(bool led1, bool led2, bool led3) {
    gpio_set_level(PATTERN_LED_1, led1);
    gpio_set_level(PATTERN_LED_2, led2);
    gpio_set_level(PATTERN_LED_3, led3);
}

static void pattern_timer_callback(TimerHandle_t timer) {
    static uint32_t pattern_cycle = 0;
    pattern_cycle++;

    switch (current_pattern) {
        case PATTERN_OFF:
            set_pattern_leds(0, 0, 0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(1000), 0);
            break;

        case PATTERN_SLOW_BLINK:
            pattern_state.state = !pattern_state.state;
            set_pattern_leds(pattern_state.state, 0, 0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(1000), 0);
            ESP_LOGI(TAG, "💡 Slow Blink: %s", pattern_state.state ? "ON" : "OFF");
            break;

        case PATTERN_FAST_BLINK:
            pattern_state.state = !pattern_state.state;
            set_pattern_leds(0, pattern_state.state, 0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(200), 0);
            break;

        case PATTERN_HEARTBEAT: {
            int step = pattern_step % 10;
            bool pulse = (step < 2) || (step >= 3 && step < 5);
            set_pattern_leds(0, 0, pulse);
            pattern_step++;
            xTimerChangePeriod(timer, pdMS_TO_TICKS(100), 0);
            if (step == 9) ESP_LOGI(TAG, "💓 Heartbeat pulse");
            break;
        }

        case PATTERN_SOS: {
            static const char* sos = "...---...";
            static int sos_pos = 0;

            bool on = (sos[sos_pos] == '.');
            int duration = on ? 200 : 600;

            set_pattern_leds(on, on, on);

            sos_pos = (sos_pos + 1) % strlen(sos);
            if (sos_pos == 0) {
                ESP_LOGI(TAG, "🆘 SOS Pattern Complete");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            xTimerChangePeriod(timer, pdMS_TO_TICKS(duration), 0);
            break;
        }

        case PATTERN_RAINBOW: {
            int rainbow_step = pattern_step % 8;
            bool led1 = (rainbow_step & 1) != 0;
            bool led2 = (rainbow_step & 2) != 0;
            bool led3 = (rainbow_step & 4) != 0;

            set_pattern_leds(led1, led2, led3);
            pattern_step++;

            if (rainbow_step == 7) ESP_LOGI(TAG, "🌈 Rainbow cycle complete");
            xTimerChangePeriod(timer, pdMS_TO_TICKS(300), 0);
            break;
        }

        default:
            set_pattern_leds(0, 0, 0);
            break;
    }

    if (pattern_cycle % 50 == 0) {
        led_pattern_t new_pattern = (current_pattern + 1) % PATTERN_MAX;
        change_led_pattern(new_pattern);
    }
}

static void change_led_pattern(led_pattern_t new_pattern) {
    const char* pattern_names[] = {
        "OFF", "SLOW_BLINK", "FAST_BLINK",
        "HEARTBEAT", "SOS", "RAINBOW"
    };

    ESP_LOGI(TAG, "🎨 Changing pattern: %s -> %s",
             pattern_names[current_pattern], pattern_names[new_pattern]);

    current_pattern = new_pattern;
    pattern_step = 0;
    pattern_state.step = 0;
    pattern_state.state = false;
    health_stats.pattern_changes++;

    xTimerReset(pattern_timer, 0);
}

// ================ SENSOR SYSTEM ================
static float read_sensor_value(void) {
    gpio_set_level(SENSOR_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint32_t adc_reading = adc1_get_raw(ADC1_CHANNEL_0);
    uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);

    float sensor_value = (voltage / 1000.0f) * 50.0f;
    sensor_value += (int)(esp_random() % 100 - 50) / 100.0f;

    gpio_set_level(SENSOR_POWER, 0);
    return sensor_value;
}

static void sensor_timer_callback(TimerHandle_t timer) {
    sensor_data_t sensor_data;

    sensor_data.value = read_sensor_value();
    sensor_data.timestamp = xTaskGetTickCount();
    sensor_data.valid = (sensor_data.value >= 0 && sensor_data.value <= 50);

    health_stats.sensor_readings++;

    // ใช้เวอร์ชัน non-ISR (เพราะ callback ของ software timer ไม่ใช่ ISR)
    if (xQueueSend(sensor_queue, &sensor_data, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Sensor queue full - dropping sample");
    }

    TickType_t new_period;
    if (sensor_data.value > 40.0f) {
        new_period = pdMS_TO_TICKS(500);
    } else if (sensor_data.value > 25.0f) {
        new_period = pdMS_TO_TICKS(1000);
    } else {
        new_period = pdMS_TO_TICKS(2000);
    }
    xTimerChangePeriod(sensor_timer, new_period, 0);
}

// ================ STATUS SYSTEM ================
static void status_timer_callback(TimerHandle_t timer) {
    health_stats.system_uptime_sec = pdTICKS_TO_MS(xTaskGetTickCount()) / 1000;

    ESP_LOGI(TAG, "\n═══════ SYSTEM STATUS ═══════");
    ESP_LOGI(TAG, "Uptime: %lu seconds", health_stats.system_uptime_sec);
    ESP_LOGI(TAG, "System Health: %s", health_stats.system_healthy ? "✅ HEALTHY" : "❌ ISSUES");
    ESP_LOGI(TAG, "Watchdog Feeds: %lu", health_stats.watchdog_feeds);
    ESP_LOGI(TAG, "Watchdog Timeouts: %lu", health_stats.watchdog_timeouts);
    ESP_LOGI(TAG, "Pattern Changes: %lu", health_stats.pattern_changes);
    ESP_LOGI(TAG, "Sensor Readings: %lu", health_stats.sensor_readings);
    ESP_LOGI(TAG, "Current Pattern: %d", current_pattern);

    ESP_LOGI(TAG, "Timer States:");
    ESP_LOGI(TAG, "  Watchdog: %s", xTimerIsTimerActive(watchdog_timer) ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "  Feed: %s", xTimerIsTimerActive(feed_timer) ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "  Pattern: %s", xTimerIsTimerActive(pattern_timer) ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "  Sensor: %s", xTimerIsTimerActive(sensor_timer) ? "ACTIVE" : "INACTIVE");
    ESP_LOGI(TAG, "════════════════════════════\n");

    gpio_set_level(STATUS_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(STATUS_LED, 0);
}

// ================ PROCESSING TASKS ================
static void sensor_processing_task(void *parameter) {
    sensor_data_t sensor_data;
    float temp_sum = 0;
    int sample_count = 0;

    ESP_LOGI(TAG, "Sensor processing task started");

    while (1) {
        if (xQueueReceive(sensor_queue, &sensor_data, portMAX_DELAY) == pdTRUE) {
            if (sensor_data.valid) {
                temp_sum += sensor_data.value;
                sample_count++;

                ESP_LOGI(TAG, "🌡️ Sensor: %.2f°C at %lu ms",
                         sensor_data.value, sensor_data.timestamp);

                if (sample_count >= 10) {
                    float average = temp_sum / sample_count;
                    ESP_LOGI(TAG, "📊 Temperature Average: %.2f°C", average);

                    if (average > 35.0f) {
                        ESP_LOGW(TAG, "🔥 High temperature warning!");
                        change_led_pattern(PATTERN_FAST_BLINK);
                    } else if (average < 15.0f) {
                        ESP_LOGW(TAG, "🧊 Low temperature warning!");
                        change_led_pattern(PATTERN_SOS);
                    }

                    temp_sum = 0;
                    sample_count = 0;
                }
            } else {
                ESP_LOGW(TAG, "Invalid sensor reading: %.2f", sensor_data.value);
            }
        }
    }
}

static void system_monitor_task(void *parameter) {
    ESP_LOGI(TAG, "System monitor task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        if (health_stats.watchdog_timeouts > 5) {
            ESP_LOGE(TAG, "🚨 Too many watchdog timeouts - system unstable!");
            health_stats.system_healthy = false;
        }

        static uint32_t last_sensor_count = 0;
        if (health_stats.sensor_readings == last_sensor_count) {
            ESP_LOGW(TAG, "⚠️ Sensor readings stopped - checking sensor system");
        }
        last_sensor_count = health_stats.sensor_readings;

        size_t free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG, "💾 Free heap: %d bytes", free_heap);

        if (free_heap < 10000) {
            ESP_LOGW(TAG, "⚠️ Low memory warning!");
        }
    }
}

// ================ INITIALIZATION ================
static void init_hardware(void) {
    gpio_set_direction(STATUS_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(WATCHDOG_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_2, GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_3, GPIO_MODE_OUTPUT);
    gpio_set_direction(SENSOR_POWER, GPIO_MODE_OUTPUT);

    gpio_set_level(STATUS_LED, 0);
    gpio_set_level(WATCHDOG_LED, 0);
    gpio_set_level(PATTERN_LED_1, 0);
    gpio_set_level(PATTERN_LED_2, 0);
    gpio_set_level(PATTERN_LED_3, 0);
    gpio_set_level(SENSOR_POWER, 0);

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);

    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, adc_chars);

    ESP_LOGI(TAG, "Hardware initialization complete");
}

static void create_timers(void) {
    watchdog_timer = xTimerCreate("WatchdogTimer",
                                 pdMS_TO_TICKS(WATCHDOG_TIMEOUT_MS),
                                 pdFALSE, (void*)1,
                                 watchdog_timeout_callback);

    feed_timer = xTimerCreate("FeedTimer",
                             pdMS_TO_TICKS(WATCHDOG_FEED_MS),
                             pdTRUE, (void*)2,
                             feed_watchdog_callback);

    pattern_timer = xTimerCreate("PatternTimer",
                                pdMS_TO_TICKS(PATTERN_BASE_MS),
                                pdTRUE, (void*)3,
                                pattern_timer_callback);

    sensor_timer = xTimerCreate("SensorTimer",
                               pdMS_TO_TICKS(SENSOR_SAMPLE_MS),
                               pdTRUE, (void*)4,
                               sensor_timer_callback);

    status_timer = xTimerCreate("StatusTimer",
                               pdMS_TO_TICKS(STATUS_UPDATE_MS),
                               pdTRUE, (void*)5,
                               status_timer_callback);

    if (!watchdog_timer || !feed_timer || !pattern_timer || !sensor_timer || !status_timer) {
        ESP_LOGE(TAG, "Failed to create one or more timers");
        return;
    }

    ESP_LOGI(TAG, "All timers created successfully");
}

static void create_queues(void) {
    sensor_queue = xQueueCreate(20, sizeof(sensor_data_t));
    pattern_queue = xQueueCreate(10, sizeof(led_pattern_t));

    if (!sensor_queue || !pattern_queue) {
        ESP_LOGE(TAG, "Failed to create queues");
        return;
    }

    ESP_LOGI(TAG, "Queues created successfully");
}

static void start_system(void) {
    ESP_LOGI(TAG, "Starting timer system...");

    xTimerStart(watchdog_timer, 0);
    xTimerStart(feed_timer, 0);
    xTimerStart(pattern_timer, 0);
    xTimerStart(sensor_timer, 0);
    xTimerStart(status_timer, 0);

    xTaskCreate(sensor_processing_task, "SensorProc", 2048, NULL, 6, NULL);
    xTaskCreate(system_monitor_task, "SysMonitor", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "🚀 Timer Applications System Started!");
    ESP_LOGI(TAG, "Watch the LEDs for different patterns and system status");
}

void app_main(void) {
    ESP_LOGI(TAG, "Timer Applications Lab Starting...");

    init_hardware();
    create_queues();
    create_timers();
    start_system();

    change_led_pattern(PATTERN_SLOW_BLINK);

    ESP_LOGI(TAG, "System operational - monitoring started");
}