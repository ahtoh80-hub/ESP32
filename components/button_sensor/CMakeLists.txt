# ============================================================================	
#  components/button_sensor/CMakeLists.txt
#	
#  Описание компонента "button_sensor" — библиотека дискретной кнопки.	
# ============================================================================

idf_component_register(
    SRCS "button_sensor.c"      # какие .c файлы собирать
    INCLUDE_DIRS "include"      # где искать публичные заголовки
    REQUIRES esp_driver_gpio indicator
    # esp_driver_gpio — драйвер GPIO состава ESP-IDF v6 (gpio_config,
    # gpio_get_level); имя компонента = имя из components/ (см. esp-idf-lib).
    # indicator — наш агрегатор индикации.
)