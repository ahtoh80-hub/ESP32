// ============================================================================
//  components/indicator/include/indicator.h
//
//  Публичный API агрегатора индикации.
//  Собирает ошибки и предупреждения от любых источников (датчиков)
//  и управляет RGB-светодиодом через компонент rgb_led.
// ============================================================================

#pragma once
// Защита от повторного включения заголовка. [наш indicator.h]

#include <stdbool.h>
// Даёт тип bool, true, false. [стандарт C, stdbool.h]

#include "esp_err.h"
// Даёт esp_err_t, ESP_OK, ESP_FAIL, ESP_ERR_*. [ESP-IDF, esp_err.h]

#ifdef __cplusplus
extern "C"
{
#endif
    // Оборачивает объявления для использования из C++. [стандарт C/C++]

    typedef enum
    {
        INDICATOR_SRC_DS18B20 = 0, // источник: датчики DS18B20. [наш indicator.h]
        // INDICATOR_SRC_BME280,   // будущий источник: BME280. [наш indicator.h]
        // INDICATOR_SRC_SHT31,    // будущий источник: SHT31. [наш indicator.h]
        INDICATOR_SRC_MAX // количество источников (граница массива). [наш indicator.h]
    } indicator_source_t;
    // Перечисление источников ошибок. Расширяется при добавлении датчиков. [наш indicator.h]

    // Возвращает esp_err_t — типовой код ошибки ESP-IDF. [ESP-IDF, esp_err.h]
    // Возможные значения: ESP_OK, иные ESP_ERR_*.
    esp_err_t indicator_init(int gpio_num);
    // Инициализация агрегатора и запуск задачи индикации. [наш indicator.h]
    // gpio_num — GPIO встроенного WS2812. [стандарт C, int]

    // Возвращает void — ничего. [стандарт C]
    void indicator_report_error(indicator_source_t src);
    // Сообщить об ошибке связи от источника. [наш indicator.h]
    // Ошибка остаётся активной до вызова indicator_clear_error(). [наш indicator.h]

    // Возвращает void — ничего. [стандарт C]
    void indicator_clear_error(indicator_source_t src);
    // Снять ошибку связи от источника. [наш indicator.h]

    // Возвращает void — ничего. [стандарт C]
    void indicator_set_warning(indicator_source_t src, bool active);
    // Установить (active=true) или снять (active=false) предупреждение. [наш indicator.h]

#ifdef __cplusplus
}
#endif
// Закрытие extern "C". [стандарт C/C++]