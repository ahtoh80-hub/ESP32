// ============================================================================
//  components/dht11_sensor/dht11_sensor.c
//
//  Самодостаточная библиотека DHT11 (температура + влажность).
//  Период фонового опроса — 1000 мс по умолчанию, задаётся через
//  dht11_sensor_set_period_ms(). Состояние публикуется в indicator.
//  Протокол датчика реализован драйвером esp-idf-lib/dht (dht.h).
// ============================================================================

#include "dht11_sensor.h"
// Собственный публичный заголовок. [наш dht11_sensor.h]

#include "indicator.h"
// Агрегатор индикации — сюда публикуем состояние. [наш indicator.h]

#include "esp_log.h"
// ESP_LOGI/LOGW/LOGE/LOGD. [ESP-IDF, esp_log.h]

#include "dht.h"
// Драйвер DHT11: dht_read_float_data(), DHT_TYPE_DHT11. [esp-idf-lib/dht, dht.h]

#include "freertos/FreeRTOS.h"
// Базовые типы FreeRTOS. [FreeRTOS, freertos/FreeRTOS.h]

#include "freertos/task.h"
// xTaskCreate, vTaskDelay, xTaskGetTickCount. [FreeRTOS, freertos/task.h]

#include "freertos/semphr.h"
// Мютексы. [FreeRTOS, freertos/semphr.h]

static const char *TAG = "DHT11";
// Тег логов. [наш dht11_sensor.c]

#define TASK_NAME "dht11"
// Имя задачи опроса. [FreeRTOS, freertos/task.h]

#define TASK_STACK 4096
// Стек задачи опроса, байт: хватает и на драйвер, и на printf с float
// в пользовательском колбэке. [FreeRTOS, freertos/task.h]

#define TASK_PRIO 5
// Приоритет задачи опроса — как у ds18b20. [FreeRTOS, freertos/task.h]

#define TASK_SLICE_MS 50
// Шаг нарезки пауз: между шагами проверяем запрос остановки. [наш dht11_sensor.c]

#define TASK_STOP_TIMEOUT_MS 5000
// Максимум ожидания самостоятельного выхода задачи при deinit, мс. [наш dht11_sensor.c]

#define TASK_STOP_POLL_MS 10
// Период опроса флага остановки при deinit, мс. [наш dht11_sensor.c]

static int s_gpio = -1;
// GPIO датчика; -1 — не инициализировано. [наш dht11_sensor.c]

static bool s_ready = false;
// Флаг инициализации: библиотека готова к опросу. [наш dht11_sensor.c]

static dht11_reading_t s_reading = {0.0f, 0.0f, DHT11_STATUS_ERROR};
// Последние показания (кэш для main). [наш dht11_sensor.h]

static SemaphoreHandle_t s_lock = NULL;
// Защита s_reading от гонок. [FreeRTOS, freertos/semphr.h]

static SemaphoreHandle_t s_read_lock = NULL;
// Сериализация обращений к датчику: фоновая задача и ручной read()
// не должны одновременно передавать кадры по одному проводу. [FreeRTOS, freertos/semphr.h]

static int s_period_ms = DHT11_PERIOD_MS;
// Период фонового опроса, мс. [наш dht11_sensor.c]

static TaskHandle_t s_task = NULL;
// Задача фонового опроса: NULL — не запущена. [FreeRTOS, freertos/task.h]

static volatile bool s_stop_req = false;
// Запрос остановки задачи опроса: true — задача должна завершиться сама. [наш dht11_sensor.c]

static dht11_on_readings_t s_callback = NULL;
// Колбэк нового цикла чтения; NULL — не задан. [наш dht11_sensor.h]

static int s_published = -1;
// Последнее опубликованное в indicator состояние; -1 — ещё ничего не публиковалось. [наш dht11_sensor.c]

// Внутренние функции.
static void s_publish(dht11_status_t status);
// Публикация состояния в indicator. [наш indicator.h]

static bool s_delay(TickType_t ticks);
// Нарезанная задержка с проверкой запроса остановки. [FreeRTOS, freertos/task.h]

static void s_task_body(void *pvParameters);
// Тело задачи фонового опроса. [FreeRTOS, freertos/task.h]

// ============================================================================
//  s_publish()
//
//  КРАТКО: Передаёт состояние в indicator и пишет в лог только смену
//          состояния — не каждую секунду.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: status — статус текущего цикла. [наш dht11_sensor.h]
// ============================================================================
static void s_publish(dht11_status_t status)
{
    if ((int)status == s_published)
    {
        // Состояние не изменилось — ни в indicator, ни в лог ничего не шлём. [наш dht11_sensor.c]
        return;
    }

    s_published = (int)status;

    if (status == DHT11_STATUS_ERROR)
    {
        // Датчик не отвечает или данных ещё нет.
        indicator_report_error(INDICATOR_SRC_DHT11);
        indicator_set_warning(INDICATOR_SRC_DHT11, false);
        ESP_LOGE(TAG, "Состояние: датчик не отвечает или нет данных");
    }
    else if (status == DHT11_STATUS_WARNING)
    {
        // Связь есть, но значение вне паспортного диапазона.
        indicator_clear_error(INDICATOR_SRC_DHT11);
        indicator_set_warning(INDICATOR_SRC_DHT11, true);
        ESP_LOGW(TAG, "Состояние: значение вне паспортного диапазона");
    }
    else
    {
        // Всё хорошо.
        indicator_clear_error(INDICATOR_SRC_DHT11);
        indicator_set_warning(INDICATOR_SRC_DHT11, false);
        ESP_LOGI(TAG, "Состояние: норма");
    }
}

// ============================================================================
//  s_delay()
//
//  КРАТКО: Спит указанное время тиками, нарезая на TASK_SLICE_MS —
//          задача быстро замечает запрос остановки при deinit.
//
//  Возвращает bool: false — запрошена остановка, ожидание прервано.
//  Параметры: ticks — сколько спать. [FreeRTOS, freertos/task.h]
// ============================================================================
static bool s_delay(TickType_t ticks)
{
    while (ticks > 0 && !s_stop_req)
    {
        TickType_t slice = pdMS_TO_TICKS(TASK_SLICE_MS);
        vTaskDelay(slice < ticks ? slice : ticks);
        ticks -= (slice < ticks ? slice : ticks);
    }

    return !s_stop_req;
}

// ============================================================================
//  s_task_body()
//
//  КРАТКО: Задача FreeRTOS — опрашивает датчик с заданным периодом.
//          Первое чтение отложено на один период: после подачи питания
//          DHT11 нужно ≈1 с на стабилизацию. Период отсчитывается от
//          начала цикла. Выход кооперативный: по s_stop_req задача сама
//          удаляет себя через vTaskDelete(NULL).
//
//  Возвращает void — удаляет саму себя при остановке. [стандарт C]
//  Параметры: pvParameters — не используется. [FreeRTOS, freertos/task.h]
// ============================================================================
static void s_task_body(void *pvParameters)
{
    (void)pvParameters;
    // Параметр не используется. [стандарт C]

    if (!s_delay(pdMS_TO_TICKS(s_period_ms)))
    {
        // Остановили сразу после старта — выходим, не читая датчик. [наш dht11_sensor.c]
        s_task = NULL;
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "Стабилизация завершена — начинаю цикл опроса");
    // Одноразовая метка (1 раз за жизнь задачи): подтверждает, что задача
    // проснулась после отложенного первого цикла и вошла в while. [наш dht11_sensor.c]

    while (!s_stop_req)
    {
        TickType_t start = xTaskGetTickCount();
        // Начало цикла. [FreeRTOS, freertos/task.h]

        dht11_sensor_read(NULL);
        // Чтение; результат сохраняется в кэш и уходит в колбэк. [наш dht11_sensor.h]

        TickType_t elapsed = xTaskGetTickCount() - start;
        // Сколько длился опрос. [FreeRTOS, freertos/task.h]

        TickType_t period = pdMS_TO_TICKS(s_period_ms);
        // Заданный период в тиках. [FreeRTOS, freertos/task.h]

        TickType_t remaining = (elapsed < period) ? (period - elapsed) : 0;
        // Сколько осталось доспать; если опрос длился дольше периода — не ждём. [наш dht11_sensor.c]

        s_delay(remaining);
        // Нарезанная пауза: между шагами проверяем запрос остановки. [наш dht11_sensor.c]
    }

    s_task = NULL;
    // Помечаем задачу остановленной — deinit продолжит освобождение ресурсов. [наш dht11_sensor.c]

    vTaskDelete(NULL);
    // Удаляем сами себя; выполнение дальше не идёт. [FreeRTOS, freertos/task.h]
}

// ============================================================================
//  dht11_sensor_init()
//
//  КРАТКО: Запоминает GPIO и создаёт мьютексы. Датчик НЕ опрашивается —
//          после подачи питания ему нужно ≈1 с, поэтому наличие проверит
//          первый цикл фонового опроса.
//
//  Возвращает esp_err_t: ESP_OK или ESP_ERR_NO_MEM. [ESP-IDF, esp_err.h]
//  Параметры: gpio_num — GPIO DATA датчика. [стандарт C, int]
// ============================================================================
esp_err_t dht11_sensor_init(int gpio_num)
{
    if (s_ready)
    {
        // Датчик уже инициализирован — повторно не настраиваем.
        ESP_LOGW(TAG, "Уже инициализирован — повторный вызов пропущен");
        return ESP_OK;
    }

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
        // Мютекс для доступа к кэшу показаний. [FreeRTOS, freertos/semphr.h]

        if (s_lock == NULL)
        {
            ESP_LOGE(TAG, "Не удалось создать мютекс");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_read_lock == NULL)
    {
        s_read_lock = xSemaphoreCreateMutex();
        // Мютекс сериализации обращений к датчику. [FreeRTOS, freertos/semphr.h]

        if (s_read_lock == NULL)
        {
            ESP_LOGE(TAG, "Не удалось создать мютекс чтения");
            return ESP_ERR_NO_MEM;
        }
    }

    s_gpio = gpio_num;
    s_reading.temperature = 0.0f;
    s_reading.humidity = 0.0f;
    s_reading.status = DHT11_STATUS_ERROR;
    // До первого успешного чтения данных нет. [наш dht11_sensor.c]

    s_ready = true;

    s_published = -1;
    s_publish(DHT11_STATUS_ERROR);
    // Индикатор красный до первого успешного цикла. [наш indicator.h]

    ESP_LOGI(TAG, "Инициализация завершена, GPIO%d (наличие датчика проверит первый цикл)",
             gpio_num);
    return ESP_OK;
}

// ============================================================================
//  dht11_sensor_deinit()
//
//  КРАТКО: Останавливает фоновый опрос (кооперативно) и освобождает
//          мьютексы. После этого библиотеку можно инициализировать заново.
//
//  Возвращает esp_err_t: ESP_OK. [ESP-IDF, esp_err.h]
// ============================================================================
esp_err_t dht11_sensor_deinit(void)
{
    if (s_task != NULL)
    {
        s_stop_req = true;
        // Просим задачу завершиться: она выходит из цикла сама
        // и удаляет себя через vTaskDelete(NULL). [наш dht11_sensor.c]

        for (int i = 0; i < TASK_STOP_TIMEOUT_MS / TASK_STOP_POLL_MS && s_task != NULL; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(TASK_STOP_POLL_MS));
            // Ждём, пока задача выйдет из read/паузы и обнулит s_task. [наш dht11_sensor.c]
        }

        if (s_task != NULL)
        {
            // Страховка: задача не заметила запрос (например, зависла
            // в передаче кадра). Удаляем принудительно. [FreeRTOS, freertos/task.h]
            ESP_LOGE(TAG, "Задача опроса не остановилась за %d мс — удаляю принудительно",
                     TASK_STOP_TIMEOUT_MS);

            vTaskDelete(s_task);
            s_task = NULL;
        }

        s_stop_req = false;
        // Готовим флаг к возможному повторному start(). [наш dht11_sensor.c]
    }

    s_ready = false;
    s_gpio = -1;
    // Сначала закрываем доступ: новые вызовы уйдут по проверке s_ready. [наш dht11_sensor.c]

    if (s_lock != NULL)
    {
        vSemaphoreDelete(s_lock);
        // Удаляем мютекс показаний. [FreeRTOS, freertos/semphr.h]

        s_lock = NULL;
    }

    if (s_read_lock != NULL)
    {
        vSemaphoreDelete(s_read_lock);
        // Удаляем мютекс чтения. [FreeRTOS, freertos/semphr.h]

        s_read_lock = NULL;
    }

    s_reading.temperature = 0.0f;
    s_reading.humidity = 0.0f;
    s_reading.status = DHT11_STATUS_ERROR;
    s_published = -1;
    // Данных больше нет — после публикации индикатор покажет ошибку. [наш dht11_sensor.c]

    s_publish(DHT11_STATUS_ERROR);

    ESP_LOGI(TAG, "Библиотека остановлена, ресурсы освобождены");
    return ESP_OK;
}

// ============================================================================
//  dht11_sensor_set_period_ms()
//
//  КРАТКО: Задаёт период фонового опроса. Значение меньше
//          DHT11_PERIOD_MIN_MS заменяется на DHT11_PERIOD_MS: спецификация
//          DHT11 — не чаще одного опроса в секунду.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: period_ms — период в миллисекундах. [стандарт C, int]
// ============================================================================
void dht11_sensor_set_period_ms(int period_ms)
{
    if (period_ms < DHT11_PERIOD_MIN_MS)
    {
        s_period_ms = DHT11_PERIOD_MS;
        ESP_LOGW(TAG, "Период %d мс слишком мал для DHT11 (нужно ≥ %d мс), берём %d мс",
                 period_ms, DHT11_PERIOD_MIN_MS, s_period_ms);
        return;
    }

    s_period_ms = period_ms;
    ESP_LOGI(TAG, "Период опроса: %d мс", s_period_ms);
}

// ============================================================================
//  dht11_sensor_start()
//
//  КРАТКО: Запускает фоновую задачу опроса с периодом s_period_ms.
//
//  Возвращает esp_err_t: ESP_OK, ESP_ERR_INVALID_STATE (нет init),
//                        ESP_FAIL (не удалось создать задачу).
// ============================================================================
esp_err_t dht11_sensor_start(void)
{
    if (!s_ready)
    {
        ESP_LOGE(TAG, "Нет инициализации — сначала вызовите dht11_sensor_init()");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task != NULL)
    {
        return ESP_OK;
        // Опрос уже идёт. [наш dht11_sensor.c]
    }

    s_stop_req = false;
    // Сбрасываем запрос остановки — задача должна запуститься вновь. [наш dht11_sensor.c]

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
//  dht11_sensor_count()
//
//  КРАТКО: Сколько датчиков обслуживает библиотека (DHT11 один на GPIO).
//
//  Возвращает int: 1 после init(), 0 иначе. [стандарт C]
// ============================================================================
int dht11_sensor_count(void)
{
    return s_ready ? 1 : 0;
}

// ============================================================================
//  dht11_sensor_read()
//
//  КРАТКО: Одно чтение DHT11 (≈5 мс), проверка паспортных диапазонов,
//          обновление кэша, публикация состояния и вызов колбэка.
//
//  Возвращает esp_err_t: ESP_OK (датчик ответил — даже с WARNING),
//          ESP_FAIL (не ответил / контрольная сумма),
//          ESP_ERR_INVALID_STATE (нет init).
//  Параметры: reading — куда записать снимок или NULL. [наш dht11_sensor.h]
// ============================================================================
esp_err_t dht11_sensor_read(dht11_reading_t *reading)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    dht11_reading_t result = {0.0f, 0.0f, DHT11_STATUS_ERROR};
    // Результат текущего цикла; при ошибке чтения — нули + ERROR. [наш dht11_sensor.h]

    static bool s_first_read = true;
    // Метки ниже печатаются только при первом чтении — диагностика без шума 1 Гц. [наш dht11_sensor.c]

    xSemaphoreTake(s_read_lock, portMAX_DELAY);
    // Сериализуем обращение к однопиновому датчику. [FreeRTOS, freertos/semphr.h]

    if (s_first_read)
    {
        ESP_LOGI(TAG, "Первое чтение: вызываю драйвер (GPIO%d)", s_gpio);
        // Одноразовая метка: задача дошла до вызова драйвера. [наш dht11_sensor.c]
    }

    esp_err_t err = dht_read_float_data(DHT_TYPE_DHT11, (gpio_num_t)s_gpio,
                                         &result.humidity, &result.temperature);
    // Чтение кадра драйвером esp-idf-lib/dht; внимание: первый выходной
    // параметр — влажность, второй — температура. [esp-idf-lib/dht, dht.h]

    if (s_first_read)
    {
        ESP_LOGI(TAG, "Первое чтение: драйвер вернул 0x%X (%s)",
                 (unsigned)err, err == ESP_OK ? "данные получены" : "нет ответа или ошибка");
        // Одноразовая метка: драйвер завершился (0 = успех). Если строки нет —
        // драйвер завис внутри, дальше ищем по логу `tasks` в консоли монитора. [наш dht11_sensor.c]
        s_first_read = false;
    }

    xSemaphoreGive(s_read_lock);

    bool read_ok = (err == ESP_OK);
    // Датчик ответил и контрольная сумма сошлась. [стандарт C, stdbool.h]

    if (!read_ok)
    {
        result.temperature = 0.0f;
        result.humidity = 0.0f;
        result.status = DHT11_STATUS_ERROR;
        ESP_LOGD(TAG, "Ошибка чтения (0x%X)", (unsigned)err);
    }
    else if (result.temperature < DHT11_TEMP_MIN_VALID || result.temperature > DHT11_TEMP_MAX_VALID ||
             result.humidity < DHT11_HUM_MIN_VALID || result.humidity > DHT11_HUM_MAX_VALID)
    {
        // Ответ получен, но значение вне паспортного диапазона. [наш dht11_sensor.h]
        result.status = DHT11_STATUS_WARNING;
        ESP_LOGW(TAG, "Вне паспортного диапазона: T=%.1f C, RH=%.1f %%",
                 result.temperature, result.humidity);
    }
    else
    {
        result.status = DHT11_STATUS_OK;
        ESP_LOGD(TAG, "T=%.1f C, RH=%.1f %%", result.temperature, result.humidity);
    }

    if (s_lock != NULL)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_reading = result;
        xSemaphoreGive(s_lock);
        // Обновляем кэш под мьютексом. [FreeRTOS, freertos/semphr.h]
    }

    if (reading != NULL)
    {
        *reading = result;
        // Отдаём снимок вызывающему, если буфер передан. [наш dht11_sensor.h]
    }

    s_publish(result.status);
    // Публикуем состояние в indicator. [наш indicator.h]

    dht11_on_readings_t cb = s_callback;
    // Локальная копия: колбэк может быть заменён из другой задачи. [наш dht11_sensor.h]

    if (cb != NULL)
    {
        cb(&result);
        // Вызов вне мьютексов — внутри колбэка можно звать get_*. [наш dht11_sensor.h]
    }

    return read_ok ? ESP_OK : ESP_FAIL;
}

// ============================================================================
//  dht11_sensor_get_reading()
//
//  КРАТКО: Снимок кэша (T + RH + статус) одним захватом мьютекса.
//
//  Возвращает esp_err_t: ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE.
//  Параметры: reading — куда записать снимок. [наш dht11_sensor.h]
// ============================================================================
esp_err_t dht11_sensor_get_reading(dht11_reading_t *reading)
{
    if (reading == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ready || s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *reading = s_reading;
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

// ============================================================================
//  dht11_sensor_get_temperature()
//
//  КРАТКО: Последняя температура из кэша — к датчику не обращаемся.
//
//  Возвращает float: температура °C или 0.0f до init(). [стандарт C]
// ============================================================================
float dht11_sensor_get_temperature(void)
{
    float temperature = 0.0f;

    if (!s_ready || s_lock == NULL)
    {
        return temperature;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    temperature = s_reading.temperature;
    xSemaphoreGive(s_lock);

    return temperature;
}

// ============================================================================
//  dht11_sensor_get_humidity()
//
//  КРАТКО: Последняя влажность из кэша — к датчику не обращаемся.
//
//  Возвращает float: влажность % или 0.0f до init(). [стандарт C]
// ============================================================================
float dht11_sensor_get_humidity(void)
{
    float humidity = 0.0f;

    if (!s_ready || s_lock == NULL)
    {
        return humidity;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    humidity = s_reading.humidity;
    xSemaphoreGive(s_lock);

    return humidity;
}

// ============================================================================
//  dht11_sensor_get_status()
//
//  КРАТКО: Статус датчика после последнего чтения.
//
//  Возвращает dht11_status_t: ERROR до init(). [наш dht11_sensor.h]
// ============================================================================
dht11_status_t dht11_sensor_get_status(void)
{
    dht11_status_t status = DHT11_STATUS_ERROR;

    if (!s_ready || s_lock == NULL)
    {
        return status;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    status = s_reading.status;
    xSemaphoreGive(s_lock);

    return status;
}

// ============================================================================
//  dht11_sensor_overall_status()
//
//  КРАТКО: «Худший» статус среди всех датчиков; датчик один — это статус
//          единственного датчика (форма API совместима с ds18b20_sensor).
//
//  Возвращает dht11_status_t. [наш dht11_sensor.h]
// ============================================================================
dht11_status_t dht11_sensor_overall_status(void)
{
    return dht11_sensor_get_status();
}

// ============================================================================
//  dht11_sensor_set_callback()
//
//  КРАТКО: Задаёт колбэк нового цикла чтения. Вызывать до start()/run().
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: callback — функция колбэка или NULL для отмены. [наш dht11_sensor.h]
// ============================================================================
void dht11_sensor_set_callback(dht11_on_readings_t callback)
{
    s_callback = callback;
    // Присваивание указателя атомарно на 32-битной архитектуре; надёжнее
    // задавать колбэк до запуска задачи, как и указано в заголовке. [наш dht11_sensor.h]
}

// ============================================================================
//  dht11_status_name()
//
//  КРАТКО: Текстовое имя статуса для вывода в консоль/лог.
//
//  Возвращает const char*: "OK" / "WARN" / "ERROR". [стандарт C]
//  Параметры: status — статус датчика. [наш dht11_sensor.h]
// ============================================================================
const char *dht11_status_name(dht11_status_t status)
{
    if (status == DHT11_STATUS_OK)
    {
        return "OK";
    }

    if (status == DHT11_STATUS_WARNING)
    {
        return "WARN";
    }

    return "ERROR";
}

// ============================================================================
//  dht11_sensor_run()
//
//  КРАТКО: Единая точка входа для main: поднимает индикацию, инициализирует
//          датчик и запускает фоновый опрос. Возвращается сразу.
//          Сбой индикации не фатален (работаем без светодиода), его код
//          в возврат не попадает.
//
//  Возвращает esp_err_t: ESP_OK; иные ESP_ERR_* от init()/start().
//  Параметры: dht_gpio — GPIO DATA датчика. [стандарт C, int]
//             rgb_led_gpio — GPIO встроенного WS2812. [стандарт C, int]
// ============================================================================
esp_err_t dht11_sensor_run(int dht_gpio, int rgb_led_gpio)
{
    if (indicator_init(rgb_led_gpio) != ESP_OK)
    {
        // Индикация не критична — продолжаем без светодиода.
        ESP_LOGE(TAG, "Индикация недоступна (GPIO%d) — работаем без неё", rgb_led_gpio);
    }

    esp_err_t err = dht11_sensor_init(dht_gpio);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Не удалось инициализировать DHT11 на GPIO%d", dht_gpio);
        return err;
        // Причина уже залогирована внутри init(). [наш dht11_sensor.h]
    }

    return dht11_sensor_start();
    // Датчик настроен — включаем периодический опрос. [наш dht11_sensor.h]
}
