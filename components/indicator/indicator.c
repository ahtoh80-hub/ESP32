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

            rgb_led_set(&RGB_COLOR_YELLOW);
            // Горим жёлтым постоянно. [наш rgb_led.h]

            vTaskDelay(pdMS_TO_TICKS(INDICATOR_POLL_MS));
            // Ждём 500 мс и опрашиваем состояние снова. [FreeRTOS, freertos/task.h]
        }
        else
        {
            // Ни ошибок, ни предупреждений — ЗЕЛЁНЫЙ. [наш indicator.c]

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
//
//  Возвращает esp_err_t — типовой код ошибки ESP-IDF. [ESP-IDF, esp_err.h]
//  Возможные значения: ESP_OK, иные ESP_ERR_*.
//
//  Параметры: gpio_num — GPIO встроенного WS2812. [стандарт C, int]
// ============================================================================
esp_err_t indicator_init(int gpio_num)
{
    esp_err_t err = rgb_led_init(gpio_num);
    // Инициализируем драйвер светодиода. [наш rgb_led.h] + [ESP-IDF, esp_err.h]

    if (err != ESP_OK)
    {
        // Если драйвер не создан — логируем и выходим. [ESP-IDF, esp_err.h]

        ESP_LOGE(TAG, "Не удалось инициализировать RGB на GPIO%d", gpio_num);
        // Сообщаем об ошибке. [ESP-IDF, esp_log.h]

        return err;
        // Возвращаем код ошибки наверх. [ESP-IDF, esp_err.h]
    }

    BaseType_t ok = xTaskCreate(indicator_task, "indicator", INDICATOR_TASK_STACK,
                                NULL, INDICATOR_TASK_PRIO, NULL);
    // Создаём задачу индикации. [FreeRTOS, freertos/task.h]

    if (ok != pdPASS)
    {
        // Не удалось создать задачу — логируем и выходим. [FreeRTOS, freertos/task.h]

        ESP_LOGE(TAG, "Не удалось создать задачу индикации");
        // Сообщаем об ошибке. [ESP-IDF, esp_log.h]

        return ESP_FAIL;
        // Возвращаем общий код ошибки. [ESP-IDF, esp_err.h]
    }

    ESP_LOGI(TAG, "Индикатор запущен на GPIO%d", gpio_num);
    // Логируем успешный запуск. [ESP-IDF, esp_log.h]

    return ESP_OK;
    // Возвращаем успех. [ESP-IDF, esp_err.h]
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

    ESP_LOGW(TAG, "Источник %d: ошибка активна", (int)src);
    // Логируем событие. [ESP-IDF, esp_log.h]
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

    ESP_LOGI(TAG, "Источник %d: ошибка снята", (int)src);
    // Логируем событие. [ESP-IDF, esp_log.h]
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