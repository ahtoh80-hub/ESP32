# ============================================================================
#  main/CMakeLists.txt
#
#  Описание компонента "main". Это единственный обязательный компонент
#  в любом проекте ESP-IDF.
# ============================================================================

# idf_component_register — команда из ESP-IDF (tools/cmake/component.cmake).
idf_component_register(
    SRCS "main.c"              # какие .c файлы собирать
    INCLUDE_DIRS "."           # где искать заголовки этого компонента
    REQUIRES ds18b20_sensor
    # REQUIRES — список наших компонентов, от которых зависит main.
    # Достаточно одного ds18b20_sensor: он тянет за собой indicator и rgb_led
    # (см. components/ds18b20_sensor/CMakeLists.txt), а main их API не использует.
)