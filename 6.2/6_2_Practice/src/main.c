#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define BLINK_GPIO 0

static const char *TAG = "SCHEDULER_TEST";

// Task 1: Low Priority (Пріоритет 1)
void HeartbeatTask(void *pvParameters) {
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    while (1) {
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

// Task 2: Medium Priority (Пріоритет 2)
void HeavyMathTask(void *pvParameters) {
    volatile uint32_t a = 0;
    while (1) {
        a += 1;
        a /= 13;
        if (a > 1000000) a = 0;

        //vTaskDelay(pdMS_TO_TICKS(1)); 
    }
}

// Task 3: High Priority (Пріоритет 3)
void CriticalMonitorTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    
    while (1) {
        ESP_LOGI(TAG, "System OK. Core: %d", xPortGetCoreID());
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting RTOS Scheduler test...");

    xTaskCreatePinnedToCore(HeartbeatTask, "Heartbeat", 2048, NULL, 1, NULL, 0);

    // Ці задачі завжди на Core 1
    xTaskCreatePinnedToCore(HeavyMathTask,       "HeavyMath", 2048, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(CriticalMonitorTask, "Monitor",   2048, NULL, 3, NULL, 1);
}