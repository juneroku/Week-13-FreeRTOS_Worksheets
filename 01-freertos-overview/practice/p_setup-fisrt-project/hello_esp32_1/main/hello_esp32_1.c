#include <stdio.h>
#include <inttypes.h>          // เพิ่มมาเพื่อใช้ PRIu32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

void app_main(void)
{
    printf("=== My First ESP32 Project ===\n");
    printf("Chip model: %s\n", esp_get_idf_version());
    printf("Free heap: %" PRIu32 " bytes\n", (uint32_t)esp_get_free_heap_size());

    int counter = 0;
    while (1) {
        printf("Running for %d seconds\n", counter++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}