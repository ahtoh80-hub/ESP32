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

// ============================================================================
//  indicator_task()
//
//  КРАТКО: Задача FreeRTOS, управляет RGB-светодиодом по текущему состоянию.
//
//  Возвращает void — ничего (задача работает вечно). [стандарт C]
//  Параметры: pvParameters — не используется (стандарт FreeRTOS). [FreeRTOS, freertos/task.h]
// ============================================================================
static void indicator_task(void *pvParameters)
{
    (void)pvParameters;
    // Явно помечаем неиспользуемый параметр, чтобы не было warning. [стандарт C]

    while (1)
    {
        // Бесконечный цикл индикации. [FreeRTOS, freertos/task.h]

        uint32_t errs;
        // Локальная копия маски ошибок. [стандарт C, stdint.h]

        uint32_t warns;
        // Локальная копия маски предупреждений. [стандарт C, stdint.h]

        portENTER_CRITICAL(&s_lock);
        // Входим в критическую секцию — читаем атомарно. [FreeRTOS, freertos/FreeRTOS.h]

        errs = s_errors;
        // Считываем маску ошибок. [наш indicator.c]

        warns = s_warnings;
        // Считываем маску предупреждений. [наш indicator.c]

        portEXIT_CRITICAL(&s_lock);
        // Выходим из критической секции. [FreeRTOS, freertos/FreeRTOS.h]

        if (errs != 0)
        {
            // Есть хотя бы одна ошибка от любого источника — КРАСНЫЙ. [наш indicator.c]

            if (s_state != 2)
            {
                // Состояние изменилось — сообщаем в лог один раз.
                s_state = 2;
                ESP_LOGE(TAG, "Индикация: ошибка");
            }

            for (int i = 0; i < INDICATOR_BLINK_COUNT; i++)
            {
                // Серия быстрых вспышек. [стандарт C]

                rgb_led_set(&RGB_COLOR_RED);
                // Включаем красный. [наш rgb_led.h]

                vTaskDelay(pdMS_TO_TICKS(INDICATOR_BLINK_ON_MS));
                // Держим красный 100 мс. [FreeRTOS, freertos/task.h]

                rgb_led_off();
                // Гасим светодиод. [наш rgb_led.h]

                vTaskDelay(pdMS_TO_TICKS(INDICATOR_BLINK_OFF_MS));
                // Пауза 100 мс. [FreeRTOS, freertos/task.h]
            }

            vTaskDelay(pdMS_TO_TICKS(INDICATOR_BLINK_PAUSE_MS));
            // Длинная пауза 1500 мс перед следующей серией. [FreeRTOS, freertos/task.h]
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

            vTaskDelay(pdMS_TO_TICKS(INDICATOR_POLL_MS));
            // Ждём 500 мс и опрашиваем состояние снова. [FreeRTOS, freertos/task.h]
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

            vTaskDelay(pdMS_TO_TICKS(INDICATOR_POLL_MS));
            // Ждём 500 мс и опрашиваем состояние снова. [FreeRTOS, freertos/task.h]
        }
    }
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
        vTaskDelete(s_task);
        // Останавливаем задачу индикации. [FreeRTOS, freertos/task.h]

        s_task = NULL;
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

    portEXIT_CRITICAL(&s_lock);
    // Выходим из критической секции. [FreeRTOS, freertos/FreeRTOS.h]
}