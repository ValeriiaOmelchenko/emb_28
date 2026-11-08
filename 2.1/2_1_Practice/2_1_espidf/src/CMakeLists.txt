# Компілюються всі step*.cpp; потрібний крок обирає макрос PRACTICE_STEP (див. platformio.ini).
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.cpp)

idf_component_register(SRCS ${app_sources}
                       PRIV_REQUIRES esp_driver_gpio esp_timer esp_hw_support heap)
