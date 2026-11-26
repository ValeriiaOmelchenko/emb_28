cmake_minimum_required(VERSION 3.16.0)

# Опції sdkconfig під окремий крок: якщо поруч є sdkconfig.<env>.defaults
# (напр. sdkconfig.step6.defaults вмикає PSRAM), беремо його. Решта кроків - налаштування ESP-IDF за замовчуванням.
# PlatformIO збирає кожне середовище в .pio/build/<назва env>, це і є CMAKE_BINARY_DIR.
# Змінив файл .defaults - видали sdkconfig.<env>, інакше PlatformIO візьме старий.
get_filename_component(PIO_ENV "${CMAKE_BINARY_DIR}" NAME)
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.${PIO_ENV}.defaults")
    set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.${PIO_ENV}.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(practice_6_7)
