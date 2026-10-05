cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Собирать только main и то, от чего он зависит.
idf_build_set_property(MINIMAL_BUILD ON)

# Каждая задача — отдельный компонент в tasks/<имя>/, общий код — в common/<имя>/.
# Каталог-контейнер подхватывает все компоненты внутри себя.
set(EXTRA_COMPONENT_DIRS
    tasks
    common
)

set(SUPPORTED_TARGETS esp32s3)

# Стартовый уровень логов (см. main.c): обычный "idf.py build" остаётся на INFO
# для отладки, релизная сборка tools/build/build.* передаёт -D RG_LOG_LEVEL=WARN.
if(NOT DEFINED RG_LOG_LEVEL)
    set(RG_LOG_LEVEL "INFO")
endif()
idf_build_set_property(COMPILE_DEFINITIONS "RG_LOG_LEVEL_STR=\"${RG_LOG_LEVEL}\"" APPEND)

# Есть ли у экрана тач-панель: 1 (по умолчанию) или 0 - "idf.py -D RG_HAS_TOUCH=0 build".
# Без тача плата его не инициализирует и задачу тача не запускает; управление остаётся
# на кнопках. Значение -D живёт в кэше сборки: вернуть - "idf.py -D RG_HAS_TOUCH=1 reconfigure".
if(NOT DEFINED RG_HAS_TOUCH)
    set(RG_HAS_TOUCH 1)
endif()
idf_build_set_property(COMPILE_DEFINITIONS "RG_HAS_TOUCH=${RG_HAS_TOUCH}" APPEND)

# Версия приложения берётся ESP-IDF из version.txt рядом с этим файлом; его
# переписывает tools/build/build.* (0.<YY>.<MMDD>).
project(roundGauge)

# Два раздела SPIFFS, каждый из своей папки (docs/fw-design.md §5):
#   www   — веб-интерфейс настройки, обновляется целиком;
#   media — фоны, стрелки и прочие картинки приборки.
# Оба заливаются вместе с прошивкой: idf.py flash возвращает media к базовому
# набору из папки проекта и стирает загруженные через веб файлы — так решено.
spiffs_create_partition_image(www ./www FLASH_IN_PROJECT)
spiffs_create_partition_image(media ./media FLASH_IN_PROJECT)
