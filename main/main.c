// ============================================================================
//  main/main.c
//
//  Точка входа. Запускает библиотеку DS18B20 и выводит показания в консоль.
// ============================================================================

#include "ds18b20_sensor.h"
// Библиотека DS18B20. [наш ds18b20_sensor.h]

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

#define SENSOR_PERIOD_MS 1000
// Период опроса датчиков, мс: 1 раз в секунду. [наш main.c]

#define PRINT_PERIOD_MS 1000
// Период вывода в консоль, мс. [наш main.c]

static const char *status_name(ds18b20_status_t status)
{
    // Текстовое имя статуса для консоли. [наш main.c]

    if (status == DS18B20_STATUS_OK)
    {
        return "OK";
    }

    if (status == DS18B20_STATUS_WARNING)
    {
        return "WARN";
    }

    return "ERROR";
}

void app_main(void)
{
    // Точка входа ESP-IDF. [ESP-IDF, app_main]

    ds18b20_sensor_set_period_ms(SENSOR_PERIOD_MS);
    // Задаём период опроса до запуска (можно любое значение ≥ 1000 мс). [наш ds18b20_sensor.h]

    ds18b20_sensor_run(ONEWIRE_BUS_GPIO, RGB_LED_GPIO);
    // Индикация + датчики + фоновый опрос — одним вызовом. [наш ds18b20_sensor.h]

    const int count = ds18b20_sensor_count();
    // Сколько датчиков найдено. [наш ds18b20_sensor.h]

    if (count == 0)
    {
        ESP_LOGE(TAG, "Датчики не найдены: проверьте DQ=GPIO%d и резистор 4.7 кОм",
                 ONEWIRE_BUS_GPIO);
    }

    while (1)
    {
        // Выводим последние показания из кэша библиотеки.
        // Шину здесь не трогаем — её опрашивает фоновая задача. [наш ds18b20_sensor.h]

        for (int i = 0; i < count; i++)
        {
            uint64_t address = 0;
            // ROM-адрес датчика. [стандарт C, stdint.h]

            ds18b20_sensor_get_address(i, &address);
            // Адрес берётся из дескриптора. [наш ds18b20_sensor.h]

            float t = ds18b20_sensor_get_temperature(i);
            // Температура, °C. [наш ds18b20_sensor.h]

            ESP_LOGI(TAG, "DS18B20[%d] 0x%016" PRIx64 ": %.2f C (%s)",
                     i, address, t, status_name(ds18b20_sensor_get_status(i)));
            // Печатаем строку показаний. [ESP-IDF, esp_log.h]
        }

        vTaskDelay(pdMS_TO_TICKS(PRINT_PERIOD_MS));
        // Ждём период вывода. [FreeRTOS, freertos/task.h]
    }
}
