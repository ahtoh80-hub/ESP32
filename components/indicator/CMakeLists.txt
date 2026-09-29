idf_component_register(
    SRCS "indicator.c"          # какие .c файлы собирать
    INCLUDE_DIRS "include"      # где искать публичные заголовки
    REQUIRES rgb_led            # зависим от библиотеки светодиода
)