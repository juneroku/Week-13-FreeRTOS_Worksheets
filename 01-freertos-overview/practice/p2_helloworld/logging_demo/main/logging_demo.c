#include <stdio.h>
#include <inttypes.h>              // สำหรับ PRIu32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_chip_info.h"         // ESP-IDF v5+
#include "esp_flash.h"             // esp_flash_get_size()

// Define tag for logging
static const char *TAG = "LOGGING_DEMO";

void demonstrate_logging_levels(void)
{
    ESP_LOGE(TAG, "This is an ERROR message - highest priority");
    ESP_LOGW(TAG, "This is a WARNING message");
    ESP_LOGI(TAG, "This is an INFO message - default level");
    ESP_LOGD(TAG, "This is a DEBUG message - needs debug level");
    ESP_LOGV(TAG, "This is a VERBOSE message - needs verbose level");
}

void demonstrate_formatted_logging(void)
{
    int   temperature = 25;
    float voltage     = 3.3f;
    const char* status = "OK";

    ESP_LOGI(TAG, "Sensor readings:");
    ESP_LOGI(TAG, "  Temperature: %d°C", temperature);
    ESP_LOGI(TAG, "  Voltage: %.2fV", voltage);
    ESP_LOGI(TAG, "  Status: %s", status);

    // Hexadecimal dump
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    ESP_LOGI(TAG, "Data dump:");
    ESP_LOG_BUFFER_HEX(TAG, data, sizeof(data));
}

void demonstrate_conditional_logging(void)
{
    int error_code = 0;

    // Conditional logging
    if (error_code != 0) {
        ESP_LOGE(TAG, "Error occurred: code %d", error_code);
    } else {
        ESP_LOGI(TAG, "System is running normally");
    }

    // Using ESP_ERROR_CHECK
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized successfully");
}

// ช่วยให้พิมพ์ค่า CONFIG_IDF_TARGET ได้เป็นสตริง
#define STRINGIFY2(x) #x
#define STRINGIFY(x)  STRINGIFY2(x)

void app_main(void)
{
    // ---------- ตั้งระดับ log ตามที่อาจารย์แนะนำ ----------
    // Set log level for specific tag
    esp_log_level_set("LOGGING_DEMO", ESP_LOG_DEBUG);
    // Set global log level
    esp_log_level_set("*", ESP_LOG_INFO);
    // ------------------------------------------------------

    // System information
    ESP_LOGI(TAG, "=== ESP32 Hello World Demo ===");
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Chip Target: %s", STRINGIFY(CONFIG_IDF_TARGET));
    ESP_LOGI(TAG, "Free Heap: %" PRIu32 " bytes", (uint32_t)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Min Free Heap: %" PRIu32 " bytes",
             (uint32_t)esp_get_minimum_free_heap_size());

    // CPU info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip cores: %d, revision: %d, model: %d",
             chip_info.cores, chip_info.revision, chip_info.model);

    // Flash info (ใช้ esp_flash API)
    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "Flash size: %" PRIu32 " MB", flash_bytes / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "Cannot read flash size");
    }

    // Demonstrate different logging levels
    ESP_LOGI(TAG, "\n--- Logging Levels Demo ---");
    demonstrate_logging_levels();

    // Demonstrate formatted logging
    ESP_LOGI(TAG, "\n--- Formatted Logging Demo ---");
    demonstrate_formatted_logging();

    // Demonstrate conditional logging
    ESP_LOGI(TAG, "\n--- Conditional Logging Demo ---");
    demonstrate_conditional_logging();

    // Main loop with counter
    int counter = 0;
    while (1) {
        ESP_LOGI(TAG, "Main loop iteration: %d", counter++);

        // Log memory status every 10 iterations
        if (counter % 10 == 0) {
            ESP_LOGI(TAG, "Memory status - Free: %" PRIu32 " bytes",
                     (uint32_t)esp_get_free_heap_size());
        }

        // Simulate different log levels based on counter
        if (counter % 20 == 0) {
            ESP_LOGW(TAG, "Warning: Counter reached %d", counter);
        }

        if (counter > 50) {
            ESP_LOGE(TAG, "Error simulation: Counter exceeded 50!");
            counter = 0; // Reset counter
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); // 2 seconds
    }
}