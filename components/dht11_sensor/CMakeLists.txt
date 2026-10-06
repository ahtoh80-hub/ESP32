idf_component_register(
    SRCS "dht11_sensor.c"       # какие .c файлы собирать
    INCLUDE_DIRS "include"      # где искать публичные заголовки
    REQUIRES "esp-idf-lib__dht" indicator
    # "esp-idf-lib__dht" — драйвер DHT11 из ESP Component Registry
    # (имя = имя_владельца + "__" + имя, см. build_name() в component manager).
    # indicator — наш агрегатор индикации.
)
