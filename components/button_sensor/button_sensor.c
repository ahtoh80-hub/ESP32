// ============================================================================
//  components/button_sensor/button_sensor.c
//
//  Самодостаточная библиотека дискретной кнопки (1 GPIO, вход + внутренняя
//  подтяжка). Период фонового опроса — 20 мс по умолчанию, задаётся через
//  button_sensor_set_period_ms(). Антидребезг 50 мс; дребезг дольше 500 мс —
//  статус WARNING. Состояние публикуется в indicator.
// ============================================================================

#include "button_sensor.h"
// Собственный публичный заголовок. [наш button_sensor.h]

#include "indicator.h"
// Агрегатор индикации — сюда публикуем состояние. [наш indicator.h]

#include "esp_log.h"
// ESP_LOGI/LOGW/LOGE. [ESP-IDF, esp_log.h]

#include "driver/gpio.h"
// gpio_config(), gpio_get_level(), GPIO_NUM_MAX. [ESP-IDF, driver/gpio.h]

#include "freertos/FreeRTOS.h"
// Базовые типы FreeRTOS, pdMS_TO_TICKS. [FreeRTOS, freertos/FreeRTOS.h]

#include "freertos/task.h"
// xTaskCreate, vTaskDelay, xTaskGetTickCount. [FreeRTOS, freertos/task.h]

#include "freertos/semphr.h"
// Мютексы. [FreeRTOS, freertos/semphr.h]

static const char *TAG = "BUTTON";
// Тег логов. [наш button_sensor.c]

#define TASK_NAME "button"
// Имя задачи опроса. [FreeRTOS, freertos/task.h]

#define TASK_STACK 4096
// Стек задачи опроса, байт — как у ds18b20/dht11. [FreeRTOS, freertos/task.h]

#define TASK_PRIO 5
// Приоритет задачи опроса — как у ds18b20/dht11. [FreeRTOS, freertos/task.h]

#define TASK_SLICE_MS 50
// Шаг нарезки пауз: между шагами проверяем запрос остановки. [наш button_sensor.c]

#define TASK_STOP_TIMEOUT_MS 5000
// Максимум ожидания самостоятельного выхода задачи при deinit, мс. [наш button_sensor.c]

#define TASK_STOP_POLL_MS 10
// Период опроса флага остановки при deinit, мс. [наш button_sensor.c]

static int s_gpio = -1;
// GPIO кнопки; -1 — не инициализировано. [наш button_sensor.c]

static bool s_ready = false;
// Флаг инициализации: библиотека готова к опросу. [наш button_sensor.c]

static button_reading_t s_reading = {false, 0, BUTTON_STATUS_ERROR};
// Последние показания (кэш для main). [наш button_sensor.h]

static SemaphoreHandle_t s_lock = NULL;
// Защита s_reading от гонок. [FreeRTOS, freertos/semphr.h]

static SemaphoreHandle_t s_read_lock = NULL;
// Сериализация сэмплов: фоновая задача и ручной read() не должны
// одновременно двигать автоматы дебаунса. [FreeRTOS, freertos/semphr.h]

static int s_period_ms = BUTTON_PERIOD_MS;
// Период фонового опроса, мс. [наш button_sensor.c]

static TaskHandle_t s_task = NULL;
// Задача фонового опроса: NULL — не запущена. [FreeRTOS, freertos/task.h]

static volatile bool s_stop_req = false;
// Запрос остановки задачи опроса: true — задача должна завершиться сама. [наш button_sensor.c]

static button_on_readings_t s_callback = NULL;
// Колбэк изменения состояния; NULL — не задан. [наш button_sensor.h]

static int s_published = -1;
// Последнее опубликованное в indicator состояние; -1 — ещё ничего. [наш button_sensor.c]

// Автоматы дебаунса (только под s_read_lock, доступны из read()).
static bool s_stable = false;
// Подтверждённое состояние «нажата». [наш button_sensor.c]

static bool s_first_sample = true;
// Первый сэмпл после init(): уровень принимается без задержки. [наш button_sensor.c]

static bool s_cand_valid = false;
// Есть неподтверждённый кандидат на новое состояние. [наш button_sensor.c]

static bool s_cand = false;
// Кандидат на новое состояние (pressed). [наш button_sensor.c]

static TickType_t s_cand_tick = 0;
// Когда кандидат появился (отсчёт дебаунса). [наш button_sensor.c]

static bool s_disturbed = false;
// Идёт эпизод нестабильности (вход уходил из подтверждённого состояния). [наш button_sensor.c]

static TickType_t s_disturb_tick = 0;
// Начало эпизода нестабильности (отсчёт дребезга). [наш button_sensor.c]

static bool s_calm_valid = false;
// Идёт отсчёт «спокойного» времени внутри эпизода. [наш button_sensor.c]

static TickType_t s_calm_tick = 0;
// Когда вход вернулся в подтверждённое состояние. [наш button_sensor.c]

// Внутренние функции.
static void s_publish(button_status_t status);
// Публикация состояния в indicator (только при смене). [наш indicator.h]

static bool s_delay(TickType_t ticks);
// Нарезанная задержка с проверкой запроса остановки. [FreeRTOS, freertos/task.h]

static void s_sample(bool *changed, button_status_t *status);
// Один сэмпл входа + дебаунс/дребезг; вызывать под s_read_lock. [наш button_sensor.c]

static void s_task_body(void *pvParameters);
// Тело задачи фонового опроса. [FreeRTOS, freertos/task.h]

// ============================================================================
//  s_publish()
//
//  КРАТКО: Передаёт состояние в indicator и пишет в лог только смену
//          состояния — не каждый опрос (50 раз в секунду).
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: status — статус текущего опроса. [наш button_sensor.h]
// ============================================================================
static void s_publish(button_status_t status)
{
    if ((int)status == s_published)
    {
        // Состояние не изменилось — ни в indicator, ни в лог ничего не шлём. [наш button_sensor.c]
        return;
    }

    s_published = (int)status;

    if (status == BUTTON_STATUS_ERROR)
    {
        // Данных ещё нет: нет init() или первый сэмпл не сделан.
        indicator_report_error(INDICATOR_SRC_BUTTON);
        indicator_set_warning(INDICATOR_SRC_BUTTON, false);
        ESP_LOGE(TAG, "Состояние: данных нет (нет инициализации)");
    }
    else if (status == BUTTON_STATUS_WARNING)
    {
        // Вход дребезжит дольше BUTTON_BOUNCE_MAX_MS — контакт нестабилен.
        indicator_clear_error(INDICATOR_SRC_BUTTON);
        indicator_set_warning(INDICATOR_SRC_BUTTON, true);
        ESP_LOGW(TAG, "Состояние: дребезг дольше %d мс — контакт нестабилен",
                 BUTTON_BOUNCE_MAX_MS);
    }
    else
    {
        // Всё хорошо.
        indicator_clear_error(INDICATOR_SRC_BUTTON);
        indicator_set_warning(INDICATOR_SRC_BUTTON, false);
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
//  s_sample()
//
//  КРАТКО: Один сэмпл входа + автомат дебаунса.
//          Первый сэмпл после init() принимается сразу (исходное состояние,
//          нажатием не считается). Дальше: сырой уровень должен держаться
//          BUTTON_DEBOUNCE_MS, чтобы переход подтвердился; если вход
//          дребезжит без подтверждения дольше BUTTON_BOUNCE_MAX_MS —
//          статус WARNING.
//
//  Возвращает void — результат через параметры.
//  Параметры: changed — true: состояние подтвердилось (нажатие/отпускание).
//             status  — статус после сэмпла (OK / WARNING).
//             Вызывать под s_read_lock! [наш button_sensor.c]
// ============================================================================
static void s_sample(bool *changed, button_status_t *status)
{
    TickType_t now = xTaskGetTickCount();

    bool raw = (gpio_get_level((gpio_num_t)s_gpio) == BUTTON_ACTIVE_LEVEL);
    // Сырой уровень: true — контакты замкнуты (с учётом активного уровня). [ESP-IDF, driver/gpio.h]

    *changed = false;
    *status = BUTTON_STATUS_OK;

    if (s_first_sample)
    {
        s_first_sample = false;
        s_stable = raw;
        s_cand_valid = false;
        s_disturbed = false;
        s_calm_valid = false;
        // Исходное состояние фиксируется сразу — это не нажатие, а факт. [наш button_sensor.c]
        return;
    }

    if (raw == s_stable)
    {
        // Вход в подтверждённом состоянии — возможный конец дребезга.
        s_cand_valid = false;

        if (s_disturbed)
        {
            if (!s_calm_valid)
            {
                s_calm_valid = true;
                s_calm_tick = now;
            }
            else if ((now - s_calm_tick) >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS))
            {
                s_disturbed = false;
                s_calm_valid = false;
                // Дребезг прекратился в исходном состоянии — эпизод закрыт. [наш button_sensor.c]
            }
        }
    }
    else
    {
        // Вход ушёл из подтверждённого состояния: отсчёт дебаунса и дребезга.
        if (!s_disturbed)
        {
            s_disturbed = true;
            s_disturb_tick = now;
            // Начало нового эпизода нестабильности. [наш button_sensor.c]
        }

        s_calm_valid = false;
        // Любой новый отход от исходного состояния обнуляет «спокойный» отсчёт. [наш button_sensor.c]

        if (!s_cand_valid || s_cand != raw)
        {
            s_cand = raw;
            s_cand_valid = true;
            s_cand_tick = now;
            // Новый кандидат — перезапускаем отсчёт дебаунса. [наш button_sensor.c]
        }

        if ((now - s_cand_tick) >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS))
        {
            // Новое состояние держится достаточно долго — подтверждаем. [наш button_sensor.c]
            s_stable = raw;
            s_cand_valid = false;
            s_disturbed = false;
            s_calm_valid = false;
            *changed = true;
            *status = BUTTON_STATUS_OK;
            return;
        }
    }

    if (s_disturbed && (now - s_disturb_tick) >= pdMS_TO_TICKS(BUTTON_BOUNCE_MAX_MS))
    {
        *status = BUTTON_STATUS_WARNING;
        // Дребезг дольше BUTTON_BOUNCE_MAX_MS: контакт нестабилен. [наш button_sensor.c]
    }
}

// ============================================================================
//  s_task_body()
//
//  КРАТКО: Задача FreeRTOS — опрашивает вход с заданным периодом.
//          Первый сэмпл делается сразу (для кнопки стабилизация не нужна).
//          Период отсчитывается от начала цикла. Выход кооперативный:
//          по s_stop_req задача сама удаляет себя через vTaskDelete(NULL).
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

        button_sensor_read(NULL);
        // Сэмпл + дебаунс; результат сохраняется в кэш и уходит в колбэк. [наш button_sensor.h]

        TickType_t elapsed = xTaskGetTickCount() - start;
        // Сколько длился опрос. [FreeRTOS, freertos/task.h]

        TickType_t period = pdMS_TO_TICKS(s_period_ms);
        // Заданный период в тиках. [FreeRTOS, freertos/FreeRTOS.h]

        if (period == 0)
        {
            period = 1;
            // Страховка: при разрешении тика 10 мс период < 10 мс
            // скатился бы в 0 и устроил busy-loop. [наш button_sensor.c]
        }

        TickType_t remaining = (elapsed < period) ? (period - elapsed) : 0;
        // Сколько осталось доспать; если опрос длился дольше периода — не ждём. [наш button_sensor.c]

        s_delay(remaining);
        // Нарезанная пауза: между шагами проверяем запрос остановки. [наш button_sensor.c]
    }

    s_task = NULL;
    // Помечаем задачу остановленной — deinit продолжит освобождение ресурсов. [наш button_sensor.c]

    vTaskDelete(NULL);
    // Удаляем сами себя; выполнение дальше не идёт. [FreeRTOS, freertos/task.h]
}

// ============================================================================
//  button_sensor_init()
//
//  КРАТКО: Настраивает GPIO как вход с внутренней подтяжкой и создаёт
//          мьютексы. Кнопка НЕ опрашивается — состояние проверит первый
//          цикл (до этого статус ERROR — «данных ещё нет»).
//
//  Возвращает esp_err_t: ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM,
//          код gpio_config(). [ESP-IDF, esp_err.h]
//  Параметры: gpio_num — GPIO кнопки. [стандарт C, int]
// ============================================================================
esp_err_t button_sensor_init(int gpio_num)
{
    if (s_ready)
    {
        // Кнопка уже инициализирована — повторно не настраиваем.
        ESP_LOGW(TAG, "Уже инициализирован — повторный вызов пропущен");
        return ESP_OK;
    }

    if (gpio_num < 0 || gpio_num >= GPIO_NUM_MAX)
    {
        ESP_LOGE(TAG, "Некорректный GPIO%d", gpio_num);
        return ESP_ERR_INVALID_ARG;
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
        // Мютекс сериализации сэмплов (задача + ручной read()). [FreeRTOS, freertos/semphr.h]

        if (s_read_lock == NULL)
        {
            ESP_LOGE(TAG, "Не удалось создать мютекс чтения");
            return ESP_ERR_NO_MEM;
        }
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (BUTTON_ACTIVE_LEVEL == 0) ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    // Вход без прерываний; подтяжка — по активному уровню: при BUTTON_ACTIVE_LEVEL=0
    // (кнопка к GND) включается pull-up, при 1 (кнопка к 3.3 В) — pull-down. [ESP-IDF, driver/gpio.h]

    esp_err_t err = gpio_config(&cfg);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Не удалось настроить GPIO%d (0x%X)", gpio_num, (unsigned)err);
        return err;
    }

    s_gpio = gpio_num;
    s_reading.pressed = false;
    s_reading.presses = 0;
    s_reading.status = BUTTON_STATUS_ERROR;
    // До первого сэмпла данных нет. [наш button_sensor.c]

    s_stable = false;
    s_first_sample = true;
    s_cand_valid = false;
    s_disturbed = false;
    s_calm_valid = false;
    // Сбрасываем автоматы дебаунса к исходному состоянию. [наш button_sensor.c]

    s_ready = true;

    s_published = -1;
    s_publish(BUTTON_STATUS_ERROR);
    // Индикатор красный до первого сэмпла. [наш indicator.h]

    ESP_LOGI(TAG, "Инициализация завершена, GPIO%d, активный уровень %d (состояние проверит первый цикл)",
             gpio_num, BUTTON_ACTIVE_LEVEL);
    return ESP_OK;
}

// ============================================================================
//  button_sensor_deinit()
//
//  КРАТКО: Останавливает фоновый опрос (кооперативно) и освобождает
//          мьютексы. После этого библиотеку можно инициализировать заново.
//
//  Возвращает esp_err_t: ESP_OK. [ESP-IDF, esp_err.h]
// ============================================================================
esp_err_t button_sensor_deinit(void)
{
    if (s_task != NULL)
    {
        s_stop_req = true;
        // Просим задачу завершиться: она выходит из цикла сама
        // и удаляет себя через vTaskDelete(NULL). [наш button_sensor.c]

        for (int i = 0; i < TASK_STOP_TIMEOUT_MS / TASK_STOP_POLL_MS && s_task != NULL; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(TASK_STOP_POLL_MS));
            // Ждём, пока задача выйдет из read/паузы и обнулит s_task. [наш button_sensor.c]
        }

        if (s_task != NULL)
        {
            // Страховка: задача не заметила запрос. Удаляем принудительно. [FreeRTOS, freertos/task.h]
            ESP_LOGE(TAG, "Задача опроса не остановилась за %d мс — удаляю принудительно",
                     TASK_STOP_TIMEOUT_MS);

            vTaskDelete(s_task);
            s_task = NULL;
        }

        s_stop_req = false;
        // Готовим флаг к возможному повторному start(). [наш button_sensor.c]
    }

    s_ready = false;
    s_gpio = -1;
    // Сначала закрываем доступ: новые вызовы уйдут по проверке s_ready. [наш button_sensor.c]

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

    s_reading.pressed = false;
    s_reading.presses = 0;
    s_reading.status = BUTTON_STATUS_ERROR;

    s_stable = false;
    s_first_sample = true;
    s_cand_valid = false;
    s_disturbed = false;
    s_calm_valid = false;
    // Данных больше нет — после публикации индикатор покажет ошибку. [наш button_sensor.c]

    s_published = -1;
    s_publish(BUTTON_STATUS_ERROR);

    ESP_LOGI(TAG, "Библиотека остановлена, ресурсы освобождены");
    return ESP_OK;
}

// ============================================================================
//  button_sensor_set_period_ms()
//
//  КРАТКО: Задаёт период фонового опроса. Значение меньше
//          BUTTON_PERIOD_MIN_MS заменяется на BUTTON_PERIOD_MS.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: period_ms — период в миллисекундах. [стандарт C, int]
// ============================================================================
void button_sensor_set_period_ms(int period_ms)
{
    if (period_ms < BUTTON_PERIOD_MIN_MS)
    {
        s_period_ms = BUTTON_PERIOD_MS;
        ESP_LOGW(TAG, "Период %d мс слишком мал (нужно ≥ %d мс), берём %d мс",
                 period_ms, BUTTON_PERIOD_MIN_MS, s_period_ms);
        return;
    }

    s_period_ms = period_ms;
    ESP_LOGI(TAG, "Период опроса: %d мс", s_period_ms);
}

// ============================================================================
//  button_sensor_start()
//
//  КРАТКО: Запускает фоновую задачу опроса с периодом s_period_ms.
//
//  Возвращает esp_err_t: ESP_OK, ESP_ERR_INVALID_STATE (нет init),
//                        ESP_FAIL (не удалось создать задачу).
// ============================================================================
esp_err_t button_sensor_start(void)
{
    if (!s_ready)
    {
        ESP_LOGE(TAG, "Нет инициализации — сначала вызовите button_sensor_init()");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task != NULL)
    {
        return ESP_OK;
        // Опрос уже идёт. [наш button_sensor.c]
    }

    s_stop_req = false;
    // Сбрасываем запрос остановки — задача должна запуститься вновь. [наш button_sensor.c]

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
//  button_sensor_count()
//
//  КРАТКО: Сколько кнопок обслуживает библиотека (кнопка одна на GPIO).
//
//  Возвращает int: 1 после init(), 0 иначе. [стандарт C]
// ============================================================================
int button_sensor_count(void)
{
    return s_ready ? 1 : 0;
}

// ============================================================================
//  button_sensor_read()
//
//  КРАТКО: Живой опрос: один сэмпл входа + дебаунс, обновление кэша,
//          публикация состояния; колбэк вызывается только при
//          подтверждённом изменении (нажатие/отпускание).
//
//  Возвращает esp_err_t: ESP_OK, ESP_ERR_INVALID_STATE (нет init).
//  Параметры: reading — куда записать снимок или NULL. [наш button_sensor.h]
// ============================================================================
esp_err_t button_sensor_read(button_reading_t *reading)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bool changed = false;
    // Было ли подтверждённое изменение состояния в этом сэмпле. [наш button_sensor.c]

    bool pressed = false;
    // Подтверждённое состояние «нажата» из этого сэмпла. [наш button_sensor.c]

    button_status_t status = BUTTON_STATUS_ERROR;
    // Статус после сэмпла. [наш button_sensor.h]

    xSemaphoreTake(s_read_lock, portMAX_DELAY);
    // Сериализуем сэмпл: задача и ручной read() не двигают автоматы одновременно. [FreeRTOS, freertos/semphr.h]

    s_sample(&changed, &status);
    pressed = s_stable;
    // Копируем состояние под локом: после освобождения s_stable может
    // изменить только другой держатель s_read_lock. [наш button_sensor.c]

    xSemaphoreGive(s_read_lock);

    button_reading_t snapshot;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_reading.pressed = pressed;

    if (changed && pressed)
    {
        s_reading.presses++;
        // Считаем только подтверждённые фронты false → true. [наш button_sensor.c]
    }

    s_reading.status = status;
    snapshot = s_reading;
    // Снимок под мьютексом — согласованные данные. [FreeRTOS, freertos/semphr.h]

    xSemaphoreGive(s_lock);

    if (reading != NULL)
    {
        *reading = snapshot;
        // Отдаём снимок вызывающему, если буфер передан. [наш button_sensor.h]
    }

    s_publish(status);
    // Публикуем состояние в indicator (только при смене). [наш indicator.h]

    if (changed)
    {
        button_on_readings_t cb = s_callback;
        // Локальная копия: колбэк может быть заменён из другой задачи. [наш button_sensor.h]

        if (cb != NULL)
        {
            cb(&snapshot);
            // Вызов вне мьютексов — внутри колбэка можно звать get_*. [наш button_sensor.h]
        }
    }

    return ESP_OK;
}

// ============================================================================
//  button_sensor_get_reading()
//
//  КРАТКО: Снимок кэша (pressed + presses + статус) одним захватом мьютекса.
//
//  Возвращает esp_err_t: ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE.
//  Параметры: reading — куда записать снимок. [наш button_sensor.h]
// ============================================================================
esp_err_t button_sensor_get_reading(button_reading_t *reading)
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
//  button_sensor_is_pressed()
//
//  КРАТКО: Состояние «нажата» из кэша — к GPIO не обращаемся.
//
//  Возвращает bool: true — нажата; false — отпущена или до init(). [стандарт C]
// ============================================================================
bool button_sensor_is_pressed(void)
{
    bool pressed = false;

    if (!s_ready || s_lock == NULL)
    {
        return pressed;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    pressed = s_reading.pressed;
    xSemaphoreGive(s_lock);

    return pressed;
}

// ============================================================================
//  button_sensor_press_count()
//
//  КРАТКО: Число подтверждённых нажатий с момента init() — из кэша.
//
//  Возвращает uint32_t: счётчик нажатий; 0 до init(). [стандарт C, stdint.h]
// ============================================================================
uint32_t button_sensor_press_count(void)
{
    uint32_t presses = 0;

    if (!s_ready || s_lock == NULL)
    {
        return presses;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    presses = s_reading.presses;
    xSemaphoreGive(s_lock);

    return presses;
}

// ============================================================================
//  button_sensor_get_status()
//
//  КРАТКО: Статус кнопки после последнего опроса.
//
//  Возвращает button_status_t: ERROR до init(). [наш button_sensor.h]
// ============================================================================
button_status_t button_sensor_get_status(void)
{
    button_status_t status = BUTTON_STATUS_ERROR;

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
//  button_sensor_overall_status()
//
//  КРАТКО: «Худший» статус; кнопка одна — это статус единственной кнопки
//          (форма API совместима с ds18b20_sensor/dht11_sensor).
//
//  Возвращает button_status_t. [наш button_sensor.h]
// ============================================================================
button_status_t button_sensor_overall_status(void)
{
    return button_sensor_get_status();
}

// ============================================================================
//  button_sensor_set_callback()
//
//  КРАТКО: Задаёт колбэк изменения состояния. Вызывать до start()/run().
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: callback — функция колбэка или NULL для отмены. [наш button_sensor.h]
// ============================================================================
void button_sensor_set_callback(button_on_readings_t callback)
{
    s_callback = callback;
    // Присваивание указателя атомарно на 32-битной архитектуре; надёжнее
    // задавать колбэк до запуска задачи, как и указано в заголовке. [наш button_sensor.h]
}

// ============================================================================
//  button_sensor_status_name()
//
//  КРАТКО: Текстовое имя статуса для вывода в консоль/лог.
//
//  Возвращает const char*: "OK" / "WARN" / "ERROR". [стандарт C]
//  Параметры: status — статус кнопки. [наш button_sensor.h]
// ============================================================================
const char *button_sensor_status_name(button_status_t status)
{
    if (status == BUTTON_STATUS_OK)
    {
        return "OK";
    }

    if (status == BUTTON_STATUS_WARNING)
    {
        return "WARN";
    }

    return "ERROR";
}

// ============================================================================
//  button_sensor_run()
//
//  КРАТКО: Единая точка входа для main: поднимает индикацию, инициализирует
//          кнопку и запускает фоновый опрос. Возвращается сразу.
//          Сбой индикации не фатален (работаем без светодиода), его код
//          в возврат не попадает.
//
//  Возвращает esp_err_t: ESP_OK; иные ESP_ERR_* от init()/start(). [ESP-IDF, esp_err.h]
//  Параметры: button_gpio — GPIO кнопки. [стандарт C, int]
//             rgb_led_gpio — GPIO встроенного WS2812. [стандарт C, int]
// ============================================================================
esp_err_t button_sensor_run(int button_gpio, int rgb_led_gpio)
{
    if (indicator_init(rgb_led_gpio) != ESP_OK)
    {
        // Индикация не критична — продолжаем без светодиода.
        ESP_LOGE(TAG, "Индикация недоступна (GPIO%d) — работаем без неё", rgb_led_gpio);
    }

    esp_err_t err = button_sensor_init(button_gpio);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Не удалось инициализировать кнопку на GPIO%d", button_gpio);
        return err;
        // Причина уже залогирована внутри init(). [наш button_sensor.h]
    }

    return button_sensor_start();
    // Кнопка настроена — включаем периодический опрос. [наш button_sensor.h]
}
