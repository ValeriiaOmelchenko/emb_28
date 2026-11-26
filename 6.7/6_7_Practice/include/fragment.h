#pragma once
// Навмисна фрагментація heap для кроків 3 і 4. Інструмент демонстрації, НЕ зразок для виробу.
//
// Сценарій той самий, що на слайді «A=50, B=20, C=15 КБ, free(B)», лише на справжньому heap:
//   1) заповнюємо heap блоками різного розміру (8-20 КБ) упритул один до одного;
//   2) дрібні залишки в кінці регіонів затикаємо «пробками» по 512 Б, щоб вони ні з чим не злились;
//   3) звільняємо кожен другий блок. Вільно сумарно - сотні КБ, але кожна дірка оточена
//      зайнятими сусідами, злити їх алокатор не може. Найбільший суцільний шматок <= 20 КБ.
//
// Поки heap заповнений, нічого не виділяємо ні тут, ні в інших задачах: ESP_LOGx і printf
// працюють без heap (буфер stdout створено ще при першому лозі на старті).

#include <stdbool.h>
#include <stdint.h>

#include "practice.h"

#define FRAG_CAPS        ((uint32_t)(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) // ті самі регіони, що й malloc() без PSRAM
#define FRAG_MAX_BLOCKS  UINT32_C(64)  // 64 x 8 КБ = 512 КБ - більше, ніж внутрішньої SRAM у S3
#define FRAG_MAX_PLUGS   UINT32_C(256)
#define FRAG_PLUG_BYTES  UINT32_C(512)
#define FRAG_BLOCK_MAX_B (UINT32_C(20) * BYTES_PER_KB) // найбільший блок = найбільша майбутня дірка

// «Різнорозмірні буфери в циклі»: розміри навмисно чергуються. static const - таблиця лежить у флеші.
static const uint32_t k_frag_sizes_b[] = {
    UINT32_C(12) * BYTES_PER_KB, FRAG_BLOCK_MAX_B,           UINT32_C(16) * BYTES_PER_KB,
    UINT32_C(8) * BYTES_PER_KB,  FRAG_BLOCK_MAX_B,           UINT32_C(12) * BYTES_PER_KB,
    UINT32_C(16) * BYTES_PER_KB, UINT32_C(8) * BYTES_PER_KB,
};
#define FRAG_SIZES_COUNT ((uint32_t)(sizeof(k_frag_sizes_b) / sizeof(k_frag_sizes_b[0])))

typedef struct {
  void *blocks[FRAG_MAX_BLOCKS];
  uint32_t block_count;
  void *plugs[FRAG_MAX_PLUGS];
  uint32_t plug_count;
} frag_state_t;

// Крок 1-2: заповнити heap блоками різного розміру, залишки заткнути пробками.
// plot = true - після кожного блоку рядок для Serial Plotter.
static inline void frag_fill(frag_state_t *s, bool plot) {
  uint32_t fails_in_row = 0;
  for (uint32_t i = 0; (s->block_count < FRAG_MAX_BLOCKS) && (fails_in_row < FRAG_SIZES_COUNT); i++) {
    void *p = heap_caps_malloc(k_frag_sizes_b[i % FRAG_SIZES_COUNT], FRAG_CAPS);
    if (p == NULL) {
      fails_in_row++; // цей розмір уже не влазить, пробуємо наступний (менший може влізти)
      continue;
    }
    fails_in_row = 0;
    s->blocks[s->block_count++] = p;
    if (plot) {
      plot_heap();
    }
  }
  while (s->plug_count < FRAG_MAX_PLUGS) {
    void *p = heap_caps_malloc(FRAG_PLUG_BYTES, FRAG_CAPS);
    if (p == NULL) {
      break;
    }
    s->plugs[s->plug_count++] = p;
  }
}

// Крок 3: звільнити кожен другий блок. Сусіди зайняті - дірки не зливаються.
static inline void frag_free_every_second(frag_state_t *s, bool plot) {
  for (uint32_t i = 1; i < s->block_count; i += 2) {
    heap_caps_free(s->blocks[i]);
    s->blocks[i] = NULL;
    if (plot) {
      plot_heap();
    }
  }
}

// Прибрати все. Коли звільнено сусідів, алокатор зливає дірки назад у великі шматки.
static inline void frag_release_all(frag_state_t *s) {
  for (uint32_t i = 0; i < s->block_count; i++) {
    heap_caps_free(s->blocks[i]); // heap_caps_free(NULL) нічого не робить, як і free(NULL)
    s->blocks[i] = NULL;
  }
  for (uint32_t i = 0; i < s->plug_count; i++) {
    heap_caps_free(s->plugs[i]);
    s->plugs[i] = NULL;
  }
  s->block_count = 0;
  s->plug_count = 0;
}
