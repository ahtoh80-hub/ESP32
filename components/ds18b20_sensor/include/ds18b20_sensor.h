// ============================================================================
//  components/ds18b20_sensor/include/ds18b20_sensor.h
//
//  Публичный API библиотеки для работы с датчиками DS18B20 по шине 1-Wire.
// ============================================================================

#pragma once
// Предотвращает повторное включение этого заголовка. [наш ds18b20_sensor.h]

#include <stdbool.h>
// Даёт тип bool, true, false. [стандарт C, stdbool.h]

#include "esp_err.h"
// Даёт esp_err_t, ESP_OK, ESP_FAIL, ESP_ERR_*. [ESP-IDF, esp_err.h]

#ifdef __cplusplus
extern "C"
{
#endif
    // Оборачивает объявления для использования из C++. [стандарт C/C++]

#define DS18B20_MAX_SENSORS 4
    // Максимум датчиков на одной шине; задаёт размер массива s_sensors[]. [наш ds18b20_sensor.h]

#define DS18B20_TEMP_MIN_VALID (-55.0f)
    // Нижняя допустимая температура DS18B20. [наш ds18b20_sensor.h]

#define DS18B20_TEMP_MAX_VALID (125.0f)
    // Верхняя допустимая температура DS18B20. [наш ds18b20_sensor.h]

    typedef enum
    {
        DS18B20_STATUS_OK = 0,  // всё хорошо. [наш ds18b20_sensor.h]
        DS18B20_STATUS_WARNING, // температура вне диапазона. [наш ds18b20_sensor.h]
        DS18B20_STATUS_ERROR,   // датчик не отвечает / ошибка CRC. [наш ds18b20_sensor.h]
    } ds18b20_status_t;
    // Наш enum статуса одного датчика. [наш ds18b20_sensor.h]

    typedef struct
    {
        float temperature;       // последняя прочитанная температура. [наш ds18b20_sensor.h]
        ds18b20_status_t status; // статус последнего чтения. [наш ds18b20_sensor.h]
    } ds18b20_reading_t;
    // Наша структура результата чтения. [наш ds18b20_sensor.h]

    // Возвращает esp_err_t — типовой код ошибки ESP-IDF. [ESP-IDF, esp_err.h]
    // Возможные значения: ESP_OK, ESP_ERR_NOT_FOUND, иные ESP_ERR_*.
    esp_err_t ds18b20_sensor_init(int gpio_num);
    // Инициализация шины и поиск всех датчиков. [наш ds18b20_sensor.h]

    // Возвращает int — обычное целое число. [стандарт C]
    int ds18b20_sensor_count(void);
    // Сколько датчиков найдено. [наш ds18b20_sensor.h]

    // Возвращает esp_err_t — типовой код ошибки ESP-IDF. [ESP-IDF, esp_err.h]
    // Возможные значения: ESP_OK, ESP_FAIL, ESP_ERR_INVALID_STATE.
    esp_err_t ds18b20_sensor_read_all(ds18b20_reading_t *readings);
    // Прочитать температуру со всех датчиков. [наш ds18b20_sensor.h]

    // Возвращает ds18b20_status_t — наш enum из этого же заголовка. [наш ds18b20_sensor.h]
    ds18b20_status_t ds18b20_sensor_overall_status(void);
    // «Худший» статус после последнего чтения. [наш ds18b20_sensor.h]

    // ------------------------------------------------------------------------
    //  ds18b20_sensor_run()
    //
    //  КРАТКО: Единая точка входа для main. Запускает indicator,
    //          инициализирует датчики и уходит в бесконечный цикл опроса.
    //
    //  Возвращает void — ничего. [стандарт C]
    //  Параметры: onewire_gpio — GPIO линии DQ. [стандарт C, int]
    //             rgb_led_gpio — GPIO встроенного WS2812. [стандарт C, int]
    // ------------------------------------------------------------------------
    void ds18b20_sensor_run(int onewire_gpio, int rgb_led_gpio);

#ifdef __cplusplus
}
#endif
// Закрытие extern "C". [стандарт C/C++]