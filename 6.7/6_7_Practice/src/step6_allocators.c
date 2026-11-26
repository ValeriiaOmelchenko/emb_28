// Крок 6: malloc() / pvPortMalloc() / heap_caps_malloc() з явними MALLOC_CAP_*.
// Нічого підключати не треба. Для цього кроку sdkconfig.step6.defaults вмикає PSRAM
// (Octal, як на ESP32-S3-DevKitC-1 N16R8). Плата без PSRAM теж завантажиться
// (CONFIG_SPIRAM_IGNORE_NOTFOUND), просто SPIRAM-виділення повернуть NULL.
//
// Для кожного виділення друкуємо: куди потрапив блок (внутрішня SRAM чи PSRAM),
// чи годиться він для DMA і скільки байт heap він зайняв насправді.
// В ESP-IDF усі три функції зрештою кличуть heap_caps_malloc(). Різниця - хто обирає регіон:
//   malloc()        - libc: дрібне у SRAM, велике (> CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) може піти в PSRAM;
//   pvPortMalloc()  - порт FreeRTOS: завжди MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
//   heap_caps_malloc(size, caps) - розробник, явно.
// Наприкінці - швидкість: memset 64 КБ у SRAM проти PSRAM.

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "practice.h"

#define SPEED_TEST_B   (UINT32_C(64) * BYTES_PER_KB) // більше за кеш PSRAM, щоб не міряти кеш
#define SPEED_REPEATS  UINT32_C(8)

// Поріг, вище якого malloc() може віддати PSRAM. Без PSRAM-malloc - усе у SRAM.
#if CONFIG_SPIRAM_USE_MALLOC
#define MALLOC_ALWAYS_INTERNAL_B ((uint32_t)CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)
#else
#define MALLOC_ALWAYS_INTERNAL_B UINT32_MAX
#endif

static const char *TAG = "alloc";

typedef enum {
  ALLOCATOR_MALLOC,
  ALLOCATOR_PVPORT,
  ALLOCATOR_CAPS,
} allocator_t;

typedef struct {
  const char *label;
  allocator_t how;
  uint32_t caps; // лише для ALLOCATOR_CAPS
  uint32_t bytes;
} alloc_case_t;

// static const - таблиця у флеші, не в RAM.
static const alloc_case_t k_cases[] = {
    {.label = "malloc(1 КБ)", .how = ALLOCATOR_MALLOC, .bytes = UINT32_C(1) * BYTES_PER_KB},
    {.label = "malloc(64 КБ)", .how = ALLOCATOR_MALLOC, .bytes = UINT32_C(64) * BYTES_PER_KB},
    {.label = "pvPortMalloc(64 КБ)", .how = ALLOCATOR_PVPORT, .bytes = UINT32_C(64) * BYTES_PER_KB},
    {.label = "caps INTERNAL 64 КБ",
     .how = ALLOCATOR_CAPS,
     .caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
     .bytes = UINT32_C(64) * BYTES_PER_KB},
    {.label = "caps DMA 4 КБ", .how = ALLOCATOR_CAPS, .caps = MALLOC_CAP_DMA, .bytes = UINT32_C(4) * BYTES_PER_KB},
    {.label = "caps SPIRAM 1 МБ", .how = ALLOCATOR_CAPS, .caps = MALLOC_CAP_SPIRAM, .bytes = UINT32_C(1024) * BYTES_PER_KB},
    {.label = "caps INTERNAL 1 МБ",
     .how = ALLOCATOR_CAPS,
     .caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
     .bytes = UINT32_C(1024) * BYTES_PER_KB}, // стільки внутрішньої SRAM немає взагалі -> NULL
};
#define CASES_COUNT (sizeof(k_cases) / sizeof(k_cases[0]))

// Виконуваного heap (MALLOC_CAP_EXEC) на S3 за замовчуванням немає: захист памʼяті (PMS)
// забороняє виконувати код із даних. Тоді ESP-IDF навіть не визначає макрос MALLOC_CAP_EXEC.
#ifdef MALLOC_CAP_EXEC
#define EXEC_FREE_B() ((uint32_t)heap_caps_get_free_size(MALLOC_CAP_EXEC))
#else
#define EXEC_FREE_B() UINT32_C(0)
#endif

static void print_regions(const char *label) {
  ESP_LOGI(TAG, "%s: INTERNAL %zu B | DMA %zu B | SPIRAM %zu B | EXEC %" PRIu32 " B | esp_get_free_heap_size %" PRIu32
           " B",
           label, heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_DMA),
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM), EXEC_FREE_B(), esp_get_free_heap_size());
}

static void *case_alloc(const alloc_case_t *c) {
  switch (c->how) {
  case ALLOCATOR_MALLOC:
    return malloc(c->bytes);
  case ALLOCATOR_PVPORT:
    return pvPortMalloc(c->bytes);
  case ALLOCATOR_CAPS:
    return heap_caps_malloc(c->bytes, c->caps);
  }
  return NULL;
}

// Звільняти тією функцією, «парною» до виділення. В ESP-IDF вони взаємозамінні,
// на STM32 (heap_4 + newlib) free() блоку з pvPortMalloc() зламає heap.
static void case_free(const alloc_case_t *c, void *p) {
  switch (c->how) {
  case ALLOCATOR_MALLOC:
    free(p);
    break;
  case ALLOCATOR_PVPORT:
    vPortFree(p);
    break;
  case ALLOCATOR_CAPS:
    heap_caps_free(p);
    break;
  }
}

static void run_case(const alloc_case_t *c) {
  void *p = case_alloc(c);
  if (p == NULL) {
    ESP_LOGW(TAG, "%-22s -> NULL (немає такого регіону або суцільного шматка)", c->label);
    return;
  }
  const char *where = esp_ptr_external_ram(p) ? "PSRAM" : (esp_ptr_internal(p) ? "SRAM" : "?");
  // heap_caps_get_allocated_size: скільки блок зайняв насправді (вирівнювання і службові байти).
  ESP_LOGI(TAG, "%-22s -> %p  %-5s  DMA: %-3s  зайнято в heap %zu B", c->label, p, where,
           esp_ptr_dma_capable(p) ? "так" : "ні", heap_caps_get_allocated_size(p));
  case_free(c, p);
}

// Скільки тактів CPU займає memset буфера: SRAM поруч з ядром, PSRAM - через SPI-шину і кеш.
static uint32_t memset_cycles(uint8_t *buf) {
  const uint32_t t0 = esp_cpu_get_cycle_count();
  for (uint32_t r = 0; r < SPEED_REPEATS; r++) {
    memset(buf, (int)r, SPEED_TEST_B);
  }
  return (esp_cpu_get_cycle_count() - t0) / SPEED_REPEATS;
}

static void speed_test(void) {
  uint8_t *sram = heap_caps_malloc(SPEED_TEST_B, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint8_t *psram = heap_caps_malloc(SPEED_TEST_B, MALLOC_CAP_SPIRAM);
  if (sram != NULL) {
    ESP_LOGI(TAG, "memset 64 КБ у SRAM:  %" PRIu32 " тактів", memset_cycles(sram));
  }
  if (psram != NULL) {
    ESP_LOGI(TAG, "memset 64 КБ у PSRAM: %" PRIu32 " тактів", memset_cycles(psram));
  } else {
    ESP_LOGW(TAG, "PSRAM немає - порівняння швидкості пропущено");
  }
  heap_caps_free(sram); // heap_caps_free(NULL) безпечний
  heap_caps_free(psram);
}

void app_main(void) {
  print_regions("старт");
  ESP_LOGI(TAG, "malloc() понад %" PRIu32 " B може піти в PSRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)",
           MALLOC_ALWAYS_INTERNAL_B);

  for (size_t i = 0; i < CASES_COUNT; i++) {
    run_case(&k_cases[i]);
  }
  speed_test();
  print_regions("кінець");
}
