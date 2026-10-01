// ============================================================================
//  components/ds18b20_sensor/ds18b20_sensor.c
//
//  Самодостаточная библиотека DS18B20 (шина 1-Wire).
//  Период фонового опроса — 1000 мс по умолчанию, задаётся через
//  ds18b20_sensor_set_period_ms(). Состояние публикуется в indicator.
// ============================================================================

#include "ds18b20_sensor.h"
// Собственный публичный заголовок. [наш ds18b20_sensor.h]

#include "indicator.h"
// Агрегатор индикации — сюда публикуем состояние. [наш indicator.h]

#include "esp_log.h"
// ESP_LOGI/LOGW/LOGE/LOGD. [ESP-IDF, esp_log.h]

#include "onewire_bus.h"
// Шина 1-Wire. [espressif/onewire_bus, onewire_bus.h]

#include "ds18b20.h"
// Датчик DS18B20. [espressif/ds18b20, ds18b20.h]

#include "freertos/FreeRTOS.h"
// Базовые типы FreeRTOS. [FreeRTOS, freertos/FreeRTOS.h]

#include "freertos/task.h"
// xTaskCreate, vTaskDelay, xTaskGetTickCount. [FreeRTOS, freertos/task.h]

#include "freertos/semphr.h"
// Мютекс. [FreeRTOS, freertos/semphr.h]

#include <inttypes.h>
// PRIx64 для печати адреса. [стандарт C, inttypes.h]

#include <stdbool.h>
// bool, true, false. [стандарт C, stdbool.h]

#include <math.h>
// fabsf для сравнения float. [стандарт C, math.h]

static const char *TAG = "DS18B20";
// Тег логов. [наш ds18b20_sensor.c]

#define TASK_NAME "ds18b20"
// Имя задачи опроса. [FreeRTOS, freertos/task.h]

#define TASK_STACK 4096
// Стек задачи опроса, байт. [FreeRTOS, freertos/task.h]

#define TASK_PRIO 5
// Приоритет задачи опроса. [FreeRTOS, freertos/task.h]

#define TASK_SLICE_MS 50
// Шаг нарезки паузы между опросами: между шагами проверяем запрос остановки. [наш ds18b20_sensor.c]

#define TASK_STOP_TIMEOUT_MS 5000
// Максимум ожидания самостоятельного выхода задачи при deinit, мс.
// Больше периода преобразования (800 мс) с большим запасом. [наш ds18b20_sensor.c]

#define TASK_STOP_POLL_MS 10
// Период опроса флага остановки при deinit, мс. [наш ds18b20_sensor.c]

#define ONEWIRE_MAX_RX_BYTES 10
// Буфер приёма RMT: 9 байт scratchpad + запас. [espressif/onewire_bus, onewire_bus.h]

#define DISCONNECT_TEMP (-196.60f)
// Значение, читаемое при обрыве линии. [наш ds18b20_sensor.c]

#define DISCONNECT_TOL 0.5f
// Допуск сравнения с DISCONNECT_TEMP. [наш ds18b20_sensor.c]

static onewire_bus_handle_t s_bus = NULL;
// Дескриптор шины 1-Wire. [espressif/onewire_bus, onewire_bus.h]

static ds18b20_device_handle_t s_sensors[DS18B20_MAX_SENSORS] = {0};
// Дескрипторы найденных датчиков. [espressif/ds18b20, ds18b20.h]

static int s_count = 0;
// Текущее количество найденных датчиков. [наш ds18b20_sensor.c]

static ds18b20_reading_t s_readings[DS18B20_MAX_SENSORS];
// Последние показания (кэш для main). [наш ds18b20_sensor.h]

static ds18b20_status_t s_overall_status = DS18B20_STATUS_ERROR;
// «Худший» статус после последнего чтения. [наш ds18b20_sensor.h]

static SemaphoreHandle_t s_lock = NULL;
// Защита s_readings и s_overall_status от гонок. [FreeRTOS, freertos/semphr.h]

static SemaphoreHandle_t s_bus_lock = NULL;
// Сериализация обращений к шине: фоновая задача и вызов read_all() из main
// не должны одновременно выполнять convert/scratchpad-последовательность. [FreeRTOS, freertos/semphr.h]

static int s_period_ms = DS18B20_PERIOD_MS;
// Период фонового опроса, мс. [наш ds18b20_sensor.c]

static TaskHandle_t s_task = NULL;
// Задача фонового опроса: NULL — не запущена. [FreeRTOS, freertos/task.h]

static volatile bool s_stop_req = false;
// Запрос остановки задачи опроса: true — задача должна завершиться сама. [наш ds18b20_sensor.c]

static ds18b20_on_readings_t s_callback = NULL;
// Колбэк нового цикла чтения; NULL — не задан. [наш ds18b20_sensor.h]

static int s_published = -1;
// Последнее опубликованное в indicator состояние; -1 — ещё ничего не публиковалось.
// Нужно, чтобы публикация и лог шли только при смене состояния, но первая
// публикация (даже «ошибка») состоялась всегда. [наш ds18b20_sensor.c]

// Внутренние функции.
static void s_publish(ds18b20_status_t worst);
// Публикация состояния в indicator. [наш indicator.h]

static void s_destroy(void);
// Освобождение шины и датчиков. [espressif/onewire_bus, onewire_bus.h]

static void s_task_body(void *pvParameters);
// Тело задачи фонового опроса. [FreeRTOS, freertos/task.h]

// ============================================================================
//  s_publish()
//
//  КРАТКО: Передаёт состояние в агрегатор indicator и пишет в лог только
//          смену состояния: и публикация, и лог выполняются один раз
//          на изменение, а не каждую секунду.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: worst — «худший» статус среди датчиков. [наш ds18b20_sensor.h]
// ============================================================================
static void s_publish(ds18b20_status_t worst)
{
    if ((int)worst == s_published)
    {
        // Состояние не изменилось — ни в indicator, ни в лог ничего не шлём. [наш ds18b20_sensor.c]
        return;
    }

    s_published = (int)worst;

    if (worst == DS18B20_STATUS_ERROR)
    {
        // Ошибка связи или данных нет.
        indicator_report_error(INDICATOR_SRC_DS18B20);
        indicator_set_warning(INDICATOR_SRC_DS18B20, false);
        ESP_LOGE(TAG, "Состояние: ошибка связи или нет данных");
    }
    else if (worst == DS18B20_STATUS_WARNING)
    {
        // Связь есть, но температура вне диапазона.
        indicator_clear_error(INDICATOR_SRC_DS18B20);
        indicator_set_warning(INDICATOR_SRC_DS18B20, true);
        ESP_LOGW(TAG, "Состояние: температура вне диапазона");
    }
    else
    {
        // Всё хорошо.
        indicator_clear_error(INDICATOR_SRC_DS18B20);
        indicator_set_warning(INDICATOR_SRC_DS18B20, false);
        ESP_LOGI(TAG, "Состояние: норма");
    }
}

// ============================================================================
//  s_destroy()
//
//  КРАТКО: Удаляет дескрипторы датчиков и саму шину.
//
//  Возвращает void — ничего. [стандарт C]
// ============================================================================
static void s_destroy(void)
{
    for (int i = 0; i < s_count; i++)
    {
        ds18b20_del_device(s_sensors[i]);
        // Удаляем дескриптор датчика. [espressif/ds18b20, ds18b20.h]

        s_sensors[i] = NULL;
    }

    s_count = 0;

    if (s_bus != NULL)
    {
        onewire_bus_del(s_bus);
        // Освобождаем шину. [espressif/onewire_bus, onewire_bus.h]

        s_bus = NULL;
    }
}

// ============================================================================
//  s_task_body()
//
//  КРАТКО: Задача FreeRTOS — опрашивает датчики с заданным периодом.
//          Период отсчитывается от начала цикла, поэтому фактический интервал
//          равен s_period_ms, а не «период + время преобразования».
//          Пауза нарезается на шаги TASK_SLICE_MS, чтобы задача быстро
//          заметила запрос остановки. Выход кооперативный: по s_stop_req
//          задача сама удаляет себя через vTaskDelete(NULL).
//
//  Возвращает void — удаляет саму себя при остановке. [стандарт C]
//  Параметры: pvParameters — не используется. [FreeRTOS, freertos/task.h]
// ============================================================================
static void s_task_body(void *pvParameters)
{
    (void)pvParameters;
    // Параметр не используется. [стандарт C]

    while (!s_stop_req)
    {
        TickType_t start = xTaskGetTickCount();
        // Начало цикла. [FreeRTOS, freertos/task.h]

        ds18b20_sensor_read_all(NULL);
        // Опрос; результат сохраняется во внутренний буфер. [наш ds18b20_sensor.h]

        TickType_t elapsed = xTaskGetTickCount() - start;
        // Сколько длился опрос. [FreeRTOS, freertos/task.h]

        TickType_t period = pdMS_TO_TICKS(s_period_ms);
        // Заданный период в тиках. [FreeRTOS, freertos/task.h]

        TickType_t remaining = (elapsed < period) ? (period - elapsed) : 0;
        // Сколько осталось доспать; если опрос длился дольше периода — не ждём. [наш ds18b20_sensor.c]

        while (remaining > 0 && !s_stop_req)
        {
            // Нарезанная пауза: между шагами проверяем запрос остановки. [наш ds18b20_sensor.c]

            TickType_t slice = pdMS_TO_TICKS(TASK_SLICE_MS);
            vTaskDelay(slice < remaining ? slice : remaining);
            remaining -= (slice < remaining ? slice : remaining);
        }
    }

    s_task = NULL;
    // Помечаем задачу остановленной — deinit продолжит освобождение ресурсов. [наш ds18b20_sensor.c]

    vTaskDelete(NULL);
    // Удаляем сами себя; выполнение дальше не идёт. [FreeRTOS, freertos/task.h]
}

// ============================================================================
//  ds18b20_sensor_init()
//
//  КРАТКО: Создаёт шину 1-Wire на GPIO, находит все DS18B20 и задаёт им
//          разрешение 12 бит. Повторный вызов при уже найденных датчиках
//          ничего не делает; после неудачи освобождает ресурсы и пробует снова.
//
//  Возвращает esp_err_t: ESP_OK, ESP_ERR_NOT_FOUND или ESP_ERR_*.
//  Параметры: gpio_num — GPIO линии DQ. [стандарт C, int]
// ============================================================================
esp_err_t ds18b20_sensor_init(int gpio_num)
{
    if (s_bus != NULL && s_count > 0)
    {
        // Датчики уже инициализированы — повторно не ищем.
        ESP_LOGW(TAG, "Датчики уже инициализированы — повторный вызов пропущен");
        return ESP_OK;
    }

    if (s_bus != NULL)
    {
        // Прошлая попытка не нашла датчики — освобождаем ресурсы.
        s_destroy();
    }

    s_published = -1;
    // Сброс кэша публикации: после (ре)инициализации первый s_publish()
    // обязан пройти, даже если состояние совпадает с прежним —
    // indicator мог быть переинициализирован и маски сброшены. [наш ds18b20_sensor.c]

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
        // Мютекс для доступа к показаниям. [FreeRTOS, freertos/semphr.h]

        if (s_lock == NULL)
        {
            ESP_LOGE(TAG, "Не удалось создать мютекс");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_bus_lock == NULL)
    {
        s_bus_lock = xSemaphoreCreateMutex();
        // Мютекс сериализации обращений к шине (convert + чтение). [FreeRTOS, freertos/semphr.h]

        if (s_bus_lock == NULL)
        {
            ESP_LOGE(TAG, "Не удалось создать мютекс шины");
            return ESP_ERR_NO_MEM;
        }
    }

    for (int i = 0; i < DS18B20_MAX_SENSORS; i++)
    {
        // Явно: статус 0 — это OK, memset оставил бы «норму» без данных.
        // До первого успешного чтения данных нет. [наш ds18b20_sensor.c]
        s_readings[i].temperature = 0.0f;
        s_readings[i].status = DS18B20_STATUS_ERROR;
    }

    s_overall_status = DS18B20_STATUS_ERROR;
    // Данных ещё нет — состояние не подтверждено. [наш ds18b20_sensor.c]

    onewire_bus_config_t bus_config = {
        .bus_gpio_num = gpio_num,
        // GPIO линии DQ. [espressif/onewire_bus, onewire_bus.h]
    };
    // Конфигурация шины 1-Wire. [espressif/onewire_bus, onewire_bus.h]

    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = ONEWIRE_MAX_RX_BYTES,
        // Максимальный размер ответа, байт. [espressif/onewire_bus, onewire_bus.h]
    };
    // Конфигурация RMT-бэкенда. [espressif/onewire_bus, onewire_bus.h]

    esp_err_t err = onewire_new_bus_rmt(&bus_config, &rmt_config, &s_bus);
    // Создаём шину 1-Wire поверх RMT. [espressif/onewire_bus, onewire_bus.h]

    if (err != ESP_OK)
    {
        s_bus = NULL;
        ESP_LOGE(TAG, "Не удалось создать шину 1-Wire на GPIO%d (0x%X)", gpio_num, (unsigned)err);
        return err;
    }

    ESP_LOGI(TAG, "Шина 1-Wire создана на GPIO%d (RMT)", gpio_num);

    err = onewire_bus_reset(s_bus);
    // Проверяем, есть ли устройства на шине. [espressif/onewire_bus, onewire_bus.h]

    if (err == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Шина не отвечает: проверьте подтяжку 4.7 кОм к 3.3 В на GPIO%d", gpio_num);
    }
    else if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Reset шины завершился ошибкой 0x%X", (unsigned)err);
    }

    onewire_device_iter_handle_t iter = NULL;
    // Итератор для перебора устройств. [espressif/onewire_bus, onewire_bus.h]

    err = onewire_new_device_iter(s_bus, &iter);
    // Создаём итератор поиска. [espressif/onewire_bus, onewire_bus.h]

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Не удалось создать итератор устройств (0x%X)", (unsigned)err);
        s_destroy();
        // Шину не оставляем занятой. [espressif/onewire_bus, onewire_bus.h]

        return err;
    }

    ds18b20_config_t ds_cfg = {};
    // Конфигурация DS18B20 по умолчанию. [espressif/ds18b20, ds18b20.h]

    onewire_device_t device;
    // Очередное устройство на шине. [espressif/onewire_bus, onewire_bus.h]

    esp_err_t search = ESP_OK;
    // Результат перебора устройств. [ESP-IDF, esp_err.h]

    do
    {
        search = onewire_device_iter_get_next(iter, &device);
        // Берём следующее устройство. [espressif/onewire_bus, onewire_bus.h]

        if (search != ESP_OK)
        {
            break;
            // Устройства закончились. [espressif/onewire_bus, onewire_bus.h]
        }

        if (s_count >= DS18B20_MAX_SENSORS)
        {
            ESP_LOGW(TAG, "Достигнут предел DS18B20_MAX_SENSORS (%d)", DS18B20_MAX_SENSORS);
            break;
        }

        if (ds18b20_new_device_from_enumeration(&device, &ds_cfg, &s_sensors[s_count]) == ESP_OK)
        {
            onewire_device_address_t address = 0;
            ds18b20_get_device_address(s_sensors[s_count], &address);
            // Адрес уже хранится в дескрипторе, шина не опрашивается. [espressif/ds18b20, ds18b20.h]

            ESP_LOGI(TAG, "Найден DS18B20[%d], адрес: 0x%016" PRIx64, s_count, (uint64_t)address);
            s_count++;
        }
        else
        {
            ESP_LOGW(TAG, "Устройство 0x%016" PRIx64 " не DS18B20 — пропущено", (uint64_t)device.address);
        }
    } while (search == ESP_OK);

    onewire_del_device_iter(iter);
    // Освобождаем итератор. [espressif/onewire_bus, onewire_bus.h]

    if (s_count == 0)
    {
        ESP_LOGE(TAG, "Датчики DS18B20 не найдены");
        s_destroy();
        s_publish(DS18B20_STATUS_ERROR);
        // Сообщаем индикатору об ошибке. [наш indicator.h]

        return ESP_ERR_NOT_FOUND;
    }

    for (int i = 0; i < s_count; i++)
    {
        if (ds18b20_set_resolution(s_sensors[i], DS18B20_RESOLUTION_12B) != ESP_OK)
        {
            ESP_LOGE(TAG, "DS18B20[%d]: не удалось установить разрешение 12 бит", i);
        }
        // 12 бит: преобразование занимает ≈800 мс. [espressif/ds18b20, ds18b20.h]

        s_readings[i].temperature = 0.0f;
        s_readings[i].status = DS18B20_STATUS_ERROR;
        // До первого успешного чтения данных нет. [наш ds18b20_sensor.c]
    }

    ESP_LOGI(TAG, "Инициализация завершена, датчиков: %d", s_count);

    s_publish(DS18B20_STATUS_ERROR);
    // До первого чтения данных нет — индикатор красный до первого
    // успешного цикла (см. s_published = -1 в начале init). [ТЗ.md, раздел 8]

    return ESP_OK;
}

// ============================================================================
//  ds18b20_sensor_deinit()
//
//  КРАТКО: Останавливает фоновый опрос и освобождает шину, датчики и мютекс.
//          После этого библиотеку можно инициализировать заново.
//
//  Возвращает esp_err_t: ESP_OK. [ESP-IDF, esp_err.h]
// ============================================================================
esp_err_t ds18b20_sensor_deinit(void)
{
    if (s_task != NULL)
    {
        s_stop_req = true;
        // Просим задачу завершиться: она выходит из цикла сама
        // и удаляет себя через vTaskDelete(NULL). [наш ds18b20_sensor.c]

        for (int i = 0; i < TASK_STOP_TIMEOUT_MS / TASK_STOP_POLL_MS && s_task != NULL; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(TASK_STOP_POLL_MS));
            // Ждём, пока задача выйдет из read_all/паузы и обнулит s_task. [наш ds18b20_sensor.c]
        }

        if (s_task != NULL)
        {
            // Страховка: задача не заметила запрос (например, драйвер завис
            // в передаче). Удаляем принудительно — иначе deinit зависнет. [FreeRTOS, freertos/task.h]
            ESP_LOGE(TAG, "Задача опроса не остановилась за %d мс — удаляю принудительно",
                     TASK_STOP_TIMEOUT_MS);

            vTaskDelete(s_task);
            s_task = NULL;
        }

        s_stop_req = false;
        // Готовим флаг к возможному повторному start(). [наш ds18b20_sensor.c]
    }

    s_destroy();
    // Освобождаем шину и датчики. [espressif/onewire_bus, onewire_bus.h]

    if (s_lock != NULL)
    {
        vSemaphoreDelete(s_lock);
        // Удаляем мютекс показаний. [FreeRTOS, freertos/semphr.h]

        s_lock = NULL;
    }

    if (s_bus_lock != NULL)
    {
        vSemaphoreDelete(s_bus_lock);
        // Удаляем мютекс шины. [FreeRTOS, freertos/semphr.h]

        s_bus_lock = NULL;
    }

    for (int i = 0; i < DS18B20_MAX_SENSORS; i++)
    {
        // Явно: статус 0 — это OK, memset оставил бы «норму» без данных. [наш ds18b20_sensor.c]
        s_readings[i].temperature = 0.0f;
        s_readings[i].status = DS18B20_STATUS_ERROR;
    }

    s_overall_status = DS18B20_STATUS_ERROR;
    s_published = -1;
    // Сбрасываем кэш публикации — после повторного init() первая публикация
    // пройдёт заново (indicator мог быть переинициализирован). [наш ds18b20_sensor.c]

    s_publish(DS18B20_STATUS_ERROR);
    // Данных больше нет — индикатор покажет ошибку. [наш indicator.h]

    ESP_LOGI(TAG, "Библиотека остановлена, ресурсы освобождены");
    return ESP_OK;
}

// ============================================================================
//  ds18b20_sensor_set_period_ms()
//
//  КРАТКО: Задаёт период фонового опроса. Значение меньше
//          DS18B20_PERIOD_MIN_MS заменяется на DS18B20_PERIOD_MS,
//          так как одно преобразование 12 бит занимает ≈800 мс.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: period_ms — период в миллисекундах. [стандарт C, int]
// ============================================================================
void ds18b20_sensor_set_period_ms(int period_ms)
{
    if (period_ms < DS18B20_PERIOD_MIN_MS)
    {
        s_period_ms = DS18B20_PERIOD_MS;
        ESP_LOGW(TAG, "Период %d мс слишком мал (преобразование ≈800 мс), берём %d мс",
                 period_ms, s_period_ms);
        return;
    }

    s_period_ms = period_ms;
    ESP_LOGI(TAG, "Период опроса: %d мс", s_period_ms);
}

// ============================================================================
//  ds18b20_sensor_start()
//
//  КРАТКО: Запускает фоновую задачу опроса с периодом s_period_ms.
//
//  Возвращает esp_err_t: ESP_OK, ESP_ERR_INVALID_STATE (нет датчиков),
//                        ESP_FAIL (не удалось создать задачу).
// ============================================================================
esp_err_t ds18b20_sensor_start(void)
{
    if (s_count == 0)
    {
        ESP_LOGE(TAG, "Нет датчиков — сначала вызовите ds18b20_sensor_init()");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task != NULL)
    {
        return ESP_OK;
        // Опрос уже идёт. [наш ds18b20_sensor.c]
    }

    s_stop_req = false;
    // Сбрасываем запрос остановки — задача должна запуститься вновь. [наш ds18b20_sensor.c]

    if (xTaskCreate(s_task_body, TASK_NAME, TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS)
    {
        s_task = NULL;
        ESP_LOGE(TAG, "Не удалось создать задачу опроса");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Фоновый опрос запущен, период %d мс", s_period_ms);
    return ESP_OK;
}

// ============================================================================
//  ds18b20_sensor_count()
//
//  КРАТКО: Сколько датчиков найдено при инициализации.
//
//  Возвращает int — число датчиков. [стандарт C]
// ============================================================================
int ds18b20_sensor_count(void)
{
    return s_count;
}

// ============================================================================
//  ds18b20_sensor_read_all()
//
//  КРАТКО: Одно общее преобразование сразу для всех датчиков (Skip ROM +
//          Convert T), затем чтение scratchpad каждого. Драйвер сам
//          выдерживает паузу ≈800 мс внутри вызова, дополнительная
//          задержка не нужна. Вся последовательность «convert → read»
//          защищена мьютексом s_bus_lock: параллельный вызов из другой
//          задачи дождётся окончания, а не перемешает тайминги на шине.
//          После обновления кэша вызывается заданный колбэк (вне локов).
//
//  Возвращает esp_err_t: ESP_OK (прочитан хотя бы один датчик — в том числе
//          со статусом WARNING), ESP_FAIL (ни одного), ESP_ERR_INVALID_STATE
//          (нет инициализации).
//  Параметры: readings — массив результатов или NULL. [наш ds18b20_sensor.h]
// ============================================================================
esp_err_t ds18b20_sensor_read_all(ds18b20_reading_t *readings)
{
    if (s_count == 0 || s_bus == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_bus_lock != NULL)
    {
        xSemaphoreTake(s_bus_lock, portMAX_DELAY);
        // Сериализуем «convert + чтение scratchpad» между задачами. [FreeRTOS, freertos/semphr.h]
    }

    esp_err_t trig_err = ds18b20_trigger_temperature_conversion_for_all(s_bus);
    // Преобразование сразу для всех датчиков на шине. [espressif/ds18b20, ds18b20.h]

    if (trig_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Не удалось запустить преобразование (0x%X)", (unsigned)trig_err);
    }

    ds18b20_reading_t result[DS18B20_MAX_SENSORS];
    // Результаты текущего цикла. [наш ds18b20_sensor.h]

    ds18b20_status_t worst = DS18B20_STATUS_OK;
    // «Худший» статус среди датчиков. [наш ds18b20_sensor.h]

    bool any_read = false;
    // Прочитан ли хотя бы один датчик (ошибка чтения — не «непрочитанное»). [стандарт C, stdbool.h]

    for (int i = 0; i < s_count; i++)
    {
        float temperature = 0.0f;
        // Результат чтения. [стандарт C]

        esp_err_t err = trig_err;
        // Если преобразование не запустилось — читать нечего. [ESP-IDF, esp_err.h]

        if (err == ESP_OK)
        {
            err = ds18b20_get_temperature(s_sensors[i], &temperature);
            // Чтение scratchpad, внутри проверяется CRC. [espressif/ds18b20, ds18b20.h]
        }

        if (err != ESP_OK || fabsf(temperature - DISCONNECT_TEMP) < DISCONNECT_TOL)
        {
            // Ошибка шины, CRC или обрыв линии. [espressif/ds18b20, ds18b20.h]

            result[i].temperature = 0.0f;
            result[i].status = DS18B20_STATUS_ERROR;
            worst = DS18B20_STATUS_ERROR;

            ESP_LOGD(TAG, "DS18B20[%d]: ошибка чтения (0x%X)", i, (unsigned)err);
            continue;
        }

        result[i].temperature = temperature;
        any_read = true;
        // Датчик ответил и значение прочитано — это «прочитан», даже если
        // температура вне диапазона (ТЗ F-18: WARNING → ESP_OK). [ТЗ.md, F-18]

        if (temperature < DS18B20_TEMP_MIN_VALID || temperature > DS18B20_TEMP_MAX_VALID)
        {
            result[i].status = DS18B20_STATUS_WARNING;

            if (worst != DS18B20_STATUS_ERROR)
            {
                worst = DS18B20_STATUS_WARNING;
            }

            ESP_LOGW(TAG, "DS18B20[%d]: %.2f °C — вне диапазона", i, temperature);
        }
        else
        {
            result[i].status = DS18B20_STATUS_OK;

            ESP_LOGD(TAG, "DS18B20[%d]: %.2f °C", i, temperature);
        }
    }

    if (s_bus_lock != NULL)
    {
        xSemaphoreGive(s_bus_lock);
        // Шина свободна — дальше только кэш и колбэк. [FreeRTOS, freertos/semphr.h]
    }

    if (s_lock != NULL)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        // Защищаем кэш показаний. [FreeRTOS, freertos/semphr.h]
    }

    for (int i = 0; i < s_count; i++)
    {
        s_readings[i] = result[i];

        if (readings != NULL)
        {
            readings[i] = result[i];
            // Отдаём данные вызывающему, если буфер передан. [наш ds18b20_sensor.h]
        }
    }

    s_overall_status = worst;
    // Запоминаем общий статус. [наш ds18b20_sensor.h]

    if (s_lock != NULL)
    {
        xSemaphoreGive(s_lock);
    }

    s_publish(worst);
    // Публикуем состояние в indicator. [наш indicator.h]

    if (s_callback != NULL)
    {
        ds18b20_info_t infos[DS18B20_MAX_SENSORS];
        // Снимки для колбэка: адрес + показания. [наш ds18b20_sensor.h]

        for (int i = 0; i < s_count; i++)
        {
            onewire_device_address_t address = 0;
            ds18b20_get_device_address(s_sensors[i], &address);
            // Адрес в дескрипторе, обращений к шине нет. [espressif/ds18b20, ds18b20.h]

            infos[i].address = (uint64_t)address;
            infos[i].temperature = result[i].temperature;
            infos[i].status = result[i].status;
        }

        ds18b20_on_readings_t cb = s_callback;
        // Локальная копия: колбэк может быть заменён из другой задачи. [наш ds18b20_sensor.h]

        cb(infos, s_count);
        // Вызов вне s_lock/s_bus_lock — внутри колбэка можно звать get_*. [наш ds18b20_sensor.h]
    }

    return any_read ? ESP_OK : ESP_FAIL;
}

// ============================================================================
//  ds18b20_sensor_overall_status()
//
//  КРАТКО: «Худший» статус после последнего чтения.
//
//  Возвращает ds18b20_status_t. [наш ds18b20_sensor.h]
// ============================================================================
ds18b20_status_t ds18b20_sensor_overall_status(void)
{
    ds18b20_status_t status = DS18B20_STATUS_ERROR;
    // По умолчанию — ошибка. [наш ds18b20_sensor.h]

    if (s_lock != NULL)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        status = s_overall_status;
        xSemaphoreGive(s_lock);
    }

    return status;
}

// ============================================================================
//  ds18b20_sensor_get_temperature()
//
//  КРАТКО: Последняя температура датчика из кэша — шина не опрашивается.
//
//  Возвращает float: температура в °C, 0.0f при неверном индексе. [стандарт C]
//  Параметры: index — индекс датчика 0..count-1. [стандарт C, int]
// ============================================================================
float ds18b20_sensor_get_temperature(int index)
{
    float temperature = 0.0f;
    // Значение по умолчанию. [стандарт C]

    if (index < 0 || index >= s_count || s_lock == NULL)
    {
        return temperature;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    temperature = s_readings[index].temperature;
    xSemaphoreGive(s_lock);

    return temperature;
}

// ============================================================================
//  ds18b20_sensor_get_status()
//
//  КРАТКО: Статус датчика после последнего чтения.
//
//  Возвращает ds18b20_status_t: ERROR при неверном индексе. [наш ds18b20_sensor.h]
//  Параметры: index — индекс датчика 0..count-1. [стандарт C, int]
// ============================================================================
ds18b20_status_t ds18b20_sensor_get_status(int index)
{
    ds18b20_status_t status = DS18B20_STATUS_ERROR;
    // По умолчанию — ошибка. [наш ds18b20_sensor.h]

    if (index < 0 || index >= s_count || s_lock == NULL)
    {
        return status;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    status = s_readings[index].status;
    xSemaphoreGive(s_lock);

    return status;
}

// ============================================================================
//  ds18b20_sensor_get_address()
//
//  КРАТКО: 64-битный ROM-адрес датчика из дескриптора, без опроса шины.
//
//  Возвращает esp_err_t: ESP_OK или ESP_ERR_INVALID_ARG. [ESP-IDF, esp_err.h]
//  Параметры: index — индекс датчика. [стандарт C, int]
//             address — куда записать адрес. [стандарт C, uint64_t *]
// ============================================================================
esp_err_t ds18b20_sensor_get_address(int index, uint64_t *address)
{
    if (address == NULL || index < 0 || index >= s_count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    onewire_device_address_t addr = 0;
    // Адрес датчика. [espressif/onewire_bus, onewire_types.h]

    esp_err_t err = ds18b20_get_device_address(s_sensors[index], &addr);
    // Адрес уже хранится в дескрипторе. [espressif/ds18b20, ds18b20.h]

    if (err == ESP_OK)
    {
        *address = (uint64_t)addr;
    }

    return err;
}

// ============================================================================
//  ds18b20_status_name()
//
//  КРАТКО: Текстовое имя статуса для вывода в консоль/лог.
//
//  Возвращает const char*: "OK" / "WARN" / "ERROR". [стандарт C]
//  Параметры: status — статус датчика. [наш ds18b20_sensor.h]
// ============================================================================
const char *ds18b20_status_name(ds18b20_status_t status)
{
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

// ============================================================================
//  ds18b20_sensor_set_callback()
//
//  КРАТКО: Задаёт колбэк нового цикла чтения. Вызывать до start()/run().
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: callback — функция колбэка или NULL для отмены. [наш ds18b20_sensor.h]
// ============================================================================
void ds18b20_sensor_set_callback(ds18b20_on_readings_t callback)
{
    s_callback = callback;
    // Присваивание указателя атомарно на 32-битной архитектуре; надёжнее
    // задавать колбэк до запуска задачи, как и указано в заголовке. [наш ds18b20_sensor.h]
}

// ============================================================================
//  ds18b20_sensor_get_info()
//
//  КРАТКО: Адрес + температура + статус одним захватом мьютекса —
//          согласованный снимок (в отличие от трёх отдельных геттеров,
//          между которыми может пройти обновление кэша).
//
//  Возвращает esp_err_t: ESP_OK или ESP_ERR_INVALID_ARG. [ESP-IDF, esp_err.h]
//  Параметры: index — индекс датчика. [стандарт C, int]
//             info — куда записать снимок. [наш ds18b20_sensor.h]
// ============================================================================
esp_err_t ds18b20_sensor_get_info(int index, ds18b20_info_t *info)
{
    if (info == NULL || index < 0 || index >= s_count || s_lock == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    onewire_device_address_t address = 0;
    ds18b20_get_device_address(s_sensors[index], &address);
    // Адрес в дескрипторе и не меняется после init(); читаем без лока. [espressif/ds18b20, ds18b20.h]

    xSemaphoreTake(s_lock, portMAX_DELAY);
    info->temperature = s_readings[index].temperature;
    info->status = s_readings[index].status;
    xSemaphoreGive(s_lock);

    info->address = (uint64_t)address;
    return ESP_OK;
}

// ============================================================================
//  ds18b20_sensor_run()
//
//  КРАТКО: Единая точка входа для main: поднимает индикацию, инициализирует
//          датчики и запускает фоновый опрос. Возвращается сразу.
//          Сбой индикации не фатален (работаем без светодиода), его код
//          в возврат не попадает. Если датчиков нет, возвращается
//          ESP_ERR_NOT_FOUND, а индикатор показывает ошибку.
//
//  Возвращает esp_err_t: ESP_OK; ESP_ERR_NOT_FOUND (датчиков нет);
//          иные ESP_ERR_* от init()/start(). [ESP-IDF, esp_err.h]
//  Параметры: onewire_gpio — GPIO линии DQ. [стандарт C, int]
//             rgb_led_gpio — GPIO встроенного WS2812. [стандарт C, int]
// ============================================================================
esp_err_t ds18b20_sensor_run(int onewire_gpio, int rgb_led_gpio)
{
    if (indicator_init(rgb_led_gpio) != ESP_OK)
    {
        // Индикация не критична — продолжаем без светодиода.
        ESP_LOGE(TAG, "Индикация недоступна (GPIO%d) — работаем без неё", rgb_led_gpio);
    }

    esp_err_t err = ds18b20_sensor_init(onewire_gpio);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Не удалось инициализировать DS18B20 на GPIO%d", onewire_gpio);
        return err;
        // Причина уже опубликована в indicator внутри init(). [наш ds18b20_sensor.h]
    }

    return ds18b20_sensor_start();
    // Датчики найдены — включаем периодический опрос. [наш ds18b20_sensor.h]
}