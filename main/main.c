// ============================================================================
//  main/main.c
//
//  Точка входа. Запускает библиотеку DS18B20; вывод показаний оформлен
//  колбэком, который библиотека вызывает после каждого цикла опроса —
//  main не опрашивает шину и не крутит собственный цикл печати.
// ============================================================================

#include "ds18b20_sensor.h"
// Библиотека DS18B20. [наш ds18b20_sensor.h]

#include "dht11_sensor.h"
// Библиотека DHT11 (температура + влажность). [наш dht11_sensor.h]

#include "button_sensor.h"
// Библиотека дискретной кнопки. [наш button_sensor.h]

#include "ky008_laser.h"
// Библиотека лазерного модуля KY-008. [наш ky008_laser.h]

#include "esp_log.h"
// ESP_LOGI/LOGE. [ESP-IDF, esp_log.h]

#include "freertos/FreeRTOS.h"
// pdMS_TO_TICKS. [FreeRTOS, freertos/FreeRTOS.h]

#include "freertos/task.h"
// vTaskDelay. [FreeRTOS, freertos/task.h]

#include <inttypes.h>
// PRIx64 для печати адреса. [стандарт C, inttypes.h]

static const char *TAG = "MAIN";
// Тег логов. [наш main.c]

#define ONEWIRE_BUS_GPIO 4
// GPIO линии DQ датчиков. [наш main.c]

#define RGB_LED_GPIO 48
// GPIO встроенного WS2812 на ESP32-S3-N16R8. [наш main.c]

#define DHT11_GPIO 5
// GPIO линии DATA датчика DHT11 (подтяжка 4.7…10 кОм к 3.3 В). [наш main.c]

#define BUTTON_GPIO 6
// GPIO дискретной кнопки: второй контакт — GND, подтяжка внутренняя. [наш main.c]

#define SENSOR_PERIOD_MS 1000
// Период опроса датчиков, мс: 1 раз в секунду. [наш main.c]

#define IDLE_PERIOD_MS 1000
// Период «сна» основного цикла, мс (вывод идёт из колбэка). [наш main.c]

static void on_readings(const ds18b20_info_t *infos, int count)
{
    // Колбэк нового цикла чтения: печатаем строки показаний.
    // Вызывается задачей опроса библиотеки после каждого цикла. [наш ds18b20_sensor.h]

    for (int i = 0; i < count; i++)
    {
        ESP_LOGI(TAG, "DS18B20[%d] 0x%016" PRIx64 ": %.2f C (%s)",
                 i, infos[i].address, infos[i].temperature,
                 ds18b20_status_name(infos[i].status));
        // Статус — текстом из библиотеки. [наш ds18b20_sensor.h]
    }
}

void app_main(void)
{
    // Точка входа ESP-IDF. [ESP-IDF, app_main]

    ds18b20_sensor_set_period_ms(SENSOR_PERIOD_MS);
    // Задаём период опроса до запуска (можно любое значение ≥ 1000 мс). [наш ds18b20_sensor.h]

    ds18b20_sensor_set_callback(on_readings);
    // Вывод показаний — из колбэка, без цикла печати в main. [наш ds18b20_sensor.h]

    esp_err_t err = ds18b20_sensor_run(ONEWIRE_BUS_GPIO, RGB_LED_GPIO);
    // Индикация + датчики + фоновый опрос — одним вызовом. [наш ds18b20_sensor.h]

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Запуск DS18B20 завершился ошибкой (0x%X); DQ=GPIO%d, нужен резистор 4.7 кОм",
                 (unsigned)err, ONEWIRE_BUS_GPIO);
        // Подробности уже залогированы внутри init(); здесь — итог для main. [наш ds18b20_sensor.h]
    }

    esp_err_t dht_err = dht11_sensor_run(DHT11_GPIO, RGB_LED_GPIO);
    // DHT11: индикация + датчик + фоновый опрос — одним вызовом. [наш dht11_sensor.h]

    if (dht_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Запуск DHT11 завершился ошибкой (0x%X); DATA=GPIO%d",
                 (unsigned)dht_err, DHT11_GPIO);
        // Подробности уже залогированы внутри init(). [наш dht11_sensor.h]
    }

    esp_err_t btn_err = button_sensor_run(BUTTON_GPIO, RGB_LED_GPIO);
    // Кнопка: индикация + вход + фоновый опрос — одним вызовом. [наш button_sensor.h]

    if (btn_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Запуск кнопки завершился ошибкой (0x%X); кнопка на GPIO%d",
                 (unsigned)btn_err, BUTTON_GPIO);
        // Подробности уже залогированы внутри init(). [наш button_sensor.h]
    }

    esp_log_level_set("DHT11", ESP_LOG_DEBUG);
    // Диагностика: причины неответа DHT11 (таймаут/CRC) логируются на DEBUG
    // раз в цикл; убрать или вернуть ESP_LOG_INFO для релиза. [ESP-IDF, esp_log.h]

    while (1)
    {
        // Печать идёт ДО задержки: первые строки появляются сразу после запуска
        // (до первого чтения в кэше — «данных ещё нет»), дальше — раз в секунду.
        // Шину и датчики здесь не трогаем — только кэш. [наш ds18b20_sensor.h]

        if (ds18b20_sensor_count() > 0)
        {
            ds18b20_reading_t ds;
            // Снимок первого датчика (T + статус) одним захватом мьютекса —
            // в стиле dht11: dht11_reading_t rh. [наш ds18b20_sensor.h]

            if (ds18b20_sensor_get_reading(0, &ds) == ESP_OK)
            {
                float T1 = ds.temperature;
                // Текущая температура из кэша — расчёт/передача по T1 здесь. [наш ds18b20_sensor.h]

                ESP_LOGI(TAG, "T1 = %.2f C (%s)", T1, ds18b20_status_name(ds.status));
                // %.2f — выводим значение; статус рядом, чтобы видеть достоверность. [ESP-IDF, esp_log.h]
            }
        }

        if (dht11_sensor_count() > 0)
        {
            dht11_reading_t rh;
            // Снимок DHT11 (T + RH + статус) одним захватом мьютекса. [наш dht11_sensor.h]

            if (dht11_sensor_get_reading(&rh) == ESP_OK)
            {
                ESP_LOGI(TAG, "DHT11: T = %.2f C, RH = %.2f %% (%s)",
                         rh.temperature, rh.humidity, dht11_status_name(rh.status));
                // %% — литеральный знак процента в формате. [ESP-IDF, esp_log.h]
            }
        }

        if (button_sensor_count() > 0)
        {
            button_reading_t btn;
            // Снимок кнопки (нажатие + число нажатий + статус) одним захватом
            // мьютекса — в стиле dht11/ds18b20. [наш button_sensor.h]

            if (button_sensor_get_reading(&btn) == ESP_OK)
            {
                ESP_LOGI(TAG, "BUTTON: %s, нажатий: %lu (%s)",
                         btn.pressed ? "нажата" : "отпущена",
                         (unsigned long)btn.presses,
                         button_sensor_status_name(btn.status));
                // Статус рядом, чтобы видеть достоверность. [ESP-IDF, esp_log.h]
            }
        }

        vTaskDelay(pdMS_TO_TICKS(IDLE_PERIOD_MS));
        // Пауза между печатями. [FreeRTOS, freertos/task.h]
    }
}
