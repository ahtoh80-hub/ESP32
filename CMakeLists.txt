# ============================================================================
#  CMakeLists.txt (корень проекта)
#
#  Верхнеуровневый файл сборки. Подключает систему сборки ESP-IDF
#  и объявляет проект.
# ============================================================================

# cmake_minimum_required — стандартная команда CMake.
cmake_minimum_required(VERSION 3.16)

# include(...) — стандартная команда CMake.
# project.cmake — часть ESP-IDF (tools/cmake/project.cmake).
# Он подхватывает компоненты из components/ и managed_components/.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# project() — стандартная команда CMake.
# Имя проекта должно совпадать с именем папки (DS18B20).
project(DS18B20)