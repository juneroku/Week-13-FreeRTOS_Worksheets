// main/hello_esp32.c
#include <stdio.h>
#include <inttypes.h>            // สำหรับ PRIu32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "build_info.h"


#define STRINGIFY2(x) #x
#define STRINGIFY(x)  STRINGIFY2(x)

void app_main(void)
{
    printf("=== %s v%s ===\n", PROJECT_NAME, PROJECT_VERSION);
    printf("Built on: %s %s\n", BUILD_DATE, BUILD_TIME);
    printf("ESP-IDF Version: %s\n", esp_get_idf_version());
    printf("Chip: %s\n", STRINGIFY(CONFIG_IDF_TARGET));

    // esp_get_free_heap_size() -> uint32_t
    printf("Free heap: %" PRIu32 " bytes\n", (uint32_t)esp_get_free_heap_size());

    while (1) {
        printf("System is running...\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}