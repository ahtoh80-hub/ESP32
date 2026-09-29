# ============================================================================
#  CMakeLists.txt (корень проекта)
#
#  Верхнеуровневый файл сборки. Подключает систему сборки ESP-IDF
#  и объявляет проект.
# ============================================================================

# Минимальная версия CMake.
# 3.16 — требование ESP-IDF v6.0.
# Верхняя граница НЕ ставится специально: ESP-IDF v6.0.2 уже умеет
# работать с CMake 4.x, но если у вас v6.0.0/v6.0.1 — лучше
# переустановить CMake через `idf_tools.py install cmake` (см. ниже).
cmake_minimum_required(VERSION 3.16)

# --- Защита от бага CMake 4.0.3 на Windows -------------------------------
# В CMake 4.0.3 сломан file(GLOB) при обходе подпапок: список файлов
# возвращается пустым, из-за чего ESP-IDF создаёт компонент как INTERFACE
# library и затем падает на component_compile_options(PRIVATE ...).
# Политика CMP0169 возвращает старое поведение file(GLOB).
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    cmake_policy(SET CMP0169 OLD)
endif()
# -------------------------------------------------------------------------

# include(...) — стандартная команда CMake.
# project.cmake — часть ESP-IDF (tools/cmake/project.cmake).
# Он подхватывает компоненты из components/ и managed_components/.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# project() — стандартная команда CMake.
# Имя проекта должно совпадать с именем папки (DS18B20).
project(DS18B20)