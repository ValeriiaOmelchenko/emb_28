# Кожне середовище [env:<назва>] збирає лише свій файл src/<назва>_*.c.
# PlatformIO не підтримує build_src_filter для ESP-IDF, тому вибираємо файл тут:
# PlatformIO збирає кожне середовище в .pio/build/<назва env>, це і є CMAKE_BINARY_DIR.
if(CMAKE_BUILD_EARLY_EXPANSION)
    # Ранній прохід ESP-IDF лише збирає залежності компонентів, джерела тут не потрібні.
    idf_component_register(PRIV_REQUIRES esp_driver_gpio esp_driver_uart)
    return()
endif()

get_filename_component(PIO_ENV "${CMAKE_BINARY_DIR}" NAME)
file(GLOB app_sources "${CMAKE_CURRENT_LIST_DIR}/${PIO_ENV}_*.c")
if(NOT app_sources)
    message(FATAL_ERROR "Немає файлу src/${PIO_ENV}_*.c для середовища ${PIO_ENV}")
endif()
# Спільний код кроків (драйвер вбудованого RGB-світлодіода) - src/common_*.c.
if(PIO_ENV MATCHES "^step")
    file(GLOB common_sources "${CMAKE_CURRENT_LIST_DIR}/common_*.c")
    list(APPEND app_sources ${common_sources})
endif()

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "../include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_uart)

# Суворіші попередження для коду кроків (ESP-IDF уже додає -Wall, стандарт C - gnu17).
# -Wall містить -Wswitch: switch по enum без default, де пропущено значення, - попередження.
# Приклад із лекції (lecture_alarm.c) лишаємо як є, без додаткових прапорців.
if(PIO_ENV MATCHES "^step")
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Wshadow -Wstrict-prototypes)
endif()
