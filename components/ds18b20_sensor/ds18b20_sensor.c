// ============================================================================
//  components/ds18b20_sensor/ds18b20_sensor.c
//
//  Реализация библиотеки DS18B20.
//  Публикует своё состояние в агрегатор indicator.
// ============================================================================

#include "ds18b20_sensor.h"
// Собственный публичный заголовок. [наш ds18b20_sensor.h]

#include "indicator.h"
// Агрегатор индикации — сюда публикуем ошибки/предупреждения. [наш indicator.h]

#include "esp_log.h"
// ESP_LOGI/LOGW/LOGE. [ESP-IDF, esp_log.h]

#include "onewire_bus.h"
// Типы и функции шины 1-Wire. [espressif/onewire_bus, onewire_bus.h]

#include "ds18b20.h"
// Типы и функции датчика DS18B20. [espressif/ds18b20, ds18b20.h]

#include "freertos/FreeRTOS.h"
// pdMS_TO_TICKS, vTaskDelay. [FreeRTOS, freertos/FreeRTOS.h]

#include "freertos/task.h"
// vTaskDelay. [FreeRTOS, freertos/task.h]

#include <inttypes.h>
// PRIx64 для печати uint64_t. [стандарт C, inttypes.h]

#include <stdbool.h>
// bool, true, false. [стандарт C, stdbool.h]

static const char *TAG = "DS18B20_SENSOR";
// Тег логов. [наш ds18b20_sensor.c]

#define READ_PERIOD_MS 2000
// Период основного цикла чтения, мс. [наш ds18b20_sensor.c]

static onewire_bus_handle_t s_bus = NULL;
// Дескриптор шины 1-Wire. [espressif/onewire_bus, onewire_bus.h]

static ds18b20_device_handle_t s_sensors[DS18B20_MAX_SENSORS];
// Массив дескрипторов найденных датчиков. [espressif/ds18b20, ds18b20.h]

static int s_count = 0;
// Текущее количество найденных датчиков. [наш ds18b20_sensor.c]

static ds18b20_status_t s_overall_status = DS18B20_STATUS_ERROR;
// Общий статус после последнего чтения. [наш ds18b20_sensor.h]

// ============================================================================
//  publish_state()
//
//  КРАТКО: Публикует состояние датчиков в агрегатор indicator.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: worst — «худший» статус среди всех датчиков. [наш ds18b20_sensor.h]
// ============================================================================
static void publish_state(ds18b20_status_t worst)
{
    switch (worst)
    {
    case DS18B20_STATUS_OK:
        // Всё хорошо. [наш ds18b20_sensor.h]

        indicator_clear_error(INDICATOR_SRC_DS18B20);
        // Снимаем ошибку связи для DS18B20. [наш indicator.h]

        indicator_set_warning(INDICATOR_SRC_DS18B20, false);
        // Снимаем предупреждение. [наш indicator.h]

        break;
        // Выход из switch. [стандарт C]

    case DS18B20_STATUS_WARNING:
        // Температура вне диапазона, но связь есть. [наш ds18b20_sensor.h]

        indicator_clear_error(INDICATOR_SRC_DS18B20);
        // Ошибки связи нет. [наш indicator.h]

        indicator_set_warning(INDICATOR_SRC_DS18B20, true);
        // Ставим предупреждение. [наш indicator.h]

        break;
        // Выход из switch. [стандарт C]

    case DS18B20_STATUS_ERROR:
    default:
        // Связь потеряна / CRC / нет ответа. [наш ds18b20_sensor.h]

        indicator_report_error(INDICATOR_SRC_DS18B20);
        // Ставим ошибку связи для DS18B20. [наш indicator.h]

        indicator_set_warning(INDICATOR_SRC_DS18B20, false);
        // Предупреждение неактуально при ошибке. [наш indicator.h]

        break;
        // Выход из switch. [стандарт C]
    }
}

// ============================================================================
//  ds18b20_sensor_init()
//
//  КРАТКО: Создаёт шину, ищет DS18B20, ставит разрешение 12 бит.
//
//  Возвращает esp_err_t — типовой код ошибки ESP-IDF. [ESP-IDF, esp_err.h]
//  Параметры: gpio_num — GPIO линии DQ. [стандарт C, int]
// ============================================================================
esp_err_t ds18b20_sensor_init(int gpio_num)
{
    if (s_bus != NULL)
    {
        // Шина уже создана — повторно не создаём. [наш ds18b20_sensor.c]

        ESP_LOGW(TAG, "Шина уже инициализирована");
        // Предупреждение в лог. [ESP-IDF, esp_log.h]

        return ESP_OK;
        // Возвращаем успех. [ESP-IDF, esp_err.h]
    }

    onewire_bus_config_t bus_config = {
        .bus_gpio_num = gpio_num,
        // GPIO линии DQ. [espressif/onewire_bus, onewire_bus.h]
    };
    // Конфигурация шины 1-Wire. [espressif/onewire_bus, onewire_bus.h]

    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10,
        // Максимальный размер ответа, байт. [espressif/onewire_bus, onewire_bus.h]
    };
    // Конфигурация RMT-бэкенда. [espressif/onewire_bus, onewire_bus.h]

    esp_err_t err = onewire_new_bus_rmt(&bus_config, &rmt_config, &s_bus);
    // Создаём шину 1-Wire поверх RMT. [espressif/onewire_bus, onewire_bus.h]

    if (err != ESP_OK)
    {
        // Не удалось создать шину. [ESP-IDF, esp_err.h]

        ESP_LOGE(TAG, "Не удалось создать шину 1-Wire на GPIO%d", gpio_num);
        // Логируем ошибку. [ESP-IDF, esp_log.h]

        return err;
        // Возвращаем код ошибки. [ESP-IDF, esp_err.h]
    }

    onewire_device_iter_handle_t iter = NULL;
    // Итератор для перебора устройств. [espressif/onewire_bus, onewire_bus.h]

    onewire_device_t next_device;
    // Структура очередного найденного устройства. [espressif/onewire_bus, onewire_bus.h]

    s_count = 0;
    // Сбрасываем счётчик датчиков. [наш ds18b20_sensor.c]

    err = onewire_new_device_iter(s_bus, &iter);
    // Создаём итератор для поиска устройств. [espressif/onewire_bus, onewire_bus.h]

    if (err != ESP_OK)
    {
        // Итератор не создан. [ESP-IDF, esp_err.h]

        ESP_LOGE(TAG, "Не удалось создать итератор устройств");
        // Логируем ошибку. [ESP-IDF, esp_log.h]

        return err;
        // Возвращаем код ошибки. [ESP-IDF, esp_err.h]
    }

    esp_err_t search_result;
    // Код возврата очередного шага поиска. [ESP-IDF, esp_err.h]

    do
    {
        // Перебираем устройства, пока есть. [стандарт C]

        search_result = onewire_device_iter_get_next(iter, &next_device);
        // Берём следующее устройство. [espressif/onewire_bus, onewire_bus.h]

        if (search_result == ESP_OK)
        {
            // Устройство найдено — пробуем распознать как DS18B20. [ESP-IDF, esp_err.h]

            ds18b20_config_t ds_cfg = {};
            // Конфигурация DS18B20 по умолчанию. [espressif/ds18b20, ds18b20.h]

            if (ds18b20_new_device_from_enumeration(&next_device, &ds_cfg,
                                                    &s_sensors[s_count]) == ESP_OK)
            {
                // Это DS18B20 — дескриптор сохранён. [espressif/ds18b20, ds18b20.h]

                onewire_device_address_t address;
                // 64-битный ROM-адрес. [espressif/onewire_bus, onewire_bus.h]

                ds18b20_get_device_address(s_sensors[s_count], &address);
                // Получаем адрес датчика. [espressif/ds18b20, ds18b20.h]

                ESP_LOGI(TAG, "Найден DS18B20[%d], адрес: %016" PRIx64, s_count, address);
                // Логируем адрес. [ESP-IDF, esp_log.h]

                s_count++;
                // Увеличиваем счётчик. [наш ds18b20_sensor.c]

                if (s_count >= DS18B20_MAX_SENSORS)
                {
                    // Достигли лимита датчиков. [наш ds18b20_sensor.h]

                    ESP_LOGW(TAG, "Достигнут лимит датчиков");
                    // Предупреждение в лог. [ESP-IDF, esp_log.h]

                    break;
                    // Выходим из цикла поиска. [стандарт C]
                }
            }
            else
            {
                // Устройство не DS18B20. [espressif/ds18b20, ds18b20.h]

                ESP_LOGW(TAG, "Найдено неизвестное устройство");
                // Предупреждение в лог. [ESP-IDF, esp_log.h]
            }
        }
    } while (search_result != ESP_ERR_NOT_FOUND);
    // Продолжаем, пока есть устройства. [ESP-IDF, esp_err.h]

    onewire_del_device_iter(iter);
    // Освобождаем итератор. [espressif/onewire_bus, onewire_bus.h]

    if (s_count == 0)
    {
        // Ни одного DS18B20 не найдено. [наш ds18b20_sensor.c]

        ESP_LOGE(TAG, "Датчики DS18B20 не найдены");
        // Логируем ошибку. [ESP-IDF, esp_log.h]

        s_overall_status = DS18B20_STATUS_ERROR;
        // Фиксируем статус ошибки. [наш ds18b20_sensor.h]

        indicator_report_error(INDICATOR_SRC_DS18B20);
        // Сообщаем агрегатору об ошибке. [наш indicator.h]

        return ESP_ERR_NOT_FOUND;
        // Возвращаем код ошибки. [ESP-IDF, esp_err.h]
    }

    for (int i = 0; i < s_count; i++)
    {
        // Для каждого найденного датчика. [стандарт C]

        ds18b20_set_resolution(s_sensors[i], DS18B20_RESOLUTION_12B);
        // Устанавливаем разрешение 12 бит. [espressif/ds18b20, ds18b20.h]
    }

    ESP_LOGI(TAG, "Инициализация завершена, найдено датчиков: %d", s_count);
    // Логируем успех. [ESP-IDF, esp_log.h]

    s_overall_status = DS18B20_STATUS_OK;
    // Начальный статус — OK. [наш ds18b20_sensor.h]

    publish_state(s_overall_status);
    // Публикуем состояние в indicator. [наш indicator.h]

    return ESP_OK;
    // Возвращаем успех. [ESP-IDF, esp_err.h]
}

// ============================================================================
//  ds18b20_sensor_count()
//
//  КРАТКО: Возвращает число найденных датчиков.
//
//  Возвращает int — обычное целое число. [стандарт C]
//  Параметры: нет.
// ============================================================================
int ds18b20_sensor_count(void)
{
    return s_count;
    // Отдаём статическую переменную. [наш ds18b20_sensor.c]
}

// ============================================================================
//  ds18b20_sensor_read_all()
//
//  КРАТКО: Читает температуру со всех датчиков, публикует статус.
//
//  Возвращает esp_err_t — типовой код ошибки ESP-IDF. [ESP-IDF, esp_err.h]
//  Параметры: readings — массив результатов. [наш ds18b20_sensor.h]
// ============================================================================
esp_err_t ds18b20_sensor_read_all(ds18b20_reading_t *readings)
{
    if (s_count == 0 || readings == NULL)
    {
        // Шина не инициализирована или массив не передан. [стандарт C]

        return ESP_ERR_INVALID_STATE;
        // Возвращаем код ошибки. [ESP-IDF, esp_err.h]
    }

    ds18b20_status_t worst = DS18B20_STATUS_OK;
    // «Худший» статус среди всех датчиков. [наш ds18b20_sensor.h]

    bool any_ok = false;
    // Флаг: хотя бы один датчик прочитан успешно. [стандарт C, stdbool.h]

    for (int i = 0; i < s_count; i++)
    {
        // Перебираем все найденные датчики. [стандарт C]

        esp_err_t err1 = ds18b20_trigger_temperature_conversion(s_sensors[i]);
        // Запускаем преобразование температуры. [espressif/ds18b20, ds18b20.h]

        float temperature = 0.0f;
        // Сюда запишем результат. [стандарт C]

        esp_err_t err2 = ds18b20_get_temperature(s_sensors[i], &temperature);
        // Читаем температуру. [espressif/ds18b20, ds18b20.h]

        if (err1 != ESP_OK || err2 != ESP_OK || temperature == -196.60f)
        {
            // Любая ошибка или -196.60 °C — ошибка датчика. [espressif/ds18b20, ds18b20.h]

            ESP_LOGE(TAG, "DS18B20[%d] ошибка чтения (err1=%d, err2=%d)", i, err1, err2);
            // Логируем ошибку. [ESP-IDF, esp_log.h]

            readings[i].temperature = 0.0f;
            // Обнуляем температуру. [наш ds18b20_sensor.h]

            readings[i].status = DS18B20_STATUS_ERROR;
            // Статус — ошибка. [наш ds18b20_sensor.h]

            worst = DS18B20_STATUS_ERROR;
            // Общий статус — ошибка. [наш ds18b20_sensor.h]

            continue;
            // Переходим к следующему датчику. [стандарт C]
        }

        readings[i].temperature = temperature;
        // Сохраняем прочитанную температуру. [наш ds18b20_sensor.h]

        if (temperature < DS18B20_TEMP_MIN_VALID || temperature > DS18B20_TEMP_MAX_VALID)
        {
            // Температура вне диапазона. [наш ds18b20_sensor.h]

            ESP_LOGW(TAG, "DS18B20[%d] температура вне диапазона: %.2f °C", i, temperature);
            // Логируем предупреждение. [ESP-IDF, esp_log.h]

            readings[i].status = DS18B20_STATUS_WARNING;
            // Статус — предупреждение. [наш ds18b20_sensor.h]

            if (worst != DS18B20_STATUS_ERROR)
            {
                // Если ошибки ещё не было. [наш ds18b20_sensor.h]

                worst = DS18B20_STATUS_WARNING;
                // Общий статус — предупреждение. [наш ds18b20_sensor.h]
            }
        }
        else
        {
            // Всё в порядке. [наш ds18b20_sensor.h]

            ESP_LOGI(TAG, "DS18B20[%d] Температура: %.2f °C", i, temperature);
            // Логируем температуру. [ESP-IDF, esp_log.h]

            readings[i].status = DS18B20_STATUS_OK;
            // Статус — OK. [наш ds18b20_sensor.h]

            any_ok = true;
            // Отмечаем успешное чтение. [стандарт C, stdbool.h]
        }
    }

    s_overall_status = worst;
    // Сохраняем общий статус. [наш ds18b20_sensor.h]

    publish_state(worst);
    // Публикуем состояние в indicator. [наш indicator.h]

    return any_ok ? ESP_OK : ESP_FAIL;
    // Успех, если хоть один датчик прочитан. [ESP-IDF, esp_err.h]
}

// ============================================================================
//  ds18b20_sensor_overall_status()
//
//  КРАТКО: Возвращает «худший» статус после последнего чтения.
//
//  Возвращает ds18b20_status_t — наш enum. [наш ds18b20_sensor.h]
//  Параметры: нет.
// ============================================================================
ds18b20_status_t ds18b20_sensor_overall_status(void)
{
    return s_overall_status;
    // Отдаём статическую переменную. [наш ds18b20_sensor.c]
}

// ============================================================================
//  ds18b20_sensor_run()
//
//  КРАТКО: Единая точка входа. Запускает indicator, инициализирует датчики
//          и уходит в бесконечный цикл чтения.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: onewire_gpio — GPIO линии DQ. [стандарт C, int]
//             rgb_led_gpio — GPIO встроенного WS2812. [стандарт C, int]
// ============================================================================
void ds18b20_sensor_run(int onewire_gpio, int rgb_led_gpio)
{
    if (indicator_init(rgb_led_gpio) != ESP_OK)
    {
        // Не удалось запустить индикатор. [наш indicator.h]

        ESP_LOGE(TAG, "Не удалось инициализировать indicator");
        // Логируем ошибку. [ESP-IDF, esp_log.h]

        return;
        // Выходим — работать без индикации нет смысла. [стандарт C]
    }

    if (ds18b20_sensor_init(onewire_gpio) != ESP_OK)
    {
        // Не удалось инициализировать датчики. [наш ds18b20_sensor.h]

        ESP_LOGE(TAG, "Не удалось инициализировать DS18B20");
        // Логируем ошибку. [ESP-IDF, esp_log.h]

        indicator_report_error(INDICATOR_SRC_DS18B20);
        // Сообщаем агрегатору об ошибке. [наш indicator.h]

        while (1)
        {
            // Бесконечная задержка — красное мигание продолжается. [FreeRTOS, freertos/task.h]

            vTaskDelay(pdMS_TO_TICKS(1000));
            // Ждём 1 секунду и повторяем. [FreeRTOS, freertos/task.h]
        }
    }

    int count = ds18b20_sensor_count();
    // Сколько датчиков нашли. [наш ds18b20_sensor.h]

    ESP_LOGI(TAG, "Найдено датчиков: %d", count);
    // Логируем количество. [ESP-IDF, esp_log.h]

    ds18b20_reading_t readings[DS18B20_MAX_SENSORS] = {0};
    // Буфер результатов чтения. [наш ds18b20_sensor.h]

    while (1)
    {
        // Основной цикл опроса. [FreeRTOS, freertos/task.h]

        esp_err_t err = ds18b20_sensor_read_all(readings);
        // Читаем все датчики. [наш ds18b20_sensor.h]

        if (err != ESP_OK)
        {
            // Чтение завершилось с ошибками. [ESP-IDF, esp_err.h]

            ESP_LOGW(TAG, "Чтение завершилось с ошибками");
            // Логируем предупреждение. [ESP-IDF, esp_log.h]
        }

        vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
        // Ждём период до следующего чтения. [FreeRTOS, freertos/task.h]
    }
}