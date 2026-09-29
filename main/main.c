// ============================================================================
//  main/main.c
//
//  Точка входа проекта. Вся логика вынесена в ds18b20_sensor и indicator.
// ============================================================================

#include "ds18b20_sensor.h"
// Наша библиотека для DS18B20 (внутри неё запускается и indicator). [наш ds18b20_sensor.h]

#define ONEWIRE_BUS_GPIO 4
// GPIO линии DQ датчиков. [наш main.c]

#define RGB_LED_GPIO 48
// GPIO встроенного WS2812 на ESP32-S3-N16R8. [наш main.c]

void app_main(void)
{
    // Точка входа ESP-IDF. [ESP-IDF, app_main]

    ds18b20_sensor_run(ONEWIRE_BUS_GPIO, RGB_LED_GPIO);
    // Запускаем всю систему: индикатор + датчики + цикл опроса. [наш ds18b20_sensor.h]
}