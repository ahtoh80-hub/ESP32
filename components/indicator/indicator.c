// ============================================================================
//  components/indicator/indicator.c
//
//  Реализация агрегатора индикации.
//  Хранит битовые маски ошибок/предупреждений по источникам
//  и управляет RGB-светодиодом в отдельной задаче FreeRTOS.
// ============================================================================

#include "indicator.h"
// Собственный публичный заголовок. [наш indicator.h]

#include "rgb_led.h"
// Наша библиотека RGB-светодиода. [наш rgb_led.h]

#include "esp_log.h"
// ESP_LOGI/LOGW/LOGE. [ESP-IDF, esp_log.h]

#include "freertos/FreeRTOS.h"
// pdMS_TO_TICKS, portMUX_TYPE, базовые макросы FreeRTOS. [FreeRTOS, freertos/FreeRTOS.h]

#include "freertos/task.h"
// xTaskCreate, vTaskDelay, vTaskDelete. [FreeRTOS, freertos/task.h]

#include <stdint.h>
// uint32_t. [стандарт C, stdint.h]

static const char *TAG = "INDICATOR";
// Тег логов. [наш indicator.c]

#define INDICATOR_TASK_STACK 3072
// Размер стека задачи индикации, байт. [наш indicator.c]

#define INDICATOR_TASK_PRIO 5
// Приоритет задачи индикации. [наш indicator.c]

#define INDICATOR_POLL_MS 500
// Период опроса состояния в режимах OK/WARNING, мс. [наш indicator.c]

#define INDICATOR_BLINK_ON_MS 100
// Длительность «вспышки» красного, мс. [наш indicator.c]

#define INDICATOR_BLINK_OFF_MS 100
// Длительность паузы между вспышками, мс. [наш indicator.c]

#define INDICATOR_BLINK_COUNT 5
// Количество вспышек в одной серии красного. [наш indicator.c]

#define INDICATOR_BLINK_PAUSE_MS 1500
// Пауза между сериями вспышек, мс. [наш indicator.c]

#define INDICATOR_SLICE_MS 50
// Шаг нарезки длинных задержек: между шагами проверяем запрос остановки. [наш indicator.c]

#define INDICATOR_STOP_TIMEOUT_MS 5000
// Максимум ожидания самостоятельного выхода задачи при deinit, мс. [наш indicator.c]

#define INDICATOR_STOP_POLL_MS 10
// Период опроса флага остановки при deinit, мс. [наш indicator.c]

static volatile uint32_t s_errors = 0;
// Битовая маска активных ошибок: бит i = 1 → источник i в ошибке. [наш indicator.c]

static volatile uint32_t s_warnings = 0;
// Битовая маска активных предупреждений: бит i = 1 → источник i в warning. [наш indicator.c]

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
// Спин-блокировка для защиты s_errors/s_warnings от гонок. [FreeRTOS, freertos/FreeRTOS.h]

static TaskHandle_t s_task = NULL;
// Дескриптор задачи индикации: NULL — задача не запущена. [FreeRTOS, freertos/task.h]

static int s_state = -1;
// Последнее выведенное состояние: -1 — старт, 0 — норма, 1 — предупреждение,
// 2 — ошибка. Нужно только для логирования смены состояния. [наш indicator.c]

static volatile bool s_stop_req = false;
// Запрос остановки задачи индикации: true — задача должна завершиться сама. [наш indicator.c]

// ============================================================================
//  s_read_masks()
//
//  КРАТКО: Атомарно копирует обе маски состояния в локальные переменные.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: errors / warnings — куда записать маски. [стандарт C, stdint.h]
// ============================================================================
static void s_read_masks(uint32_t *errors, uint32_t *warnings)
{
    // Копируем обе маски за один захват критической секции,
    // чтобы не получить «смешанное» состояние. [наш indicator.c]

    portENTER_CRITICAL(&s_lock);

    *errors = s_errors;
    *warnings = s_warnings;

    portEXIT_CRITICAL(&s_lock);
}

// ============================================================================
//  s_errors_active()
//
//  КРАТКО: Проверяет, осталась ли хотя бы одна активная ошибка.
//          Используется для прерывания серии мигания при снятии ошибки.
//
//  Возвращает bool: true — есть активная ошибка. [стандарт C, stdbool.h]
// ============================================================================
static bool s_errors_active(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t errs = s_errors;
    portEXIT_CRITICAL(&s_lock);

    return errs != 0;
}

// ============================================================================
//  ind_delay()
//
//  КРАТКО: Задержка, нарезанная на шаги INDICATOR_SLICE_MS.
//          Позволяет задаче быстро заметить запрос остановки при deinit.
//
//  Возвращает bool: false — запрошена остановка, задержка прервана.
//  Параметры: ms — общая длительность задержки, мс. [стандарт C, int]
// ============================================================================
static bool ind_delay(int ms)
{
    int elapsed = 0;

    while (elapsed < ms)
    {
        if (s_stop_req)
        {
            return false;
        }

        int slice = ms - elapsed;
        // Последний шаг — ровно остаток, без удлинения. [стандарт C]

        if (slice > INDICATOR_SLICE_MS)
        {
            slice = INDICATOR_SLICE_MS;
        }

        vTaskDelay(pdMS_TO_TICKS(slice));
        elapsed += slice;
    }

    return !s_stop_req;
}

// ============================================================================
//  indicator_task()
//
//  КРАТКО: Задача FreeRTOS, управляет RGB-светодиодом по текущему состоянию.
//          Состояние перечитывается при каждой нотификации (вызовы report/clear/
//          set_warning будят задачу сразу) и, страховочно, с периодом
//          INDICATOR_POLL_MS. Серия красных вспышек прерывается сразу после
//          снятия ошибки. Остановка — кооперативная: по s_stop_req задача
//          выходит сама и удаляет себя через vTaskDelete(NULL).
//
//  Возвращает void — ничего (удаляет саму себя при остановке). [стандарт C]
//  Параметры: pvParameters — не используется (стандарт FreeRTOS). [FreeRTOS, freertos/task.h]
// ============================================================================
static void indicator_task(void *pvParameters)
{
    (void)pvParameters;
    // Явно помечаем неиспользуемый параметр, чтобы не было warning. [стандарт C]

    while (!s_stop_req)
    {
        // Бесконечный цикл индикации; выход — по запросу остановки. [наш indicator.c]

        uint32_t errs;
        // Локальная копия маски ошибок. [стандарт C, stdint.h]

        uint32_t warns;
        // Локальная копия маски предупреждений. [стандарт C, stdint.h]

        s_read_masks(&errs, &warns);
        // Атомарно читаем обе маски. [наш indicator.c]

        if (errs != 0)
        {
            // Есть хотя бы одна ошибка от любого источника — КРАСНЫЙ. [наш indicator.c]

            if (s_state != 2)
            {
                // Состояние изменилось — сообщаем в лог один раз.
                s_state = 2;
                ESP_LOGE(TAG, "Индикация: ошибка");
            }

            bool series_ok = true;
            // false — серия прервана (остановка задачи). [стандарт C, stdbool.h]

            for (int i = 0; i < INDICATOR_BLINK_COUNT && series_ok; i++)
            {
                // Серия быстрых вспышек. [стандарт C]

                if (!s_errors_active())
                {
                    // Ошибка уже снята — серию не продолжаем, задача
                    // немедленно перейдёт к новому состоянию. [наш indicator.c]
                    break;
                }

                rgb_led_set(&RGB_COLOR_RED);
                // Включаем красный. [наш rgb_led.h]

                series_ok = ind_delay(INDICATOR_BLINK_ON_MS);
                // Держим красный 100 мс (прерываемо при остановке). [наш indicator.c]

                if (series_ok)
                {
                    rgb_led_off();
                    // Гасим светодиод. [наш rgb_led.h]

                    series_ok = ind_delay(INDICATOR_BLINK_OFF_MS);
                    // Пауза 100 мс (прерываема при остановке). [наш indicator.c]
                }
            }

            if (series_ok && !s_stop_req)
            {
                // Длинная пауза 1500 мс перед следующей серией.
                // Нотификация прерывает её сразу при смене состояния. [FreeRTOS, freertos/task.h]
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(INDICATOR_BLINK_PAUSE_MS));
            }
        }
        else if (warns != 0)
        {
            // Ошибок нет, но есть предупреждение — ЖЁЛТЫЙ. [наш indicator.c]

            if (s_state != 1)
            {
                s_state = 1;
                ESP_LOGW(TAG, "Индикация: предупреждение");
            }

            rgb_led_set(&RGB_COLOR_YELLOW);
            // Горим жёлтым постоянно. [наш rgb_led.h]

            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(INDICATOR_POLL_MS));
            // Ждём смену состояния (нотификация) или страховочные 500 мс. [наш indicator.c]
        }
        else
        {
            // Ни ошибок, ни предупреждений — ЗЕЛЁНЫЙ. [наш indicator.c]

            if (s_state != 0)
            {
                s_state = 0;
                ESP_LOGI(TAG, "Индикация: норма");
            }

            rgb_led_set(&RGB_COLOR_GREEN);
            // Горим зелёным постоянно. [наш rgb_led.h]

            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(INDICATOR_POLL_MS));
            // Ждём смену состояния (нотификация) или страховочные 500 мс. [наш indicator.c]
        }
    }

    portENTER_CRITICAL(&s_lock);
    s_task = NULL;
    portEXIT_CRITICAL(&s_lock);
    // Помечаем задачу остановленной под локом: сеттеры, читающие s_task
    // внутри этой же секции, не смогут уведомить уже удалённую задачу. [наш indicator.c]

    vTaskDelete(NULL);
    // Удаляем сами себя; выполнение дальше не идёт. [FreeRTOS, freertos/task.h]
}

// ============================================================================
//  indicator_init()
//
//  КРАТКО: Инициализирует RGB-светодиод и запускает задачу индикации.
//          Повторный вызов безопасен — вторая задача не создаётся.
//
//  Возвращает esp_err_t: ESP_OK или код ошибки. [ESP-IDF, esp_err.h]
//  Параметры: gpio_num — GPIO встроенного WS2812. [стандарт C, int]
// ============================================================================
esp_err_t indicator_init(int gpio_num)
{
    if (s_task != NULL)
    {
        // Задача уже работает — второй раз не создаём.
        return ESP_OK;
    }

    esp_err_t err = rgb_led_init(gpio_num);
    // Инициализируем драйвер светодиода. [наш rgb_led.h]

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Не удалось инициализировать RGB на GPIO%d (0x%X)", gpio_num, (unsigned)err);
        return err;
    }

    s_stop_req = false;
    // Сбрасываем запрос остановки — задача должна запуститься вновь. [наш indicator.c]

    if (xTaskCreate(indicator_task, "indicator", INDICATOR_TASK_STACK,
                    NULL, INDICATOR_TASK_PRIO, &s_task) != pdPASS)
    {
        // Задачу создать не удалось — освобождаем светодиод.
        s_task = NULL;
        rgb_led_deinit();

        ESP_LOGE(TAG, "Не удалось создать задачу индикации");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Индикатор запущен на GPIO%d", gpio_num);
    return ESP_OK;
}

// ============================================================================
//  indicator_deinit()
//
//  КРАТКО: Останавливает задачу индикации, сбрасывает маски и гасит RGB.
//          Повторный вызов безопасен.
//
//  Возвращает esp_err_t: ESP_OK или код ошибки драйвера. [ESP-IDF, esp_err.h]
// ============================================================================
esp_err_t indicator_deinit(void)
{
    if (s_task != NULL)
    {
        s_stop_req = true;
        // Просим задачу завершиться: она выходит из цикла сама
        // и удаляет себя через vTaskDelete(NULL).
        // Нарочно НЕ шлём нотификацию: задача могла успеть завершиться
        // между проверкой и notify — notify по освобождённому TCB недопустим.
        // Пробуждение и так наступает в течение ≤ 1500 мс
        // (таймауты ulTaskNotifyTake / шаги ind_delay). [наш indicator.c]

        for (int i = 0; i < INDICATOR_STOP_TIMEOUT_MS / INDICATOR_STOP_POLL_MS && s_task != NULL; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(INDICATOR_STOP_POLL_MS));
            // Ждём, пока задача выйдет и обнулит s_task. [наш indicator.c]
        }

        if (s_task != NULL)
        {
            // Страховка: задача не заметила запрос (например, зависла в драйвере).
            // Удаляем принудительно — иначе deinit зависнет навсегда. [FreeRTOS, freertos/task.h]
            ESP_LOGE(TAG, "Задача индикации не остановилась за %d мс — удаляю принудительно",
                     INDICATOR_STOP_TIMEOUT_MS);

            vTaskDelete(s_task);
            s_task = NULL;
        }

        s_stop_req = false;
        // Готовим флаг к возможному повторному init(). [наш indicator.c]
    }

    portENTER_CRITICAL(&s_lock);
    s_errors = 0;
    s_warnings = 0;
    portEXIT_CRITICAL(&s_lock);
    // Сбрасываем маски ошибок и предупреждений. [наш indicator.c]

    s_state = -1;
    // Следующий запуск начнёт логировать состояние заново. [наш indicator.c]

    return rgb_led_deinit();
    // Гасим и освобождаем светодиод. [наш rgb_led.h]
}

// ============================================================================
//  indicator_report_error()
//
//  КРАТКО: Атомарно ставит бит ошибки для указанного источника.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: src — источник ошибки. [наш indicator.h]
// ============================================================================
void indicator_report_error(indicator_source_t src)
{
    if (src < 0 || src >= INDICATOR_SRC_MAX)
    {
        // Защита от некорректного источника. [стандарт C]

        return;
        // Ничего не делаем. [стандарт C]
    }

    portENTER_CRITICAL(&s_lock);
    // Входим в критическую секцию. [FreeRTOS, freertos/FreeRTOS.h]

    s_errors |= (1u << src);
    // Ставим бит источника в маске ошибок. [стандарт C]

    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
        // Будим задачу индикации — она увидит ошибку сразу, без ожидания
        // опроса. Notify строго внутри критической секции: задача обнуляет
        // s_task под тем же локом до self-delete — гонки с notify нет. [FreeRTOS, freertos/task.h]
    }

    portEXIT_CRITICAL(&s_lock);
    // Выходим из критической секции. [FreeRTOS, freertos/FreeRTOS.h]
}

// ============================================================================
//  indicator_clear_error()
//
//  КРАТКО: Атомарно снимает бит ошибки для указанного источника.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: src — источник ошибки. [наш indicator.h]
// ============================================================================
void indicator_clear_error(indicator_source_t src)
{
    if (src < 0 || src >= INDICATOR_SRC_MAX)
    {
        // Защита от некорректного источника. [стандарт C]

        return;
        // Ничего не делаем. [стандарт C]
    }

    portENTER_CRITICAL(&s_lock);
    // Входим в критическую секцию. [FreeRTOS, freertos/FreeRTOS.h]

    s_errors &= ~(1u << src);
    // Снимаем бит источника в маске ошибок. [стандарт C]

    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
        // Будим задачу — снятие ошибки тоже должно отразиться сразу. [FreeRTOS, freertos/task.h]
    }

    portEXIT_CRITICAL(&s_lock);
    // Выходим из критической секции. [FreeRTOS, freertos/FreeRTOS.h]
}

// ============================================================================
//  indicator_set_warning()
//
//  КРАТКО: Атомарно ставит/снимает бит предупреждения для источника.
//
//  Возвращает void — ничего. [стандарт C]
//  Параметры: src — источник. [наш indicator.h]
//             active — true поставить, false снять. [стандарт C, stdbool.h]
// ============================================================================
void indicator_set_warning(indicator_source_t src, bool active)
{
    if (src < 0 || src >= INDICATOR_SRC_MAX)
    {
        // Защита от некорректного источника. [стандарт C]

        return;
        // Ничего не делаем. [стандарт C]
    }

    portENTER_CRITICAL(&s_lock);
    // Входим в критическую секцию. [FreeRTOS, freertos/FreeRTOS.h]

    if (active)
    {
        // Нужно поставить предупреждение. [стандарт C]

        s_warnings |= (1u << src);
        // Ставим бит источника в маске предупреждений. [стандарт C]
    }
    else
    {
        // Нужно снять предупреждение. [стандарт C]

        s_warnings &= ~(1u << src);
        // Снимаем бит источника в маске предупреждений. [стандарт C]
    }

    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
        // Будим задачу — смена предупреждения отражается сразу. [FreeRTOS, freertos/task.h]
    }

    portEXIT_CRITICAL(&s_lock);
    // Выходим из критической секции. [FreeRTOS, freertos/FreeRTOS.h]
}