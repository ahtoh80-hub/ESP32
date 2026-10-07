// ============================================================================
//  components/button_sensor/include/button_sensor.h
//
//  Самодостаточная библиотека дискретной кнопки (один GPIO, внешних
//  компонентов схемы не нужно: только кнопка между GPIO и GND — подтяжка
//  внутренняя, включается программно). Библиотека сама опрашивает вход
//  фоновой задачей (по умолчанию раз в 20 мс), отрабатывает антидребезг
//  (50 мс) и хранит последние показания — main читает их в любой момент,
//  не обращаясь к GPIO. Устройство одно; форма API совместима с
//  dht11_sensor/ds18b20_sensor.
//  Пример использования из main:
//
//      button_sensor_run(6, 48);              // индикация + кнопка + фоновый опрос
//      button_reading_t btn;                  // снимок в стиле dht11_reading_t rh
//      if (button_sensor_get_reading(&btn) == ESP_OK)
//          bool pressed = btn.pressed;        // как rh.temperature в dht11
// ============================================================================

#pragma once
// Предотвращает повторное включение этого заголовка. [наш button_sensor.h]

#include <stdbool.h>
// Даёт тип bool, true, false. [стандарт C, stdbool.h]

#include <stdint.h>
// Даёт uint32_t. [стандарт C, stdint.h]

#include "esp_err.h"
// Даёт esp_err_t, ESP_OK, ESP_ERR_*. [ESP-IDF, esp_err.h]

#ifdef __cplusplus
extern "C"
{
#endif
    // Оборачивает объявления для использования из C++. [стандарт C/C++]

#define BUTTON_PERIOD_MS 20
    // Период фонового опроса по умолчанию, мс — 50 опросов в секунду. [наш button_sensor.h]

#define BUTTON_PERIOD_MIN_MS 20
    // Минимальный период, мс: подтверждение дебаунса и так занимает
    // BUTTON_DEBOUNCE_MS, опрос чаще 20 мс смысла не имеет. [наш button_sensor.h]

#define BUTTON_DEBOUNCE_MS 50
    // Антидребезг, мс: новое состояние подтверждается, если вход держится
    // неизменным столько времени подряд. [наш button_sensor.h]

#define BUTTON_BOUNCE_MAX_MS 500
    // Максимально допустимая длительность дребезга, мс: дольше — статус
    // WARNING «контакт нестабилен» (жёлтая индикация). [наш button_sensor.h]

#define BUTTON_ACTIVE_LEVEL 0
    // Активный уровень входа: 0 — кнопка замыкает GPIO на GND (внутренняя
    // подтяжка к 3.3 В; подключение по умолчанию); 1 — кнопка замыкает
    // GPIO на 3.3 В (внутренняя подтяжка к GND). [наш button_sensor.h]

    typedef enum
    {
        BUTTON_STATUS_OK = 0,  // вход стабилен, данные актуальны. [наш button_sensor.h]
        BUTTON_STATUS_WARNING, // дребезг дольше BUTTON_BOUNCE_MAX_MS. [наш button_sensor.h]
        BUTTON_STATUS_ERROR,   // нет init() / данных ещё нет. [наш button_sensor.h]
    } button_status_t;
    // Наш enum статуса кнопки (форма совместима с ds18b20/dht11). [наш button_sensor.h]

    typedef struct
    {
        bool pressed;           // состояние с учётом дебаунса: true — нажата. [наш button_sensor.h]
        uint32_t presses;       // сколько раз нажата с момента init(). [стандарт C, stdint.h]
        button_status_t status; // статус последнего опроса. [наш button_sensor.h]
    } button_reading_t;
    // Наша структура снимка кнопки — аналог dht11_reading_t. [наш button_sensor.h]

    typedef void (*button_on_readings_t)(const button_reading_t *reading);
    // Колбэк подтверждённого изменения состояния (нажатие/отпускание):
    // reading — снимок кнопки (валиден только внутри вызова). Вызывается
    // в контексте задачи опроса (или того, кто вызвал read()), вне локов;
    // без изменения состояния не вызывается. [наш button_sensor.h]

    const char *button_sensor_status_name(button_status_t status);
    // Текстовое имя статуса для вывода: "OK" / "WARN" / "ERROR". [наш button_sensor.h]

    esp_err_t button_sensor_init(int gpio_num);
    // Настроить GPIO как вход с внутренней подтяжкой (направление — по
    // BUTTON_ACTIVE_LEVEL) и создать мьютексы. Кнопка при этом НЕ опрашивается:
    // состояние проверяет первый цикл (до этого статус ERROR — «данных ещё нет»).
    // Возвращает ESP_OK, ESP_ERR_INVALID_ARG (неверный GPIO), ESP_ERR_NO_MEM
    // (нет памяти под мьютексы), код gpio_config(). Повторный вызов безопасен. [наш button_sensor.h]

    esp_err_t button_sensor_deinit(void);
    // Остановить опрос и освободить ресурсы. Остановка кооперативная:
    // задача завершается сама (ждём до 5 с, затем принудительное удаление).
    // Не вызывать одновременно с read()/геттерами из других задач.
    // После этого библиотеку можно инициализировать снова. [наш button_sensor.h]

    void button_sensor_set_period_ms(int period_ms);
    // Задать период фонового опроса, мс (по умолчанию BUTTON_PERIOD_MS).
    // Значения меньше BUTTON_PERIOD_MIN_MS заменяются на BUTTON_PERIOD_MS.
    // Вызывать до button_sensor_start(). [наш button_sensor.h]

    esp_err_t button_sensor_start(void);
    // Запустить фоновый опрос. Без него можно читать вручную через read(). [наш button_sensor.h]

    int button_sensor_count(void);
    // Сколько кнопок обслуживает библиотека: 1 после init(), 0 иначе.
    // Кнопка одна; форма API совместима с ds18b20_sensor/dht11_sensor. [наш button_sensor.h]

    esp_err_t button_sensor_read(button_reading_t *reading);
    // Живой опрос: один сэмпл входа + антидребезг, обновление кэша,
    // публикация состояния в indicator и вызов колбэка (при изменении).
    // reading может быть NULL. Возвращает ESP_OK, ESP_ERR_INVALID_STATE (нет init()). [наш button_sensor.h]

    esp_err_t button_sensor_get_reading(button_reading_t *reading);
    // Снимок из кэша (pressed + presses + статус) одним захватом мьютекса —
    // согласованные данные без обращения к GPIO; в стиле dht11:
    //     button_reading_t btn;
    //     if (button_sensor_get_reading(&btn) == ESP_OK)
    //         bool pressed = btn.pressed;
    // ESP_ERR_INVALID_ARG при NULL, ESP_ERR_INVALID_STATE до init(). [наш button_sensor.h]

    bool button_sensor_is_pressed(void);
    // Нажата ли кнопка сейчас, с учётом дебаунса (из кэша). false до init(). [наш button_sensor.h]

    uint32_t button_sensor_press_count(void);
    // Сколько раз кнопка нажата с момента init() (из кэша). 0 до init(). [наш button_sensor.h]

    button_status_t button_sensor_get_status(void);
    // Статус последнего опроса. ERROR до init(). [наш button_sensor.h]

    button_status_t button_sensor_overall_status(void);
    // «Худший» статус; кнопка одна — совпадает с get_status() (форма API
    // совместима с ds18b20_sensor/dht11_sensor). [наш button_sensor.h]

    void button_sensor_set_callback(button_on_readings_t callback);
    // Задать колбэк изменения состояния (NULL — отменить).
    // Задавать до button_sensor_start()/run(). [наш button_sensor.h]

    esp_err_t button_sensor_run(int button_gpio, int rgb_led_gpio);
    // Единая точка входа для main: индикация + кнопка + фоновый опрос.
    // Возвращается сразу; возвращает результат init()/start() (сбой
    // индикации не критичен — его код не попадает в возврат). [наш button_sensor.h]

#ifdef __cplusplus
}
#endif
// Закрытие extern "C". [стандарт C/C++]
