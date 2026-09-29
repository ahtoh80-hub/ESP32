// ============================================================================
//  components/rgb_led/include/rgb_led.h
//
//  Публичный API библиотеки для управления адресным RGB-светодиодом.
// ============================================================================

#pragma once
// Защита от повторного включения. [наш rgb_led.h]

#include <stdint.h>
// uint8_t и другие типы фиксированной ширины. [стандарт C, stdint.h]

#include "esp_err.h"
// esp_err_t, ESP_OK. [ESP-IDF, esp_err.h]

#ifdef __cplusplus
extern "C"
{
#endif
    // Оборачивает объявления для использования из C++. [стандарт C/C++]

    typedef struct
    {
        uint8_t r; // красный канал 0..255. [стандарт C, stdint.h]
        uint8_t g; // зелёный канал 0..255. [стандарт C, stdint.h]
        uint8_t b; // синий канал 0..255. [стандарт C, stdint.h]
    } rgb_color_t;
    // Наш тип цвета. [наш rgb_led.h]

    extern const rgb_color_t RGB_COLOR_OFF;    // (0,0,0). [наш rgb_led.h]
    extern const rgb_color_t RGB_COLOR_GREEN;  // (0,255,0). [наш rgb_led.h]
    extern const rgb_color_t RGB_COLOR_YELLOW; // (255,255,0). [наш rgb_led.h]
    extern const rgb_color_t RGB_COLOR_RED;    // (255,0,0). [наш rgb_led.h]

    // Возвращает esp_err_t — типовой код ошибки ESP-IDF. [ESP-IDF, esp_err.h]
    // Возможные значения: ESP_OK, иные ESP_ERR_*.
    esp_err_t rgb_led_init(int gpio_num);
    // Инициализация светодиода на GPIO. [наш rgb_led.h]

    // Возвращает void — ничего. [стандарт C]
    void rgb_led_set(const rgb_color_t *color);
    // Установить цвет. [наш rgb_led.h]

    // Возвращает void — ничего. [стандарт C]
    void rgb_led_off(void);
    // Погасить светодиод. [наш rgb_led.h]

#ifdef __cplusplus
}
#endif
// Закрытие extern "C". [стандарт C/C++]