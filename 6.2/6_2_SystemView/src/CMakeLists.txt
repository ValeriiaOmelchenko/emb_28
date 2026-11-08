idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES app_trace)

# Суворіші попередження для нашого коду (ESP-IDF уже додає -Wall, стандарт C - gnu17).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Wshadow)
