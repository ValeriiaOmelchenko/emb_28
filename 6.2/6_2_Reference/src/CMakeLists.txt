idf_component_register(SRCS "main.c")

# Суворіші попередження для нашого коду (ESP-IDF уже додає -Wall, стандарт C - gnu17).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Wshadow)
