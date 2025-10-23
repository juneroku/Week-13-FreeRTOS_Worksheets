#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    printf("Hello, ESP32 World!\n");
    
    // Keep the program running
    while(1) {
        printf("ESP32 is running...\n");
        vTaskDelay(pdMS_TO_TICKS(1000)); // 1 second delay
    }
}