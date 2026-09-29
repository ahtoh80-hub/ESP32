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

static led_strip_handle_t s_led = NULL;
// Дескриптор светодиодной ленты. [espressif/led_strip, led_strip.h]

const rgb_color_t RGB_COLOR_OFF = {0, 0, 0};        // выключен. [наш rgb_led.h]
const rgb_color_t RGB_COLOR_GREEN = {0, 255, 0};    // норма. [наш rgb_led.h]
const rgb_color_t RGB_COLOR_YELLOW = {255, 255, 0}; // предупреждение. [наш rgb_led.h]
const rgb_color_t RGB_COLOR_RED = {255, 0, 0};      // ошибка. [наш rgb_led.h]

// ============================================================================
//  rgb_led_init()
//
//  КРАТКО: Создаёт драйвер led_strip для одного WS2812 и гасит светодиод.
//
//  Возвращает esp_err_t — типовой код ошибки ESP-IDF. [ESP-IDF, esp_err.h]
//  Возможные значения: ESP_OK, иные ESP_ERR_*.
//
//  Параметры: gpio_num — GPIO светодиода. [стандарт C, int]
// ============================================================================
esp_err_t rgb_led_init(int gpio_num)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio_num, // GPIO светодиода. [espressif/led_strip, led_strip.h]
        .max_leds = 1,              // у нас один светодиод. [espressif/led_strip, led_strip.h]
    };
    // Конфигурация «ленты». [espressif/led_strip, led_strip.h]

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,    // тактирование по умолчанию. [espressif/led_strip, led_strip.h]
        .resolution_hz = 10 * 1000 * 1000, // 10 МГц → тик 0.1 мкс. [espressif/led_strip, led_strip.h]
        .flags.with_dma = false,           // DMA для 1 LED не нужен. [espressif/led_strip, led_strip.h]
    };
    // Конфигурация RMT-бэкенда. [espressif/led_strip, led_strip.h]

    // led_strip_new_rmt_device() возвращает esp_err_t. [ESP-IDF, esp_err.h]
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led);
    // Создаём драйвер; дескриптор пишется в s_led. [espressif/led_strip, led_strip.h]

    if (err != ESP_OK)
    {
        // Не удалось создать драйвер — логируем и выходим.
        ESP_LOGE(TAG, "Не удалось создать led_strip на GPIO%d", gpio_num); // [ESP-IDF, esp_log.h]
        return err;                                                        // [ESP-IDF, esp_err.h]
    }

    // led_strip_clear() возвращает void. [стандарт C]
    led_strip_clear(s_led);
    // Гасим светодиод на старте. [espressif/led_strip, led_strip.h]

    ESP_LOGI(TAG, "RGB-светодиод инициализирован на GPIO%d", gpio_num); // [ESP-IDF, esp_log.h]
    return ESP_OK;                                                      // [ESP-IDF, esp_err.h]
}

// ============================================================================
//  rgb_led_set()
//
//  КРАТКО: Записывает цвет в буфер и сразу отправляет на светодиод.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: color — указатель на rgb_color_t. [наш rgb_led.h]
// ============================================================================
void rgb_led_set(const rgb_color_t *color)
{
    if (s_led == NULL || color == NULL)
    {
        // Драйвер не создан или цвет не передан — выходим.
        return; // [стандарт C]
    }

    // led_strip_set_pixel() возвращает void. [стандарт C]
    led_strip_set_pixel(s_led, 0, color->r, color->g, color->b);
    // Устанавливаем цвет пикселя 0. [espressif/led_strip, led_strip.h]

    // led_strip_refresh() возвращает void. [стандарт C]
    led_strip_refresh(s_led);
    // Отправляем данные на физическую ленту. [espressif/led_strip, led_strip.h]
}

// ============================================================================
//  rgb_led_off()
//
//  КРАТКО: Гасит светодиод.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: нет.
// ============================================================================
void rgb_led_off(void)
{
    if (s_led == NULL)
    {
        // Драйвер не создан — выходим.
        return; // [стандарт C]
    }

    // led_strip_clear() возвращает void. [стандарт C]
    led_strip_clear(s_led);
    // Гасим все пиксели. [espressif/led_strip, led_strip.h]
}