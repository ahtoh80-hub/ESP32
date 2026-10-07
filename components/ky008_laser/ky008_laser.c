// ============================================================================
//  components/ky008_laser/ky008_laser.c
//
//  Самодостаточная библиотека лазерного модуля KY-008 (излучатель, выход).
//  Период контрольного цикла — 1000 мс по умолчанию, задаётся через
//  ky008_laser_set_period_ms(). Фоновая задача сверяет фактический уровень
//  на пине с заданной командой (readback); расхождение — статус WARNING.
//  Состояние публикуется в indicator.
// ============================================================================

#include "ky008_laser.h"
// Собственный публичный заголовок. [наш ky008_laser.h]

#include "indicator.h"
// Агрегатор индикации — сюда публикуем состояние. [наш indicator.h]

#include "esp_log.h"
// ESP_LOGI/LOGW/LOGE. [ESP-IDF, esp_log.h]

#include "driver/gpio.h"
// gpio_config(), gpio_set_level(), gpio_get_level(), GPIO_NUM_MAX. [ESP-IDF, driver/gpio.h]

#include "freertos/FreeRTOS.h"
// Базовые типы FreeRTOS, pdMS_TO_TICKS. [FreeRTOS, freertos/FreeRTOS.h]

#include "freertos/task.h"
// xTaskCreate, vTaskDelay, xTaskGetTickCount. [FreeRTOS, freertos/task.h]

#include "freertos/semphr.h"
// Мютексы. [FreeRTOS, freertos/semphr.h]

static const char *TAG = "KY008";
// Тег логов. [наш ky008_laser.c]

#define TASK_NAME "ky008"
// Имя задачи контроля. [FreeRTOS, freertos/task.h]

#define TASK_STACK 4096
// Стек задачи контроля, байт — как у остальных библиотек. [FreeRTOS, freertos/task.h]

#define TASK_PRIO 5
// Приоритет задачи контроля — как у остальных библиотек. [FreeRTOS, freertos/task.h]

#define TASK_SLICE_MS 50
// Шаг нарезки пауз: между шагами проверяем запрос остановки. [наш ky008_laser.c]

#define TASK_STOP_TIMEOUT_MS 5000
// Максимум ожидания самостоятельного выхода задачи при deinit, мс. [наш ky008_laser.c]

#define TASK_STOP_POLL_MS 10
// Период опроса флага остановки при deinit, мс. [наш ky008_laser.c]

static int s_gpio = -1;
// GPIO модуля; -1 — не инициализировано. [наш ky008_laser.c]

static bool s_ready = false;
// Флаг инициализации: библиотека готова к работе. [наш ky008_laser.c]

static ky008_reading_t s_reading = {false, 0, KY008_STATUS_ERROR};
// Последние показания (кэш для main). [наш ky008_laser.h]

static SemaphoreHandle_t s_lock = NULL;
// Защита s_reading от гонок. [FreeRTOS, freertos/semphr.h]

static SemaphoreHandle_t s_read_lock = NULL;
// Сериализация обращий к пину: запись команды (set) и сверка уровня (read)
// не должны выполняться одновременно. [FreeRTOS, freertos/semphr.h]

static bool s_commanded = false;
// Последняя заданная команда (ожидаемое состояние пина). [наш ky008_laser.c]

static int s_period_ms = KY008_PERIOD_MS;
// Период контрольного цикла, мс. [наш ky008_laser.c]

static TaskHandle_t s_task = NULL;
// Задача контроля: NULL — не запущена. [FreeRTOS, freertos/task.h]

static volatile bool s_stop_req = false;
// Запрос остановки задачи контроля: true — задача должна завершиться сама. [наш ky008_laser.c]

static ky008_on_readings_t s_callback = NULL;
// Колбэк изменения состояния; NULL — не задан. [наш ky008_laser.h]

static int s_published = -1;
// Последнее опубликованное в indicator состояние; -1 — ещё ничего. [наш ky008_laser.c]

// Внутренние функции.
static void s_publish(ky008_status_t status);
// Публикация состояния в indicator (только при смене). [наш indicator.h]

static bool s_delay(TickType_t ticks);
// Нарезанная задержка с проверкой запроса остановки. [FreeRTOS, freertos/task.h]

static void s_apply(bool enabled);
// Запись состояния на пин + обновление кэша + колбэк. [наш ky008_laser.c]

static ky008_status_t s_verify(void);
// Сверка фактического уровня на пине с командой. [наш ky008_laser.c]

static void s_task_body(void *pvParameters);
// Тело задачи контроля. [FreeRTOS, freertos/task.h]

// __TAIL__
