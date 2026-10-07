# ============================================================================
#  components/ky008_laser/CMakeLists.txt
#
#  Описание компонента "ky008_laser" — библиотека лазерного модуля KY-008.
# ============================================================================

idf_component_register(
    SRCS "ky008_laser.c"        # какие .c файлы собирать
    INCLUDE_DIRS "include"      # где искать публичные заголовки
    REQUIRES esp_driver_gpio indicator
    # esp_driver_gpio — драйвер GPIO состава ESP-IDF v6 (gpio_config,
    # gpio_set_level, gpio_get_level).
    # indicator — наш агрегатор индикации.
)