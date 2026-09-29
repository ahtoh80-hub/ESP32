# Техническое задание на проект «DS18B20 + RGB-индикация» (ESP32-S3)

---

## 1. Общие сведения

| Параметр | Значение |
|---|---|
| **Название проекта** | DS18B20 |
| **Платформа** | ESP32-S3-N16R8 |
| **Фреймворк** | ESP-IDF v6.x |
| **Язык** | C (стандарт C11) |
| **Система сборки** | CMake + idf_component_register |
| **Менеджер компонентов** | ESP Component Registry (`idf_component.yml`) |
| **Среда разработки** | ESP-IDF + VS Code / idf.py |

---

## 2. Назначение и цель

Разработать прошивку для микроконтроллера ESP32-S3, которая:

1. Опрашивает один или несколько цифровых датчиков температуры **DS18B20**, подключённых по шине **1-Wire**.
2. Отображает текущее состояние системы через встроенный адресный RGB-светодиод **WS2812**.
3. Имеет модульную архитектуру: логика работы с датчиками, логика индикации и агрегатор ошибок вынесены в отдельные компоненты.
4. Точка входа `app_main()` содержит минимальный код — только один вызов запуска системы.
5. **Спроектирована с расчётом на расширение**: в будущем появятся другие датчики со своими библиотеками, и все они будут сообщать о своих ошибках в единый агрегатор индикации.

---

## 3. Аппаратная часть

| Компонент | Назначение | Подключение |
|---|---|---|
| ESP32-S3-N16R8 | Основной МК | — |
| DS18B20 (1–4 шт.) | Датчики температуры | Линия DQ → **GPIO 4**, питание 3.3 В, подтяжка 4.7 кОм к 3.3 В |
| WS2812 (встроенный) | RGB-индикация | **GPIO 48** (встроен в плату) |

**Ограничения:**
- Максимум датчиков DS18B20 на одной шине: **4** (`DS18B20_MAX_SENSORS`).
- Диапазон допустимых температур: **−55.0 … +125.0 °C**.
- Разрешение датчиков: **12 бит**.

---

## 4. Функциональные требования

### 4.1. Работа с датчиками (компонент `ds18b20_sensor`)

| № | Требование |
|---|---|
| F-01 | Инициализация шины 1-Wire на заданном GPIO через RMT-бэкенд |
| F-02 | Автоматический поиск всех устройств на шине и отбор только DS18B20 |
| F-03 | Сохранение ROM-адресов найденных датчиков в лог |
| F-04 | Установка разрешения 12 бит для каждого датчика |
| F-05 | Периодическое чтение температуры со всех датчиков (период 2000 мс) |
| F-06 | Обнаружение ошибок чтения (CRC, отсутствие ответа, значение −196.60 °C) |
| F-07 | Проверка температуры на выход за допустимый диапазон |
| F-08 | Вычисление «худшего» статуса среди всех своих датчиков (ERROR > WARNING > OK) |
| F-09 | Возврат кода ошибки `esp_err_t` для каждой операции |
| F-10 | **Публикация ошибок связи в общий агрегатор индикации** (`indicator_report_error` / `indicator_clear_error`) |

### 4.2. Индикация (компонент `rgb_led`)

| № | Требование |
|---|---|
| F-11 | Инициализация драйвера `led_strip` для одного WS2812 |
| F-12 | Установка цвета (R, G, B) с немедленным обновлением |
| F-13 | Выключение светодиода |
| F-14 | Предопределённые цвета: OFF, GREEN, YELLOW, RED |

### 4.3. Агрегатор индикации (новый компонент `indicator`)

| № | Требование |
|---|---|
| F-15 | Единая точка приёма ошибок от **любых** датчиков и библиотек |
| F-16 | API: `indicator_report_error(source)`, `indicator_clear_error(source)`, `indicator_set_warning(source, bool)` |
| F-17 | Внутреннее состояние — **битовая маска** активных ошибок по источникам |
| F-18 | Запускает FreeRTOS-задачу, которая управляет RGB-светодиодом |
| F-19 | Логика отображения (см. п. 4.4) |
| F-20 | Потокобезопасность: доступ к состоянию — через критическую секцию или атомарные операции |
| F-21 | Легко расширяется новыми источниками (перечисление `indicator_source_t`) |

### 4.4. Логика индикации (внутри `indicator`)

Логика строится по принципу **приоритета состояний**:

```
ERROR (любая ошибка связи хотя бы от одного источника)
   │
   ├──► КРАСНЫЙ: 5 быстрых вспышек (100 мс), пауза 1500 мс — повтор
   │
   ▼ если ошибок связи нет
WARNING (хотя бы один источник сообщил о некритичном предупреждении)
   │
   ├──► ЖЁЛТЫЙ: горит постоянно
   │
   ▼ если предупреждений нет
OK (нет ни ошибок, ни предупреждений)
   │
   └──► ЗЕЛЁНЫЙ: горит постоянно
```

| Состояние | Условие | Цвет | Паттерн |
|---|---|---|---|
| **ERROR** | Есть хотя бы одна активная ошибка связи от любого источника | Красный | 5 вспышек по 100 мс, пауза 1500 мс |
| **WARNING** | Ошибок нет, но есть хотя бы одно предупреждение | Жёлтый | Горит постоянно |
| **OK** | Ни ошибок, ни предупреждений | Зелёный | Горит постоянно |

**Ключевые принципы:**
- Ошибка **любого** источника (в том числе будущих) → красное мигание.
- Ошибка имеет **наивысший приоритет** над предупреждением.
- Предупреждение имеет приоритет над нормой.
- Если ошибка снята (`indicator_clear_error`) и предупреждений нет → возврат к зелёному.
- Если ошибка снята, но осталось предупреждение → жёлтый.

### 4.5. Точка входа (`main`)

| № | Требование |
|---|---|
| F-22 | `app_main()` содержит **один** вызов `ds18b20_sensor_run(ONEWIRE_BUS_GPIO, RGB_LED_GPIO)` |
| F-23 | Вся логика (инициализация, цикл опроса, индикация) — внутри библиотек |

---

## 5. Архитектура проекта

```
DS18B20/
├── CMakeLists.txt                          ← корневой файл сборки
├── main/
│   ├── CMakeLists.txt                      ← REQUIRES ds18b20_sensor indicator
│   ├── idf_component.yml                   ← зависимости из реестра
│   └── main.c                              ← точка входа (1 вызов)
├── components/
│   ├── indicator/                          ← НОВЫЙ компонент: агрегатор + задача индикации
│   │   ├── CMakeLists.txt                  ← REQUIRES rgb_led
│   │   ├── indicator.c
│   │   └── include/
│   │       └── indicator.h
│   ├── rgb_led/
│   │   ├── CMakeLists.txt                  ← REQUIRES espressif__led_strip
│   │   ├── rgb_led.c
│   │   └── include/
│   │       └── rgb_led.h
│   └── ds18b20_sensor/
│       ├── CMakeLists.txt                  ← REQUIRES espressif__ds18b20 espressif__onewire_bus indicator
│       ├── ds18b20_sensor.c                ← публикует ошибки в indicator
│       └── include/
│           └── ds18b20_sensor.h
└── managed_components/
    ├── espressif__onewire_bus/
    ├── espressif__ds18b20/
    └── espressif__led_strip/
```

**Поток данных:**

```
ds18b20_sensor  ──report_error/clear_error──►  indicator  ──►  rgb_led
   (источник)                                  (агрегатор)      (железо)
       ▲
       │
   другие датчики (в будущем)
```

---

## 6. Публичные API

### 6.1. `indicator.h` (новый)

```c
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Источники ошибок. Расширяется по мере добавления новых датчиков.
typedef enum {
    INDICATOR_SRC_DS18B20 = 0,   // датчики температуры DS18B20
    // INDICATOR_SRC_BME280,     // будущий датчик
    // INDICATOR_SRC_SHT31,      // будущий датчик
    INDICATOR_SRC_MAX
} indicator_source_t;

// Инициализация агрегатора и запуск задачи индикации.
// gpio_num — GPIO встроенного WS2812.
esp_err_t indicator_init(int gpio_num);

// Сообщить об ошибке связи от источника. Ошибка активна до clear.
void indicator_report_error(indicator_source_t src);

// Снять ошибку связи от источника.
void indicator_clear_error(indicator_source_t src);

// Установить/снять предупреждение от источника.
void indicator_set_warning(indicator_source_t src, bool active);

#ifdef __cplusplus
}
#endif
```

### 6.2. `ds18b20_sensor.h`

```c
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS18B20_MAX_SENSORS      4
#define DS18B20_TEMP_MIN_VALID  (-55.0f)
#define DS18B20_TEMP_MAX_VALID  (125.0f)

typedef enum {
    DS18B20_STATUS_OK = 0,
    DS18B20_STATUS_WARNING,
    DS18B20_STATUS_ERROR,
} ds18b20_status_t;

typedef struct {
    float             temperature;
    ds18b20_status_t  status;
} ds18b20_reading_t;

esp_err_t         ds18b20_sensor_init(int gpio_num);
int               ds18b20_sensor_count(void);
esp_err_t         ds18b20_sensor_read_all(ds18b20_reading_t *readings);
ds18b20_status_t  ds18b20_sensor_overall_status(void);
void              ds18b20_sensor_run(int onewire_gpio, int rgb_led_gpio);

#ifdef __cplusplus
}
#endif
```

### 6.3. `rgb_led.h`

```c
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r, g, b;
} rgb_color_t;

extern const rgb_color_t RGB_COLOR_OFF;
extern const rgb_color_t RGB_COLOR_GREEN;
extern const rgb_color_t RGB_COLOR_YELLOW;
extern const rgb_color_t RGB_COLOR_RED;

esp_err_t rgb_led_init(int gpio_num);
void      rgb_led_set(const rgb_color_t *color);
void      rgb_led_off(void);

#ifdef __cplusplus
}
#endif
```

---

## 7. Зависимости

### 7.1. Внешние (из реестра, `main/idf_component.yml`)

```yaml
dependencies:
  espressif/onewire_bus: "*"
  espressif/ds18b20:     "*"
  espressif/led_strip:   "^3.0.0"
```

### 7.2. Внутренние (локальные компоненты)

| Компонент | Зависит от |
|---|---|
| `main` | `ds18b20_sensor`, `indicator` |
| `ds18b20_sensor` | `espressif__ds18b20`, `espressif__onewire_bus`, `indicator` |
| `indicator` | `rgb_led` |
| `rgb_led` | `espressif__led_strip` |

---

## 8. Требования к качеству кода

| № | Требование |
|---|---|
| Q-01 | Разделение на `.h` (публичный API) и `.c` (реализация) |
| Q-02 | Все внутренние переменные и функции — `static` |
| Q-03 | Защита заголовков через `#pragma once` |
| Q-04 | Обёртка `extern "C"` для совместимости с C++ |
| Q-05 | Комментирование каждой функции: назначение, параметры, возврат |
| Q-06 | Комментарии-ссылки на источник типа/функции: `[ESP-IDF, esp_err.h]`, `[espressif/ds18b20, ds18b20.h]` |
| Q-07 | Логирование через `ESP_LOGI/W/E` с тегом компонента |
| Q-08 | Проверка всех возвращаемых `esp_err_t` |
| Q-09 | Проверка входных указателей на `NULL` |
| Q-10 | Отсутствие «магических чисел» — только `#define` или именованные константы |
| Q-11 | Потокобезопасность агрегатора `indicator` (portMUX / critical section) |

---

## 9. Поведение системы

### 9.1. Штатный режим

1. `app_main()` вызывает `ds18b20_sensor_run(4, 48)`.
2. `indicator_init(48)` создаёт задачу `indicator_task`.
3. Инициализируется шина 1-Wire на GPIO 4, находятся DS18B20.
4. Запускается бесконечный цикл опроса с периодом 2000 мс.
5. После каждого цикла `ds18b20_sensor`:
   - при ошибке связи хотя бы одного датчика → `indicator_report_error(INDICATOR_SRC_DS18B20)`;
   - при отсутствии ошибок → `indicator_clear_error(INDICATOR_SRC_DS18B20)`;
   - при выходе температуры за диапазон → `indicator_set_warning(INDICATOR_SRC_DS18B20, true)`;
   - иначе → `indicator_set_warning(INDICATOR_SRC_DS18B20, false)`.
6. `indicator_task` реагирует на смену состояния и управляет светодиодом.

### 9.2. Логика индикации

| Условие | Состояние | Отображение |
|---|---|---|
| Есть хотя бы одна активная ошибка | ERROR | Красный: 5 × 100 мс, пауза 1500 мс |
| Ошибок нет, есть предупреждение | WARNING | Жёлтый, горит постоянно |
| Ни ошибок, ни предупреждений | OK | Зелёный, горит постоянно |

### 9.3. Аварийные режимы

| Ситуация | Поведение |
|---|---|
| Не удалось создать шину 1-Wire | `indicator_report_error(DS18B20)`, `ds18b20_sensor_run` уходит в `vTaskDelay(1000)` |
| Не найдено ни одного DS18B20 | `indicator_report_error(DS18B20)` |
| Ошибка чтения конкретного датчика | `indicator_report_error(DS18B20)` |
| Температура вне диапазона | `indicator_set_warning(DS18B20, true)` |
| Не удалось инициализировать RGB | Лог `ESP_LOGE`, задача индикации удаляется |
| **Будущий датчик** сообщил об ошибке | `indicator_report_error(SRC_XXX)` → красное мигание |

---

## 10. Критерии приёмки

| № | Критерий |
|---|---|
| A-01 | Проект собирается командой `idf.py build` без ошибок и предупреждений |
| A-02 | Прошивка загружается на ESP32-S3-N16R8 через `idf.py flash monitor` |
| A-03 | В логе видны адреса всех найденных DS18B20 |
| A-04 | Температура выводится в лог каждые 2 секунды |
| A-05 | При нормальной работе светодиод **горит зелёным постоянно** |
| A-06 | При температуре вне диапазона светодиод **горит жёлтым постоянно** |
| A-07 | При отключении датчика светодиод **мигает красным** (5 × 100 мс / 1500 мс) |
| A-08 | После восстановления связи светодиод возвращается к зелёному |
| A-09 | `main.c` содержит не более 10 строк кода в `app_main()` |
| A-10 | Все публичные функции задокументированы в `.h` |
| A-11 | `idf.py size` показывает разумный размер прошивки |
| A-12 | Проект собирается «с нуля» после `idf.py fullclean` |
| A-13 | Добавление нового источника ошибок не требует правки `indicator.c` (только `indicator_source_t`) |

---

## 11. Ограничения и допущения

- Проект рассчитан на **ESP-IDF v6.x** (RMT-бэкенд `led_strip` v3.x).
- Питание датчиков — внешнее 3.3 В.
- Подтягивающий резистор 4.7 кОм на линии DQ **обязателен**.
- Максимум **4 датчика** DS18B20 на одной шине.
- Период опроса **фиксирован** (2000 мс).
- Проект **не использует** Wi-Fi, Bluetooth, NVS, файловую систему.
- `indicator` рассчитан максимум на `INDICATOR_SRC_MAX` источников (расширяется правкой enum).

---

## 12. Этапы разработки

| Этап | Содержание | Результат |
|---|---|---|
| 1 | Структура проекта, корневой `CMakeLists.txt` | Каркас |
| 2 | Компонент `rgb_led`: API + реализация | Управление цветом |
| 3 | Компонент `indicator`: агрегатор + задача индикации | Реакция на report/clear/set_warning |
| 4 | Компонент `ds18b20_sensor`: API + чтение + публикация ошибок | Датчики сообщают в indicator |
| 5 | `ds18b20_sensor_run()` — единая точка входа | `main.c` — 1 вызов |
| 6 | Документирование, тесты, проверка расширяемости | Готовый проект |

---

## 13. Глоссарий

| Термин | Определение |
|---|---|
| **1-Wire** | Однопроводная шина обмена данными (Dallas/Maxim) |
| **DS18B20** | Цифровой датчик температуры с уникальным 64-битным ROM-адресом |
| **WS2812** | Адресный RGB-светодиод с встроенным контроллером |
| **RMT** | Remote Control Transceiver — периферия ESP32 для точных временных последовательностей |
| **ROM-адрес** | Уникальный 64-битный идентификатор устройства на шине 1-Wire |
| **CRC** | Контрольная сумма для проверки целостности данных |
| **Компонент** | Модуль ESP-IDF с собственным `CMakeLists.txt` и публичным API |
| **Агрегатор** | Компонент `indicator`, собирающий ошибки/предупреждения от всех источников |
| **Источник** | Любая библиотека, публикующая своё состояние через `indicator_*` |

---

## 14. Приложение: минимальный `main.c`

```c
// ============================================================================
//  main/main.c
//
//  Точка входа проекта. Вся логика вынесена в ds18b20_sensor и indicator.
// ============================================================================

#include "ds18b20_sensor.h"

#define ONEWIRE_BUS_GPIO 4
#define RGB_LED_GPIO     48

void app_main(void)
{
    ds18b20_sensor_run(ONEWIRE_BUS_GPIO, RGB_LED_GPIO);
}
```

---

## 15. Приложение: `main/idf_component.yml`

```yaml
dependencies:
  espressif/onewire_bus: "*"
  espressif/ds18b20:     "*"
  espressif/led_strip:   "^3.0.0"
```

---

## 16. Приложение: `components/indicator/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "indicator.c"
    INCLUDE_DIRS "include"
    REQUIRES rgb_led
)
```

---

## 17. Приложение: `components/ds18b20_sensor/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "ds18b20_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__ds18b20 espressif__onewire_bus indicator
)
```

---

## 18. Приложение: `components/rgb_led/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "rgb_led.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__led_strip
)
```

---

## 19. Приложение: корневой `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(DS18B20)
```

---

## 20. Приложение: логика `indicator.c` (эскиз)

```c
// Внутреннее состояние — битовые маски по источникам.
static volatile uint32_t s_errors;    // 1 бит = активная ошибка связи
static volatile uint32_t s_warnings;  // 1 бит = активное предупреждение
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;

void indicator_report_error(indicator_source_t src)
{
    portENTER_CRITICAL(&s_lock);
    s_errors |= (1u << src);
    portEXIT_CRITICAL(&s_lock);
}

void indicator_clear_error(indicator_source_t src)
{
    portENTER_CRITICAL(&s_lock);
    s_errors &= ~(1u << src);
    portEXIT_CRITICAL(&s_lock);
}

void indicator_set_warning(indicator_source_t src, bool active)
{
    portENTER_CRITICAL(&s_lock);
    if (active) s_warnings |=  (1u << src);
    else        s_warnings &= ~(1u << src);
    portEXIT_CRITICAL(&s_lock);
}

// Задача индикации.
static void indicator_task(void *arg)
{
    while (1)
    {
        if (s_errors != 0)
        {
            // КРАСНЫЙ: 5 × 100 мс, пауза 1500 мс
            for (int i = 0; i < 5; i++) {
                rgb_led_set(&RGB_COLOR_RED);
                vTaskDelay(pdMS_TO_TICKS(100));
                rgb_led_off();
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        else if (s_warnings != 0)
        {
            // ЖЁЛТЫЙ: горит постоянно
            rgb_led_set(&RGB_COLOR_YELLOW);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else
        {
            // ЗЕЛЁНЫЙ: горит постоянно
            rgb_led_set(&RGB_COLOR_GREEN);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}
```

---

## 21. Приложение: публикация ошибок из `ds18b20_sensor.c` (эскиз)

```c
static void publish_state(ds18b20_status_t worst)
{
    switch (worst)
    {
    case DS18B20_STATUS_OK:
        indicator_clear_error(INDICATOR_SRC_DS18B20);
        indicator_set_warning(INDICATOR_SRC_DS18B20, false);
        break;

    case DS18B20_STATUS_WARNING:
        indicator_clear_error(INDICATOR_SRC_DS18B20);
        indicator_set_warning(INDICATOR_SRC_DS18B20, true);
        break;

    case DS18B20_STATUS_ERROR:
    default:
        indicator_report_error(INDICATOR_SRC_DS18B20);
        indicator_set_warning(INDICATOR_SRC_DS18B20, false);
        break;
    }
}
```

Вызывается в конце `ds18b20_sensor_read_all()` вместо прямого управления `rgb_led`.