@echo off
REM Builds the roundGauge firmware and packages a release: everything needed to
REM flash a new board over USB, plus the images for the /ota page.
REM Lives in tools/build/ - run from anywhere, it locates the project itself.
REM
REM Usage:
REM   tools\build\build.bat
REM
REM If idf.py is not already on PATH, this script tries to activate the ESP-IDF
REM environment itself: via IDF_PATH/IDF_PYTHON_ENV if set, or via the paths used
REM to develop this project - override with the same environment variables.
setlocal enabledelayedexpansion

for %%i in ("%~dp0..\..") do set "REPO_DIR=%%~fi"
set "FW_DIR=%REPO_DIR%\fw"
cd /d "%FW_DIR%"

where idf.py >nul 2>nul
if errorlevel 1 (
    if "!IDF_PATH!"=="" set "IDF_PATH=%USERPROFILE%\esp\v6.0\esp-idf"
    if "!IDF_PYTHON_ENV!"=="" set "IDF_PYTHON_ENV=%USERPROFILE%\.espressif\python_env\idf6.0_py3.11_env\Scripts"

    if not exist "!IDF_PATH!\export.bat" (
        echo idf.py not found on PATH, and export.bat not found at "!IDF_PATH!".
        echo Activate the ESP-IDF environment yourself ^(export.bat^) or set IDF_PATH and run this script again.
        exit /b 1
    )

    echo Activating ESP-IDF environment: !IDF_PATH!
    REM Python from the IDF venv goes first: export.bat would otherwise pick the
    REM system Python and look for a venv that was never created for it.
    if exist "!IDF_PYTHON_ENV!" set "PATH=!IDF_PYTHON_ENV!;%PATH%"
    call "!IDF_PATH!\export.bat"
    if errorlevel 1 (
        echo Failed to activate the ESP-IDF environment.
        exit /b 1
    )
)

REM Версия релиза = 0.<год YY>.<месяц MM><день DD> сборки - выставляется
REM автоматически при каждом релизном билде, fw/version.txt руками менять не надо.
REM Major = 0, пока проект в разработке.
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yy"') do set "VER_YY=%%i"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format MMdd"') do set "VER_MMDD=%%i"
set "VERSION=0.%VER_YY%.%VER_MMDD%"
echo %VERSION%> version.txt

echo === gen_build_id ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gen_build_id.ps1"
if errorlevel 1 (
    echo Failed to generate www build id.
    exit /b 1
)

echo === idf.py build ===
REM RG_LOG_LEVEL=WARN и RG_UI_SHOW_FPS=0 (без строк FPS на экране) - только для этого релизного
REM билда (см. fw/CMakeLists.txt), обычный "idf.py build" без -D остаётся на INFO и с FPS.
REM Сбрасываем кэш обратно сразу после сборки, иначе следующий "idf.py build" тоже
REM унаследует эти значения из CMakeCache.txt.
call idf.py -D RG_LOG_LEVEL=WARN -D RG_UI_SHOW_FPS=0 build
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
call idf.py -D RG_LOG_LEVEL=INFO -D RG_UI_SHOW_FPS=1 reconfigure >nul

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TIMESTAMP=%%i"
for /f "delims=" %%i in ('powershell -NoProfile -Command "Get-Date -Format 'yyyy-MM-dd HH:mm:ss'"') do set "BUILD_DATE=%%i"

set "RELEASE_DIR=%REPO_DIR%\releases\roundGauge_v%VERSION%_%TIMESTAMP%"
mkdir "%RELEASE_DIR%" 2>nul

copy /y "build\roundGauge.bin" "%RELEASE_DIR%\roundGauge_firmware_v%VERSION%.bin" >nul
copy /y "build\www.bin" "%RELEASE_DIR%\roundGauge_www_v%VERSION%.bin" >nul
copy /y "build\media.bin" "%RELEASE_DIR%\roundGauge_media_v%VERSION%.bin" >nul
copy /y "build\bootloader\bootloader.bin" "%RELEASE_DIR%\bootloader.bin" >nul
copy /y "build\partition_table\partition-table.bin" "%RELEASE_DIR%\partition-table.bin" >nul
copy /y "build\ota_data_initial.bin" "%RELEASE_DIR%\ota_data_initial.bin" >nul

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gen_flash_args.ps1" -Version "%VERSION%" -BuildFlashArgs "%FW_DIR%\build\flash_args" -OutFile "%RELEASE_DIR%\flash_args"
if errorlevel 1 (
    echo Failed to generate flash_args.
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gen_release_readme.ps1" -Version "%VERSION%" -BuildDate "%BUILD_DATE%" -OutFile "%RELEASE_DIR%\README.txt" -TemplateFile "%~dp0release_readme_template.txt" -ConfHeader "%FW_DIR%\common\config\conf.h"

REM Портативный набор для прошивки на столе: esptool.exe, flash_all/update_firmware (.bat и .sh),
REM и zip всей папки рядом с ней - см. package_release.py.
echo === package_release ===
python "%~dp0package_release.py" "%RELEASE_DIR%" "%VERSION%"
if errorlevel 1 (
    echo Failed to package the release.
    exit /b 1
)

echo.
echo === Done ===
echo Version:      %VERSION%
echo Release dir:  %RELEASE_DIR%
echo Release zip:  %RELEASE_DIR%.zip

endlocal
