#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "COMPLEX_EVENTS";

// GPIO สำหรับ Smart Home System
#define LED_LIVING_ROOM    GPIO_NUM_2   // Living room light
#define LED_KITCHEN        GPIO_NUM_4   // Kitchen light
#define LED_BEDROOM        GPIO_NUM_5   // Bedroom light
#define LED_SECURITY       GPIO_NUM_18  // Security system
#define LED_EMERGENCY      GPIO_NUM_19  // Emergency indicator
#define MOTION_SENSOR      GPIO_NUM_21  // Motion sensor input
#define DOOR_SENSOR        GPIO_NUM_22  // Door sensor input

// Smart Home State Machine States
typedef enum {
    HOME_STATE_IDLE = 0,
    HOME_STATE_OCCUPIED,
    HOME_STATE_AWAY,
    HOME_STATE_SLEEP,
    HOME_STATE_SECURITY_ARMED,
    HOME_STATE_EMERGENCY,
    HOME_STATE_MAINTENANCE
} home_state_t;

// Event Groups และ Event Bits
EventGroupHandle_t sensor_events;
EventGroupHandle_t system_events;
EventGroupHandle_t pattern_events;

// Sensor Events
#define MOTION_DETECTED_BIT     (1 << 0)
#define DOOR_OPENED_BIT         (1 << 1)
#define DOOR_CLOSED_BIT         (1 << 2)
#define LIGHT_ON_BIT            (1 << 3)
#define LIGHT_OFF_BIT           (1 << 4)
#define TEMPERATURE_HIGH_BIT    (1 << 5)
#define TEMPERATURE_LOW_BIT     (1 << 6)
#define SOUND_DETECTED_BIT      (1 << 7)
#define PRESENCE_CONFIRMED_BIT  (1 << 8)

// System Events
#define SYSTEM_INIT_BIT         (1 << 0)
#define USER_HOME_BIT           (1 << 1)
#define USER_AWAY_BIT           (1 << 2)
#define SLEEP_MODE_BIT          (1 << 3)
#define SECURITY_ARMED_BIT      (1 << 4)
#define EMERGENCY_MODE_BIT      (1 << 5)
#define MAINTENANCE_MODE_BIT    (1 << 6)

// Pattern Events
#define PATTERN_NORMAL_ENTRY_BIT    (1 << 0)
#define PATTERN_BREAK_IN_BIT        (1 << 1)
#define PATTERN_EMERGENCY_BIT       (1 << 2)
#define PATTERN_GOODNIGHT_BIT       (1 << 3)
#define PATTERN_WAKE_UP_BIT         (1 << 4)
#define PATTERN_LEAVING_BIT         (1 << 5)
#define PATTERN_RETURNING_BIT       (1 << 6)

// Event และ State Management
static home_state_t current_home_state = HOME_STATE_IDLE;
static SemaphoreHandle_t state_mutex;

// Event History สำหรับ Pattern Recognition
#define EVENT_HISTORY_SIZE 20
typedef struct {
    EventBits_t event_bits;
    uint64_t timestamp;
    home_state_t state_at_time;
} event_record_t;

static event_record_t event_history[EVENT_HISTORY_SIZE];
static int history_index = 0;

// Pattern Recognition Data
typedef struct {
    const char* name;
    EventBits_t required_events[4];  // Up to 4 events in sequence
    uint32_t time_window_ms;         // Max time between events
    EventBits_t result_event;        // Event to set when pattern matches
    void (*action_callback)(void);   // Optional callback function
} event_pattern_t;

// Adaptive System Parameters
typedef struct {
    float motion_sensitivity;
    uint32_t auto_light_timeout;
    uint32_t security_delay;
    bool learning_mode;
    uint32_t pattern_confidence[10];
} adaptive_params_t;

static adaptive_params_t adaptive_params = {
    .motion_sensitivity = 0.7f,
    .auto_light_timeout = 300000,  // 5 minutes
    .security_delay = 30000,       // 30 seconds
    .learning_mode = true,
    .pattern_confidence = {0}
};

// Smart Devices Control
typedef struct {
    bool living_room_light;
    bool kitchen_light;
    bool bedroom_light;
    bool security_system;
    bool emergency_mode;
    uint32_t temperature_celsius;
    uint32_t light_level_percent;
} smart_home_status_t;

static smart_home_status_t home_status = {0};

// ========= Pattern Action Callbacks =========
void normal_entry_action(void) {
    ESP_LOGI(TAG, "🏠 Normal entry pattern detected - Welcome home!");
    home_status.living_room_light = true;
    gpio_set_level(LED_LIVING_ROOM, 1);
    xEventGroupSetBits(system_events, USER_HOME_BIT);
}

void break_in_action(void) {
    ESP_LOGW(TAG, "🚨 Break-in pattern detected - Security alert!");
    home_status.security_system = true;
    home_status.emergency_mode = true;
    gpio_set_level(LED_SECURITY, 1);
    gpio_set_level(LED_EMERGENCY, 1);
    xEventGroupSetBits(system_events, EMERGENCY_MODE_BIT);
}

void goodnight_action(void) {
    ESP_LOGI(TAG, "🌙 Goodnight pattern detected - Sleep mode activated");
    home_status.living_room_light = false;
    home_status.kitchen_light = false;
    gpio_set_level(LED_LIVING_ROOM, 0);
    gpio_set_level(LED_KITCHEN, 0);
    gpio_set_level(LED_BEDROOM, 1);  // Keep bedroom light dim
    xEventGroupSetBits(system_events, SLEEP_MODE_BIT);
}

void wake_up_action(void) {
    ESP_LOGI(TAG, "☀️ Wake-up pattern detected - Good morning!");
    home_status.bedroom_light = true;
    home_status.kitchen_light = true;
    gpio_set_level(LED_BEDROOM, 1);
    gpio_set_level(LED_KITCHEN, 1);
    xEventGroupClearBits(system_events, SLEEP_MODE_BIT);
}

void leaving_action(void) {
    ESP_LOGI(TAG, "🚪 Leaving pattern detected - Securing home");
    home_status.living_room_light = false;
    home_status.kitchen_light = false;
    home_status.bedroom_light = false;
    home_status.security_system = true;

    gpio_set_level(LED_LIVING_ROOM, 0);
    gpio_set_level(LED_KITCHEN, 0);
    gpio_set_level(LED_BEDROOM, 0);
    gpio_set_level(LED_SECURITY, 1);

    xEventGroupSetBits(system_events, USER_AWAY_BIT | SECURITY_ARMED_BIT);
}

void returning_action(void) {
    ESP_LOGI(TAG, "🔓 Returning pattern detected - Disabling security");
    home_status.security_system = false;
    gpio_set_level(LED_SECURITY, 0);
    xEventGroupClearBits(system_events, USER_AWAY_BIT | SECURITY_ARMED_BIT);
}

// ========= Event Patterns Definition =========
static event_pattern_t event_patterns[] = {
    {
        .name = "Normal Entry",
        .required_events = {DOOR_OPENED_BIT, MOTION_DETECTED_BIT, DOOR_CLOSED_BIT, 0},
        .time_window_ms = 10000,
        .result_event = PATTERN_NORMAL_ENTRY_BIT,
        .action_callback = normal_entry_action
    },
    {
        .name = "Break-in Attempt",
        .required_events = {DOOR_OPENED_BIT, MOTION_DETECTED_BIT, 0, 0},
        .time_window_ms = 5000,
        .result_event = PATTERN_BREAK_IN_BIT,
        .action_callback = break_in_action
    },
    {
        .name = "Goodnight Routine",
        .required_events = {LIGHT_OFF_BIT, MOTION_DETECTED_BIT, LIGHT_OFF_BIT, 0},
        .time_window_ms = 30000,
        .result_event = PATTERN_GOODNIGHT_BIT,
        .action_callback = goodnight_action
    },
    {
        .name = "Wake-up Routine",
        .required_events = {MOTION_DETECTED_BIT, LIGHT_ON_BIT, 0, 0},
        .time_window_ms = 5000,
        .result_event = PATTERN_WAKE_UP_BIT,
        .action_callback = wake_up_action
    },
    {
        .name = "Leaving Home",
        .required_events = {LIGHT_OFF_BIT, DOOR_OPENED_BIT, DOOR_CLOSED_BIT, 0},
        .time_window_ms = 15000,
        .result_event = PATTERN_LEAVING_BIT,
        .action_callback = leaving_action
    },
    {
        .name = "Returning Home",
        .required_events = {DOOR_OPENED_BIT, MOTION_DETECTED_BIT, DOOR_CLOSED_BIT, 0},
        .time_window_ms = 8000,
        .result_event = PATTERN_RETURNING_BIT,
        .action_callback = returning_action
    }
};

#define NUM_PATTERNS (sizeof(event_patterns) / sizeof(event_pattern_t))

// ========= State Machine =========
const char* get_state_name(home_state_t state) {
    switch (state) {
        case HOME_STATE_IDLE: return "Idle";
        case HOME_STATE_OCCUPIED: return "Occupied";
        case HOME_STATE_AWAY: return "Away";
        case HOME_STATE_SLEEP: return "Sleep";
        case HOME_STATE_SECURITY_ARMED: return "Security Armed";
        case HOME_STATE_EMERGENCY: return "Emergency";
        case HOME_STATE_MAINTENANCE: return "Maintenance";
        default: return "Unknown";
    }
}

void change_home_state(home_state_t new_state) {
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        home_state_t old_state = current_home_state;
        current_home_state = new_state;
        ESP_LOGI(TAG, "🏠 State changed: %s → %s", get_state_name(old_state), get_state_name(new_state));
        xSemaphoreGive(state_mutex);
    }
}

// ========= Event History =========
void add_event_to_history(EventBits_t event_bits) {
    event_history[history_index].event_bits = event_bits;
    event_history[history_index].timestamp = esp_timer_get_time();
    event_history[history_index].state_at_time = current_home_state;
    history_index = (history_index + 1) % EVENT_HISTORY_SIZE;
}

// ========= Pattern Recognition Engine =========
void pattern_recognition_task(void *pvParameters) {
    ESP_LOGI(TAG, "🧠 Pattern recognition engine started");
    while (1) {
        EventBits_t sensor_bits = xEventGroupWaitBits(
            sensor_events,
            0xFFFFFF,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY
        );
        if (sensor_bits != 0) {
            ESP_LOGI(TAG, "🔍 Sensor event detected: 0x%08X", sensor_bits);
            add_event_to_history(sensor_bits);

            for (int p = 0; p < NUM_PATTERNS; p++) {
                event_pattern_t* pattern = &event_patterns[p];

                bool state_applicable = true;
                if (strcmp(pattern->name, "Break-in Attempt") == 0) {
                    state_applicable = (current_home_state == HOME_STATE_SECURITY_ARMED);
                } else if (strcmp(pattern->name, "Wake-up Routine") == 0) {
                    state_applicable = (current_home_state == HOME_STATE_SLEEP);
                } else if (strcmp(pattern->name, "Returning Home") == 0) {
                    state_applicable = (current_home_state == HOME_STATE_AWAY);
                }
                if (!state_applicable) continue;

                bool found_all = false;
                uint64_t now = esp_timer_get_time();
                int need_idx = 0;

                for (int h = 0; h < EVENT_HISTORY_SIZE && pattern->required_events[need_idx] != 0; h++) {
                    int hist_idx = (history_index - 1 - h + EVENT_HISTORY_SIZE) % EVENT_HISTORY_SIZE;
                    event_record_t* rec = &event_history[hist_idx];
                    if ((now - rec->timestamp) > (pattern->time_window_ms * 1000ULL)) break;
                    if (rec->event_bits & pattern->required_events[need_idx]) {
                        need_idx++;
                        ESP_LOGI(TAG, "✅ Pattern '%s': matched step %d (0x%08X)",
                                 pattern->name, need_idx, pattern->required_events[need_idx-1]);
                        if (pattern->required_events[need_idx] == 0) { found_all = true; break; }
                    }
                }

                if (found_all) {
                    ESP_LOGI(TAG, "🎯 Pattern matched: %s", pattern->name);
                    xEventGroupSetBits(pattern_events, pattern->result_event);
                    if (pattern->action_callback) pattern->action_callback();
                    if (p < 10) adaptive_params.pattern_confidence[p]++;
                    xEventGroupClearBits(sensor_events, 0xFFFFFF);
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ================== SENSOR SIMULATIONS ==================
// เปิด/ปิด “โหมดเดโมควบคุมเหตุการณ์เอง”
static volatile bool scenario_mode = true;

static inline void wait_if_demo(void) {
    if (scenario_mode) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void motion_sensor_task(void *pvParameters) {
    ESP_LOGI(TAG, "🏃 Motion sensor simulation started");
    while (1) {
        if (scenario_mode) { wait_if_demo(); continue; }
        if ((esp_random() % 100) < 15) {
            ESP_LOGI(TAG, "👥 Motion detected!");
            xEventGroupSetBits(sensor_events, MOTION_DETECTED_BIT);
            vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 2000)));
            if ((esp_random() % 100) < 60) {
                ESP_LOGI(TAG, "✅ Presence confirmed");
                xEventGroupSetBits(sensor_events, PRESENCE_CONFIRMED_BIT);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 5000)));
    }
}

void door_sensor_task(void *pvParameters) {
    ESP_LOGI(TAG, "🚪 Door sensor simulation started");
    bool door_open = false;
    while (1) {
        if (scenario_mode) { wait_if_demo(); continue; }
        if ((esp_random() % 100) < 8) {
            if (!door_open) {
                ESP_LOGI(TAG, "🔓 Door opened");
                xEventGroupSetBits(sensor_events, DOOR_OPENED_BIT);
                door_open = true;
                vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 8000)));
                if ((esp_random() % 100) < 85) {
                    ESP_LOGI(TAG, "🔒 Door closed");
                    xEventGroupSetBits(sensor_events, DOOR_CLOSED_BIT);
                    door_open = false;
                }
            } else {
                ESP_LOGI(TAG, "🔒 Door closed");
                xEventGroupSetBits(sensor_events, DOOR_CLOSED_BIT);
                door_open = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000 + (esp_random() % 10000)));
    }
}

void light_control_task(void *pvParameters) {
    ESP_LOGI(TAG, "💡 Light control system started");
    while (1) {
        if (scenario_mode) { wait_if_demo(); continue; }
        if ((esp_random() % 100) < 12) {
            bool on = (esp_random() % 2);
            if (on) {
                ESP_LOGI(TAG, "💡 Light turned ON");
                xEventGroupSetBits(sensor_events, LIGHT_ON_BIT);
                int c = esp_random() % 3;
                if (c == 0) { home_status.living_room_light = true; gpio_set_level(LED_LIVING_ROOM, 1); }
                else if (c == 1) { home_status.kitchen_light = true; gpio_set_level(LED_KITCHEN, 1); }
                else { home_status.bedroom_light = true; gpio_set_level(LED_BEDROOM, 1); }
            } else {
                ESP_LOGI(TAG, "💡 Light turned OFF");
                xEventGroupSetBits(sensor_events, LIGHT_OFF_BIT);
                int c = esp_random() % 3;
                if (c == 0) { home_status.living_room_light = false; gpio_set_level(LED_LIVING_ROOM, 0); }
                else if (c == 1) { home_status.kitchen_light = false; gpio_set_level(LED_KITCHEN, 0); }
                else { home_status.bedroom_light = false; gpio_set_level(LED_BEDROOM, 0); }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(4000 + (esp_random() % 8000)));
    }
}

void environmental_sensor_task(void *pvParameters) {
    ESP_LOGI(TAG, "🌡️ Environmental sensors started");
    while (1) {
        if (scenario_mode) { wait_if_demo(); continue; }
        home_status.temperature_celsius = 20 + (esp_random() % 15);
        if (home_status.temperature_celsius > 28) {
            ESP_LOGI(TAG, "🔥 High temperature detected: %d°C", home_status.temperature_celsius);
            xEventGroupSetBits(sensor_events, TEMPERATURE_HIGH_BIT);
        } else if (home_status.temperature_celsius < 22) {
            ESP_LOGI(TAG, "🧊 Low temperature detected: %d°C", home_status.temperature_celsius);
            xEventGroupSetBits(sensor_events, TEMPERATURE_LOW_BIT);
        }
        if ((esp_random() % 100) < 5) {
            ESP_LOGI(TAG, "🔊 Sound detected");
            xEventGroupSetBits(sensor_events, SOUND_DETECTED_BIT);
        }
        home_status.light_level_percent = esp_random() % 100;
        vTaskDelay(pdMS_TO_TICKS(8000 + (esp_random() % 7000)));
    }
}

// ========= State Machine Task =========
void state_machine_task(void *pvParameters) {
    ESP_LOGI(TAG, "🏠 Home state machine started");
    while (1) {
        EventBits_t system_bits = xEventGroupWaitBits(
            system_events, 0xFFFFFF, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
        if (system_bits != 0) {
            ESP_LOGI(TAG, "🔄 System event: 0x%08X", system_bits);
            if (system_bits & USER_HOME_BIT) {
                if (current_home_state == HOME_STATE_AWAY || current_home_state == HOME_STATE_IDLE)
                    change_home_state(HOME_STATE_OCCUPIED);
            }
            if (system_bits & USER_AWAY_BIT) {
                change_home_state(HOME_STATE_AWAY);
            }
            if (system_bits & SLEEP_MODE_BIT) {
                if (current_home_state == HOME_STATE_OCCUPIED)
                    change_home_state(HOME_STATE_SLEEP);
            }
            if (system_bits & SECURITY_ARMED_BIT) {
                if (current_home_state == HOME_STATE_AWAY)
                    change_home_state(HOME_STATE_SECURITY_ARMED);
            }
            if (system_bits & EMERGENCY_MODE_BIT) {
                change_home_state(HOME_STATE_EMERGENCY);
            }
            if (system_bits & MAINTENANCE_MODE_BIT) {
                change_home_state(HOME_STATE_MAINTENANCE);
            }
        }

        switch (current_home_state) {
            case HOME_STATE_EMERGENCY:
                vTaskDelay(pdMS_TO_TICKS(10000));
                ESP_LOGI(TAG, "🆘 Emergency cleared - returning to normal");
                home_status.emergency_mode = false;
                gpio_set_level(LED_EMERGENCY, 0);
                change_home_state(HOME_STATE_OCCUPIED);
                break;
            case HOME_STATE_IDLE: {
                EventBits_t sensor_activity = xEventGroupGetBits(sensor_events);
                if (sensor_activity & (MOTION_DETECTED_BIT | PRESENCE_CONFIRMED_BIT))
                    change_home_state(HOME_STATE_OCCUPIED);
            } break;
            default: break;
        }
    }
}

// ========= Adaptive Learning =========
void adaptive_learning_task(void *pvParameters) {
    ESP_LOGI(TAG, "🧠 Adaptive learning system started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (adaptive_params.learning_mode) {
            ESP_LOGI(TAG, "📊 Learning from patterns...");
            for (int i = 0; i < NUM_PATTERNS; i++) {
                if (adaptive_params.pattern_confidence[i] > 5) {
                    ESP_LOGI(TAG, "📈 Pattern %d confidence high (%lu) - optimizing",
                             i, adaptive_params.pattern_confidence[i]);
                }
            }
            uint32_t recent_motion = 0;
            uint64_t now = esp_timer_get_time();
            for (int h = 0; h < EVENT_HISTORY_SIZE; h++) {
                int idx = (history_index - 1 - h + EVENT_HISTORY_SIZE) % EVENT_HISTORY_SIZE;
                event_record_t* rec = &event_history[idx];
                if ((now - rec->timestamp) < 300000000ULL) {
                    if (rec->event_bits & MOTION_DETECTED_BIT) recent_motion++;
                } else break;
            }
            if (recent_motion > 10) {
                adaptive_params.motion_sensitivity *= 0.95f;
                ESP_LOGI(TAG, "🔧 High motion activity - reducing sensitivity to %.2f",
                         adaptive_params.motion_sensitivity);
            } else if (recent_motion < 2) {
                adaptive_params.motion_sensitivity *= 1.05f;
                ESP_LOGI(TAG, "🔧 Low motion activity - increasing sensitivity to %.2f",
                         adaptive_params.motion_sensitivity);
            }
            if (adaptive_params.motion_sensitivity > 1.0f) adaptive_params.motion_sensitivity = 1.0f;
            if (adaptive_params.motion_sensitivity < 0.3f) adaptive_params.motion_sensitivity = 0.3f;
        }
    }
}

// ========= Status Monitor =========
void status_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "📊 Status monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        ESP_LOGI(TAG, "\n🏠 ═══ SMART HOME STATUS ═══");
        ESP_LOGI(TAG, "Current State:     %s", get_state_name(current_home_state));
        ESP_LOGI(TAG, "Living Room:       %s", home_status.living_room_light ? "ON" : "OFF");
        ESP_LOGI(TAG, "Kitchen:           %s", home_status.kitchen_light ? "ON" : "OFF");
        ESP_LOGI(TAG, "Bedroom:           %s", home_status.bedroom_light ? "ON" : "OFF");
        ESP_LOGI(TAG, "Security:          %s", home_status.security_system ? "ARMED" : "DISARMED");
        ESP_LOGI(TAG, "Emergency:         %s", home_status.emergency_mode ? "ACTIVE" : "NORMAL");
        ESP_LOGI(TAG, "Temperature:       %d°C", home_status.temperature_celsius);
        ESP_LOGI(TAG, "Light Level:       %d%%", home_status.light_level_percent);
        ESP_LOGI(TAG, "\n📊 Event Group Status:");
        ESP_LOGI(TAG, "Sensor Events:     0x%08X", xEventGroupGetBits(sensor_events));
        ESP_LOGI(TAG, "System Events:     0x%08X", xEventGroupGetBits(system_events));
        ESP_LOGI(TAG, "Pattern Events:    0x%08X", xEventGroupGetBits(pattern_events));
        ESP_LOGI(TAG, "\n🧠 Adaptive Parameters:");
        ESP_LOGI(TAG, "Motion Sensitivity: %.2f", adaptive_params.motion_sensitivity);
        ESP_LOGI(TAG, "Light Timeout:      %lu ms", adaptive_params.auto_light_timeout);
        ESP_LOGI(TAG, "Security Delay:     %lu ms", adaptive_params.security_delay);
        ESP_LOGI(TAG, "Learning Mode:      %s", adaptive_params.learning_mode ? "ON" : "OFF");
        ESP_LOGI(TAG, "\n📈 Pattern Confidence:");
        for (int i = 0; i < NUM_PATTERNS; i++) {
            if (adaptive_params.pattern_confidence[i] > 0) {
                ESP_LOGI(TAG, "  %s: %lu", event_patterns[i].name,
                         adaptive_params.pattern_confidence[i]);
            }
        }
        ESP_LOGI(TAG, "Free Heap:         %d bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "════════════════════════════════════════\n");
    }
}

// ========= SCENARIO INJECTOR (Real-world demo) =========
static inline void push_sensor(EventBits_t bits, uint32_t delay_ms) {
    xEventGroupSetBits(sensor_events, bits);
    if (delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
}
static inline void push_system(EventBits_t bits, uint32_t delay_ms) {
    xEventGroupSetBits(system_events, bits);
    if (delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

void scenario_injector_task(void *pv) {
    ESP_LOGI(TAG, "🧪 Scenario injector started (leaving, break-in, goodnight)");
    vTaskDelay(pdMS_TO_TICKS(5000)); // รอระบบพร้อม

    // Scenario #1: Leaving Home
    ESP_LOGI(TAG, "➡️  Scenario: Leaving Home");
    push_sensor(LIGHT_OFF_BIT, 300);
    push_sensor(LIGHT_OFF_BIT, 300);
    push_sensor(DOOR_OPENED_BIT, 1500);
    push_sensor(DOOR_CLOSED_BIT, 0);
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Scenario #2: Break-in (ต้องอยู่โหมด Armed)
    ESP_LOGI(TAG, "➡️  Scenario: Break-in");
    push_system(USER_AWAY_BIT, 1000);
    push_system(SECURITY_ARMED_BIT, 1000);
    push_sensor(DOOR_OPENED_BIT, 1000);
    push_sensor(MOTION_DETECTED_BIT, 0);
    vTaskDelay(pdMS_TO_TICKS(12000)); // รอ emergency clear

    // Scenario #3: Goodnight Routine
    ESP_LOGI(TAG, "➡️  Scenario: Goodnight");
    push_system(USER_HOME_BIT, 1500);
    push_sensor(LIGHT_OFF_BIT, 500);
    push_sensor(MOTION_DETECTED_BIT, 1500);
    push_sensor(LIGHT_OFF_BIT, 0);

    ESP_LOGI(TAG, "✅ Scenarios injected. Idling...");
    for (;;) vTaskDelay(pdMS_TO_TICKS(60000));
}

// ========= APP MAIN =========
void app_main(void) {
    ESP_LOGI(TAG, "🚀 Complex Event Patterns - Smart Home System Starting...");

    // Configure GPIO (outputs)
    gpio_set_direction(LED_LIVING_ROOM, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_KITCHEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_BEDROOM, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SECURITY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_EMERGENCY, GPIO_MODE_OUTPUT);

    // Initialize LEDs off
    gpio_set_level(LED_LIVING_ROOM, 0);
    gpio_set_level(LED_KITCHEN, 0);
    gpio_set_level(LED_BEDROOM, 0);
    gpio_set_level(LED_SECURITY, 0);
    gpio_set_level(LED_EMERGENCY, 0);

    // Mutex
    state_mutex = xSemaphoreCreateMutex();
    if (!state_mutex) { ESP_LOGE(TAG, "Failed to create state mutex!"); return; }

    // Event groups
    sensor_events = xEventGroupCreate();
    system_events = xEventGroupCreate();
    pattern_events = xEventGroupCreate();
    if (!sensor_events || !system_events || !pattern_events) {
        ESP_LOGE(TAG, "Failed to create event groups!"); return;
    }
    ESP_LOGI(TAG, "Event groups created successfully");

    xEventGroupSetBits(system_events, SYSTEM_INIT_BIT);
    change_home_state(HOME_STATE_IDLE);

    // Core tasks
    xTaskCreate(pattern_recognition_task, "PatternEngine", 4096, NULL, 8, NULL);
    xTaskCreate(state_machine_task,       "StateMachine",   3072, NULL, 7, NULL);
    xTaskCreate(adaptive_learning_task,   "Learning",       3072, NULL, 5, NULL);
    xTaskCreate(status_monitor_task,      "Monitor",        3072, NULL, 3, NULL);

    // Sensor simulation tasks (จะถูกพักอัตโนมัติเมื่อ scenario_mode = true)
    xTaskCreate(motion_sensor_task,       "MotionSensor",   2048, NULL, 6, NULL);
    xTaskCreate(door_sensor_task,         "DoorSensor",     2048, NULL, 6, NULL);
    xTaskCreate(light_control_task,       "LightControl",   2048, NULL, 6, NULL);
    xTaskCreate(environmental_sensor_task,"EnvSensors",     2048, NULL, 5, NULL);

    // เปิดโหมดเดโมและเรียกตัวฉีดสถานการณ์
    scenario_mode = true;
    xTaskCreate(scenario_injector_task,   "ScenarioDemo",   3072, NULL, 9, NULL);

    ESP_LOGI(TAG, "All tasks created successfully");
    ESP_LOGI(TAG, "\n🎯 Smart Home LED Indicators:");
    ESP_LOGI(TAG, "  GPIO2  - Living Room Light");
    ESP_LOGI(TAG, "  GPIO4  - Kitchen Light");
    ESP_LOGI(TAG, "  GPIO5  - Bedroom Light");
    ESP_LOGI(TAG, "  GPIO18 - Security System");
    ESP_LOGI(TAG, "  GPIO19 - Emergency Mode");

    ESP_LOGI(TAG, "\n🤖 System Features:");
    ESP_LOGI(TAG, "  • Event-driven State Machine");
    ESP_LOGI(TAG, "  • Pattern Recognition Engine");
    ESP_LOGI(TAG, "  • Adaptive Learning System");
    ESP_LOGI(TAG, "  • Smart Home Automation");
    ESP_LOGI(TAG, "  • Complex Event Correlation");

    ESP_LOGI(TAG, "\n🔍 Monitored Patterns:");
    for (int i = 0; i < NUM_PATTERNS; i++) {
        ESP_LOGI(TAG, "  • %s", event_patterns[i].name);
    }
    ESP_LOGI(TAG, "Complex Event Pattern System operational!");
}