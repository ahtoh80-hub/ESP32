// ============================================================================
//  components/rgb_led/rgb_led.c
//
//  Реализация библиотеки управления адресным RGB-светодиодом (WS2812).
// ============================================================================

#include "rgb_led.h"
// Собственный заголовок. [наш rgb_led.h]

#include "esp_log.h"
// ESP_LOGI/LOGW/LOGE. [ESP-IDF, esp_log.h]

#include "led_strip.h"
// Типы и функции драйвера WS2812. [espressif/led_strip, led_strip.h]

static const char *TAG = "RGB_LED";
// Тег логов. [наш rgb_led.c]

#define LED_STRIP_MAX_LEDS 1
// У нас один светодиод. [espressif/led_strip, led_strip.h]

#define LED_STRIP_RESOLUTION_HZ (10 * 1000 * 1000)
// 10 МГц → тик 0.1 мкс. [espressif/led_strip, led_strip.h]

static led_strip_handle_t s_led = NULL;
// Дескриптор светодиодной ленты. [espressif/led_strip, led_strip.h]

static rgb_color_t s_current = {0, 0, 0};
// Последний выведенный цвет — повторно не отправляется. [наш rgb_led.c]

const rgb_color_t RGB_COLOR_OFF = {0, 0, 0};        // выключен. [наш rgb_led.h]
const rgb_color_t RGB_COLOR_GREEN = {0, 255, 0};    // норма. [наш rgb_led.h]
const rgb_color_t RGB_COLOR_YELLOW = {255, 255, 0}; // предупреждение. [наш rgb_led.h]
const rgb_color_t RGB_COLOR_RED = {255, 0, 0};      // ошибка. [наш rgb_led.h]

// ============================================================================
//  rgb_led_init()
//
//  Создаёт драйвер led_strip для одного WS2812. Повторный вызов безопасен.
//
//  Возвращает esp_err_t: ESP_OK или код ошибки драйвера. [ESP-IDF, esp_err.h]
//  Параметры: gpio_num — GPIO светодиода. [стандарт C, int]
// ============================================================================
esp_err_t rgb_led_init(int gpio_num)
{
    if (s_led != NULL)
    {
        // Уже инициализировано — второй драйвер не создаём.
        return ESP_OK;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio_num,     // GPIO светодиода. [espressif/led_strip, led_strip.h]
        .max_leds = LED_STRIP_MAX_LEDS, // один светодиод. [espressif/led_strip, led_strip.h]
    };
    // Конфигурация «ленты». [espressif/led_strip, led_strip.h]

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,           // тактирование по умолчанию. [espressif/led_strip, led_strip.h]
        .resolution_hz = LED_STRIP_RESOLUTION_HZ, // 10 МГц → тик 0.1 мкс. [espressif/led_strip, led_strip.h]
        .flags.with_dma = false,                  // для 1 LED DMA не нужен. [espressif/led_strip, led_strip.h]
    };
    // Конфигурация RMT-бэкенда. [espressif/led_strip, led_strip.h]

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led);
    // Создаём драйвер; дескриптор пишется в s_led. [espressif/led_strip, led_strip.h]

    if (err != ESP_OK)
    {
        s_led = NULL;
        ESP_LOGE(TAG, "Не удалось создать led_strip на GPIO%d (0x%X)", gpio_num, (unsigned)err);
        return err;
    }

    s_current = RGB_COLOR_OFF;
    (void)led_strip_clear(s_led);
    // Гасим светодиод на старте. [espressif/led_strip, led_strip.h]

    ESP_LOGI(TAG, "RGB-светодиод инициализирован на GPIO%d", gpio_num);
    return ESP_OK;
}

// ============================================================================
//  rgb_led_deinit()
//
//  Освобождает драйвер светодиода. Повторный вызов безопасен.
//
//  Возвращает esp_err_t: ESP_OK или код ошибки драйвера. [ESP-IDF, esp_err.h]
// ============================================================================
esp_err_t rgb_led_deinit(void)
{
    if (s_led == NULL)
    {
        return ESP_OK;
    }

    esp_err_t err = led_strip_del(s_led);
    // Освобождаем ресурсы драйвера. [espressif/led_strip, led_strip.h]

    s_led = NULL;
    s_current = RGB_COLOR_OFF;
    return err;
}

// ============================================================================
//  rgb_led_set()
//
//  КРАТКО: Выводит цвет на светодиод. Если этот цвет уже горит, обращений
//          к RMT не будет.
//
//  Возвращает esp_err_t: ESP_OK или код ошибки драйвера. [ESP-IDF, esp_err.h]
//  Параметры: color — нужный цвет. [наш rgb_led.h]
// ============================================================================
esp_err_t rgb_led_set(const rgb_color_t *color)
{
    if (s_led == NULL || color == NULL)
    {
        // Драйвер не создан или цвет не передан.
        return ESP_ERR_INVALID_STATE;
    }

    if (color->r == s_current.r && color->g == s_current.g && color->b == s_current.b)
    {
        // Такой цвет уже выведен — повторно не отправляем.
        return ESP_OK;
    }

    esp_err_t err = led_strip_set_pixel(s_led, 0, color->r, color->g, color->b);
    // Записываем цвет пикселя 0. [espressif/led_strip, led_strip.h]

    if (err != ESP_OK)
    {
        return err;
    }

    err = led_strip_refresh(s_led);
    // Отправляем данные на светодиод. [espressif/led_strip, led_strip.h]

    if (err == ESP_OK)
    {
        s_current = *color;
    }

    return err;
}

// ============================================================================
//  rgb_led_off()
//
//  КРАТКО: Гасит светодиод.
//
//  Возвращает esp_err_t: ESP_OK или код ошибки драйвера. [ESP-IDF, esp_err.h]
// ============================================================================
esp_err_t rgb_led_off(void)
{
    if (s_led == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_current = RGB_COLOR_OFF;
    return led_strip_clear(s_led);
    // Гасим пиксели и обновляем светодиод. [espressif/led_strip, led_strip.h]
}