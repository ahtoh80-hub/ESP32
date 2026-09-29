idf_component_register(
    SRCS "ds18b20_sensor.c"     # какие .c файлы собирать
    INCLUDE_DIRS "include"      # где искать публичные заголовки
    REQUIRES espressif__ds18b20 espressif__onewire_bus indicator
    # espressif__ds18b20, espressif__onewire_bus — внешние драйверы из реестра
    # indicator — наш агрегатор индикации
)