// Домашнє завдання 6.7 - рефакторинг.
//
// Метеостанція: два "сенсори" (температура і вологість) складають виміри у свої буфери,
// задача звіту раз на 2 секунди рахує середнє, мінімум і максимум, друкує звіт
// у монітор порта і очищає буфери. Сенсор вологості іноді "зависає" на кілька секунд.
//
// Код компілюється і на перший погляд працює. Подивіться на нього очима embedded-розробника:
// що станеться через годину, добу, тиждень роботи?

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    char *name;
    float *data;
    int count;
} sensor_buf_t;

sensor_buf_t *temp_buf;
sensor_buf_t *hum_buf;

void temp_task(void *arg)
{
    while (1) {
        float value = 20.0 + (esp_random() % 100) / 10.0;

        temp_buf->data = realloc(temp_buf->data, (temp_buf->count + 1) * sizeof(float));
        temp_buf->data[temp_buf->count] = value;
        temp_buf->count++;

        vTaskDelay(10);
    }
}

void hum_task(void *arg)
{
    while (1) {
        float value = 40.0 + (esp_random() % 300) / 10.0;

        hum_buf->data = realloc(hum_buf->data, (hum_buf->count + 1) * sizeof(float));
        hum_buf->data[hum_buf->count] = value;
        hum_buf->count++;

        if (esp_random() % 50 == 0) {
            vTaskDelay(300); // сенсор "завис"
        }
        vTaskDelay(5);
    }
}

void report_task(void *arg)
{
    while (1) {
        vTaskDelay(2000 / portTICK_PERIOD_MS);

        char *report = malloc(256);

        if (temp_buf->count == 0 || hum_buf->count == 0) {
            printf("No data yet\n");
            continue;
        }

        float t_sum = 0, t_min = 1000, t_max = -1000;
        for (int i = 0; i < temp_buf->count; i++) {
            t_sum += temp_buf->data[i];
            if (temp_buf->data[i] < t_min) t_min = temp_buf->data[i];
            if (temp_buf->data[i] > t_max) t_max = temp_buf->data[i];
        }

        float h_sum = 0, h_min = 1000, h_max = -1000;
        for (int i = 0; i < hum_buf->count; i++) {
            h_sum += hum_buf->data[i];
            if (hum_buf->data[i] < h_min) h_min = hum_buf->data[i];
            if (hum_buf->data[i] > h_max) h_max = hum_buf->data[i];
        }

        sprintf(report, "%s: avg=%.1f min=%.1f max=%.1f n=%d | %s: avg=%.1f min=%.1f max=%.1f n=%d | free heap: %d\n",
                temp_buf->name, t_sum / temp_buf->count, t_min, t_max, temp_buf->count,
                hum_buf->name, h_sum / hum_buf->count, h_min, h_max, hum_buf->count,
                (int)esp_get_free_heap_size());
        printf(report);

        free(temp_buf->data);
        temp_buf->data = NULL;
        temp_buf->count = 0;

        free(hum_buf->data);
        hum_buf->data = NULL;
        hum_buf->count = 0;

        free(report);
    }
}

void app_main(void)
{
    temp_buf = malloc(sizeof(sensor_buf_t));
    temp_buf->name = malloc(16);
    strcpy(temp_buf->name, "temperature");
    temp_buf->data = NULL;
    temp_buf->count = 0;

    hum_buf = malloc(sizeof(sensor_buf_t));
    hum_buf->name = malloc(16);
    strcpy(hum_buf->name, "humidity");
    hum_buf->data = NULL;
    hum_buf->count = 0;

    xTaskCreate(temp_task, "temp", 8192, NULL, 5, NULL);
    xTaskCreate(hum_task, "hum", 8192, NULL, 5, NULL);
    xTaskCreate(report_task, "report", 4096, NULL, 5, NULL);
}
