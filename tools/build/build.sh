#!/usr/bin/env bash
# Builds the roundGauge firmware and packages a release: everything needed to
# flash a new board over USB, plus the images for the /ota page.
# Lives in tools/build/ - run from anywhere, it locates the project itself.
#
# Usage:
#   tools/build/build.sh
#
# If idf.py is not already on PATH, this script tries to activate the ESP-IDF
# environment itself: via IDF_PATH if set, or via the default install location
# ($HOME/esp/v6.0/esp-idf) - override with the IDF_PATH environment variable.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
FW_DIR="$REPO_DIR/fw"
cd "$FW_DIR"

if ! command -v idf.py >/dev/null 2>&1; then
    IDF_PATH="${IDF_PATH:-$HOME/esp/v6.0/esp-idf}"
    if [ -f "$IDF_PATH/export.sh" ]; then
        echo "Activating ESP-IDF environment: $IDF_PATH"
        # shellcheck source=/dev/null
        source "$IDF_PATH/export.sh"
    else
        echo "idf.py not found on PATH, and export.sh not found at \"$IDF_PATH\"." >&2
        echo "Activate the ESP-IDF environment yourself (source export.sh) or set IDF_PATH and run this script again." >&2
        exit 1
    fi
fi

# Версия релиза = 0.<год YY>.<месяц MM><день DD> сборки - выставляется
# автоматически при каждом релизном билде, fw/version.txt руками менять не надо.
# Major = 0, пока проект в разработке.
VERSION="0.$(date +%y).$(date +%m%d)"
printf '%s\n' "$VERSION" > version.txt

echo "=== gen_build_id ==="
"$SCRIPT_DIR/gen_build_id.sh"

echo "=== idf.py build ==="
# RG_LOG_LEVEL=WARN - только для этого релизного билда (см. fw/CMakeLists.txt),
# обычный "idf.py build" без -D остаётся на INFO. Сбрасываем кэш обратно в
# INFO сразу после сборки, иначе следующий "idf.py build" тоже унаследует
# WARN из CMakeCache.txt.
idf.py -D RG_LOG_LEVEL=WARN build
idf.py -D RG_LOG_LEVEL=INFO reconfigure >/dev/null

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
BUILD_DATE="$(date '+%Y-%m-%d %H:%M:%S')"
RELEASE_DIR="$REPO_DIR/releases/roundGauge_v${VERSION}_${TIMESTAMP}"
mkdir -p "$RELEASE_DIR"

FW_BIN="roundGauge_firmware_v${VERSION}.bin"
WWW_BIN="roundGauge_www_v${VERSION}.bin"
MEDIA_BIN="roundGauge_media_v${VERSION}.bin"

cp "build/roundGauge.bin" "$RELEASE_DIR/$FW_BIN"
cp "build/www.bin" "$RELEASE_DIR/$WWW_BIN"
cp "build/media.bin" "$RELEASE_DIR/$MEDIA_BIN"
cp "build/bootloader/bootloader.bin" "$RELEASE_DIR/bootloader.bin"
cp "build/partition_table/partition-table.bin" "$RELEASE_DIR/partition-table.bin"
cp "build/ota_data_initial.bin" "$RELEASE_DIR/ota_data_initial.bin"

# flash_args из сборки, но с именами файлов релиза - см. gen_flash_args.ps1.
awk -v fw="$FW_BIN" -v www="$WWW_BIN" -v media="$MEDIA_BIN" '
    NF == 2 && $1 ~ /^0x/ {
        n = split($2, p, "/"); f = p[n]
        if (f == "roundGauge.bin") f = fw
        else if (f == "www.bin") f = www
        else if (f == "media.bin") f = media
        print $1, f; next
    }
    { print }
' build/flash_args > "$RELEASE_DIR/flash_args"

# SSID/пароль точки доступа по умолчанию - прямо из fw/common/config/conf.h,
# одно место истины вместо дублирования значений в шаблоне.
CONF_H="$FW_DIR/common/config/conf.h"
AP_SSID_PREFIX="$(sed -n 's/^#define RG_WIFI_AP_SSID_PREFIX *"\(.*\)".*/\1/p' "$CONF_H" | head -1)"
AP_PASSWORD="$(sed -n 's/^#define RG_WIFI_AP_PASS_DEFAULT *"\(.*\)".*/\1/p' "$CONF_H" | head -1)"

sed -e "s/{{VERSION}}/${VERSION}/g" -e "s/{{BUILD_DATE}}/${BUILD_DATE}/g" \
    -e "s/{{AP_SSID_PREFIX}}/${AP_SSID_PREFIX}/g" -e "s/{{AP_PASSWORD}}/${AP_PASSWORD}/g" \
    "$SCRIPT_DIR/release_readme_template.txt" > "$RELEASE_DIR/README.txt"

# Портативный набор для прошивки на столе: esptool.exe, flash_all/update_firmware (.bat и .sh),
# и zip всей папки рядом с ней - см. package_release.py.
echo "=== package_release ==="
PYTHON="$(command -v python3 || command -v python)"
"$PYTHON" "$SCRIPT_DIR/package_release.py" "$RELEASE_DIR" "$VERSION"

echo
echo "=== Done ==="
echo "Version:     ${VERSION}"
echo "Release dir: ${RELEASE_DIR}"
echo "Release zip: ${RELEASE_DIR}.zip"
