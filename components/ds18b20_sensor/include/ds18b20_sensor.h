// ============================================================================
//  components/ds18b20_sensor/include/ds18b20_sensor.h
//
//  Самодостаточная библиотека DS18B20 (шина 1-Wire).
//  Библиотека сама создаёт шину на указанном GPIO, находит датчики,
//  опрашивает их с заданным периодом (по умолчанию 1 раз в секунду)
//  и хранит последние показания — main может читать их в любой момент.
//  Пример использования из main:
//
//      static void on_readings(const ds18b20_info_t *infos, int count)
//      {
//          for (int i = 0; i < count; i++)
//              printf("0x%016llX: %.2f C (%s)\n", infos[i].address,
//                     infos[i].temperature, ds18b20_status_name(infos[i].status));
//      }
//
//      ds18b20_sensor_set_period_ms(1000);   // период опроса (по умолчанию 1000 мс)
//      ds18b20_sensor_set_callback(on_readings); // вывод показаний из колбэка
//      ds18b20_sensor_run(4, 48);            // индикация + датчики + фоновый опрос
//      ds18b20_sensor_get_temperature(0);    // чтение из кэша, без обращений к шине
// ============================================================================

#pragma once
// Предотвращает повторное включение этого заголовка. [наш ds18b20_sensor.h]

#include <stdbool.h>
// Даёт тип bool, true, false. [стандарт C, stdbool.h]

#include <stdint.h>
// Даёт uint64_t. [стандарт C, stdint.h]

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

#define DS18B20_PERIOD_MS 1000
    // Период опроса по умолчанию, мс — 1 раз в секунду. [наш ds18b20_sensor.h]

#define DS18B20_PERIOD_MIN_MS 1000
    // Минимальный период, мс: преобразование 12 бит занимает ≈800 мс. [наш ds18b20_sensor.h]

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

    typedef struct
    {
        uint64_t address;        // ROM-адрес датчика. [стандарт C, stdint.h]
        float temperature;       // последняя прочитанная температура. [наш ds18b20_sensor.h]
        ds18b20_status_t status; // статус последнего чтения. [наш ds18b20_sensor.h]
    } ds18b20_info_t;
    // Полный снимок одного датчика: адрес + показания одним захватом мьютекса. [наш ds18b20_sensor.h]

    typedef void (*ds18b20_on_readings_t)(const ds18b20_info_t *infos, int count);
    // Колбэк нового цикла чтения: infos — снимки всех датчиков (валидны только
    // внутри вызова), count — их число. Вызывается в контексте того, кто вызвал
    // read_all() (обычно задача опроса). Внутри колбэка read_all() не вызывать. [наш ds18b20_sensor.h]

    const char *ds18b20_status_name(ds18b20_status_t status);
    // Текстовое имя статуса для вывода: "OK" / "WARN" / "ERROR". [наш ds18b20_sensor.h]

    esp_err_t ds18b20_sensor_init(int gpio_num);
    // Создать шину на GPIO, найти датчики, задать разрешение 12 бит.
    // Возвращает ESP_OK, ESP_ERR_NOT_FOUND (датчиков нет) или ESP_ERR_*. [наш ds18b20_sensor.h]

    esp_err_t ds18b20_sensor_deinit(void);
    // Остановить опрос и освободить шину с датчиками.
    // Остановка кооперативная: задача завершается сама (ждём до 5 с, затем
    // принудительное удаление). Не вызывать одновременно с read_all()/
    // геттерами из других задач — дождитесь окончания их вызовов.
    // После этого библиотеку можно инициализировать снова. [наш ds18b20_sensor.h]

    void ds18b20_sensor_set_period_ms(int period_ms);
    // Задать период фонового опроса, мс (по умолчанию DS18B20_PERIOD_MS).
    // Значения меньше DS18B20_PERIOD_MIN_MS заменяются на DS18B20_PERIOD_MS.
    // Вызывать до ds18b20_sensor_start(). [наш ds18b20_sensor.h]

    esp_err_t ds18b20_sensor_start(void);
    // Запустить фоновый опрос. Без него можно читать вручную через read_all(). [наш ds18b20_sensor.h]

    int ds18b20_sensor_count(void);
    // Сколько датчиков найдено. [наш ds18b20_sensor.h]

    esp_err_t ds18b20_sensor_read_all(ds18b20_reading_t *readings);
    // Опросить все датчики прямо сейчас (блокирует ≈800 мс — время преобразования).
    // readings может быть NULL — тогда результаты доступны через get_temperature(). [наш ds18b20_sensor.h]

    float ds18b20_sensor_get_temperature(int index);
    // Последняя температура датчика, °C (из кэша). 0.0f при неверном index. [наш ds18b20_sensor.h]

    ds18b20_status_t ds18b20_sensor_get_status(int index);
    // Статус датчика из последнего чтения. ERROR при неверном index. [наш ds18b20_sensor.h]

    ds18b20_status_t ds18b20_sensor_overall_status(void);
    // «Худший» статус среди всех датчиков. [наш ds18b20_sensor.h]

    esp_err_t ds18b20_sensor_get_address(int index, uint64_t *address);
    // 64-битный ROM-адрес датчика. ESP_ERR_INVALID_ARG при неверных параметрах. [наш ds18b20_sensor.h]

    esp_err_t ds18b20_sensor_get_info(int index, ds18b20_info_t *info);
    // Адрес + температура + статус одним захватом мьютекса — согласованный снимок.
    // ESP_ERR_INVALID_ARG при NULL/неверном index. [наш ds18b20_sensor.h]

    void ds18b20_sensor_set_callback(ds18b20_on_readings_t callback);
    // Задать колбэк, вызываемый после каждого цикла чтения (NULL — отменить).
    // Задавать до ds18b20_sensor_start()/run(). [наш ds18b20_sensor.h]

    esp_err_t ds18b20_sensor_run(int onewire_gpio, int rgb_led_gpio);
    // Единая точка входа для main: индикация + датчики + фоновый опрос.
    // Возвращается сразу; возвращает результат init()/start() (индикация
    // не критична — её сбой не влияет на возврат). Нет датчиков —
    // ESP_ERR_NOT_FOUND, count() вернёт 0. [наш ds18b20_sensor.h]

#ifdef __cplusplus
}
#endif
// Закрытие extern "C". [стандарт C/C++]