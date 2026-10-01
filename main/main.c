// ============================================================================
//  main/main.c
//
//  Точка входа. Запускает библиотеку DS18B20; вывод показаний оформлен
//  колбэком, который библиотека вызывает после каждого цикла опроса —
//  main не опрашивает шину и не крутит собственный цикл печати.
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

    while (1)
    {
        // Сначала ждём: первый цикл опроса завершится примерно через 800 мс
        // (время преобразования 12 бит) — раньше читать в кэше нечего. [наш ds18b20_sensor.h]
        vTaskDelay(pdMS_TO_TICKS(IDLE_PERIOD_MS));

        if (ds18b20_sensor_count() > 0)
        {
            float T1 = ds18b20_sensor_get_temperature(0);
            // Текущая температура первого датчика из кэша; шина не опрашивается. [наш ds18b20_sensor.h]

            ESP_LOGI(TAG, "T1 = %.2f C (%s)", T1,
                     ds18b20_status_name(ds18b20_sensor_get_status(0)));
            // %.2f — выводим значение; статус рядом, чтобы видеть достоверность. [ESP-IDF, esp_log.h]
        }

        // Расчёт/передача по T1 — здесь, внутри цикла, по свежему значению.
    }
}
