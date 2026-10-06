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
    REQUIRES ds18b20_sensor dht11_sensor
    # REQUIRES — список наших компонентов, от которых зависит main.
    # ds18b20_sensor тянет за собой indicator и rgb_led — main их API
    # не использует напрямую. dht11_sensor — датчик DHT11 на GPIO5. [наш main/CMakeLists.txt]
)