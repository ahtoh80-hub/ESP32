// ============================================================================
//  components/dht11_sensor/include/dht11_sensor.h
//
//  Самодостаточная библиотека DHT11 (температура + влажность, собственный
//  однопиновый протокол на одном GPIO). Устроена аналогично ds18b20_sensor:
//  библиотека сама опрашивает датчик фоновой задачей с заданным периодом
//  (по умолчанию 1 раз в секунду) и хранит последние показания — main может
//  читать их в любой момент без обращений к датчику.
//  Пример использования из main:
//
//      dht11_sensor_set_period_ms(1000);   // период опроса (по умолчанию 1000 мс)
//      dht11_sensor_run(5, 48);            // индикация + датчик + фоновый опрос
//      dht11_sensor_get_temperature();     // чтение из кэша, без опроса датчика
// ============================================================================

#pragma once
// Предотвращает повторное включение этого заголовка. [наш dht11_sensor.h]

#include <stdbool.h>
// Даёт тип bool, true, false. [стандарт C, stdbool.h]

#include <stdint.h>
// Стандартные типы фиксированной ширины. [стандарт C, stdint.h]

#include "esp_err.h"
// Даёт esp_err_t, ESP_OK, ESP_FAIL, ESP_ERR_*. [ESP-IDF, esp_err.h]

#ifdef __cplusplus
extern "C"
{
#endif
    // Оборачивает объявления для использования из C++. [стандарт C/C++]

#define DHT11_PERIOD_MS 1000
    // Период опроса по умолчанию, мс — 1 раз в секунду. [наш dht11_sensor.h]

#define DHT11_PERIOD_MIN_MS 1000
    // Минимальный период, мс: спецификация DHT11 — не чаще одного опроса
    // в секунду, иначе датчик отдаёт устаревшее значение. [наш dht11_sensor.h]

#define DHT11_TEMP_MIN_VALID (0.0f)
    // Нижняя паспортная температура DHT11, °C. [наш dht11_sensor.h]

#define DHT11_TEMP_MAX_VALID (50.0f)
    // Верхняя паспортная температура DHT11, °C. [наш dht11_sensor.h]

#define DHT11_HUM_MIN_VALID (20.0f)
    // Нижняя паспортная влажность DHT11, %. [наш dht11_sensor.h]

#define DHT11_HUM_MAX_VALID (80.0f)
    // Верхняя паспортная влажность DHT11, %. [наш dht11_sensor.h]

    typedef enum
    {
        DHT11_STATUS_OK = 0,  // всё хорошо. [наш dht11_sensor.h]
        DHT11_STATUS_WARNING, // температура/влажность вне паспортного диапазона. [наш dht11_sensor.h]
        DHT11_STATUS_ERROR,   // датчик не отвечает / ошибка контрольной суммы. [наш dht11_sensor.h]
    } dht11_status_t;
    // Наш enum статуса датчика. [наш dht11_sensor.h]

    typedef struct
    {
        float temperature;     // последняя прочитанная температура, °C. [наш dht11_sensor.h]
        float humidity;        // последняя прочитанная влажность, %. [наш dht11_sensor.h]
        dht11_status_t status; // статус последнего чтения. [наш dht11_sensor.h]
    } dht11_reading_t;
    // Наша структура результата чтения (снимок датчика). [наш dht11_sensor.h]

    typedef void (*dht11_on_readings_t)(const dht11_reading_t *reading);
    // Колбэк нового цикла чтения: reading — снимок датчика (валиден только
    // внутри вызова). Вызывается в контексте того, кто вызвал read()
    // (обычно задача опроса). Внутри колбэка read() не вызывать. [наш dht11_sensor.h]

    esp_err_t dht11_sensor_init(int gpio_num);
    // Инициализировать библиотеку на GPIO. Датчик при этом НЕ опрашивается:
    // после подачи питания ему нужно ≈1 с, поэтому наличие проверяет первый
    // цикл фонового опроса (до этого статус ERROR — «данных ещё нет»).
    // Возвращает ESP_OK или ESP_ERR_NO_MEM (нет памяти под мьютексы).
    // Повторный вызов безопасен. [наш dht11_sensor.h]

    esp_err_t dht11_sensor_deinit(void);
    // Остановить опрос и освободить ресурсы. Остановка кооперативная:
    // задача завершается сама (ждём до 5 с, затем принудительное удаление).
    // Не вызывать одновременно с read()/геттерами из других задач.
    // После этого библиотеку можно инициализировать снова. [наш dht11_sensor.h]

    void dht11_sensor_set_period_ms(int period_ms);
    // Задать период фонового опроса, мс (по умолчанию DHT11_PERIOD_MS).
    // Значения меньше DHT11_PERIOD_MIN_MS заменяются на DHT11_PERIOD_MS.
    // Вызывать до dht11_sensor_start(). [наш dht11_sensor.h]

    esp_err_t dht11_sensor_start(void);
    // Запустить фоновый опрос. Без него можно читать вручную через read(). [наш dht11_sensor.h]

    int dht11_sensor_count(void);
    // Сколько датчиков обслуживает библиотека: 1 после init(), 0 иначе.
    // DHT11 — одиночный датчик на GPIO; форма совместима с ds18b20_sensor. [наш dht11_sensor.h]

    esp_err_t dht11_sensor_read(dht11_reading_t *reading);
    // Опросить датчик прямо сейчас (блокирует ≈5 мс — передача кадра DHT11),
    // обновить кэш и вызвать колбэк. reading может быть NULL.
    // Возвращает ESP_OK (датчик ответил, даже если значение вне диапазона),
    // ESP_FAIL (датчик не ответил), ESP_ERR_INVALID_STATE (нет init()). [наш dht11_sensor.h]

    esp_err_t dht11_sensor_get_reading(dht11_reading_t *reading);
    // Снимок из кэша (температура + влажность + статус) одним захватом
    // мьютекса — согласованные данные без обращения к датчику.
    // ESP_ERR_INVALID_ARG при NULL, ESP_ERR_INVALID_STATE до init(). [наш dht11_sensor.h]

    float dht11_sensor_get_temperature(void);
    // Последняя температура, °C (из кэша). 0.0f до init(). [наш dht11_sensor.h]

    float dht11_sensor_get_humidity(void);
    // Последняя влажность, % (из кэша). 0.0f до init(). [наш dht11_sensor.h]

    dht11_status_t dht11_sensor_get_status(void);
    // Статус датчика из последнего чтения. ERROR до init(). [наш dht11_sensor.h]

    dht11_status_t dht11_sensor_overall_status(void);
    // «Худший» статус среди всех датчиков; у DHT11 датчик один,
    // поэтому совпадает с get_status(). [наш dht11_sensor.h]

    void dht11_sensor_set_callback(dht11_on_readings_t callback);
    // Задать колбэк, вызываемый после каждого цикла чтения (NULL — отменить).
    // Задавать до dht11_sensor_start()/run(). [наш dht11_sensor.h]

    const char *dht11_status_name(dht11_status_t status);
    // Текстовое имя статуса для вывода: "OK" / "WARN" / "ERROR". [наш dht11_sensor.h]

    esp_err_t dht11_sensor_run(int dht_gpio, int rgb_led_gpio);
    // Единая точка входа для main: индикация + датчик + фоновый опрос.
    // Возвращается сразу; возвращает результат init()/start() (индикация
    // не критична — её сбой не влияет на возврат). [наш dht11_sensor.h]

#ifdef __cplusplus
}
#endif
// Закрытие extern "C". [стандарт C/C++]
