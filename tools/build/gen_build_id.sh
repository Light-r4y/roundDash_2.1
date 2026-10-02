#!/usr/bin/env bash
# Хеширует содержимое fw/www (имя+SHA256 каждого файла, отсортировано по пути)
# и пишет итоговый хеш в fw/www/.build_id - см. gen_build_id.ps1 для полного
# объяснения, зачем он нужен отдельно от fw/version.txt.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${1:-$SCRIPT_DIR/../../fw/www}"

if command -v sha256sum >/dev/null 2>&1; then
    HASH_CMD=(sha256sum)
else
    HASH_CMD=(shasum -a 256)
fi

hash_file() {
    "${HASH_CMD[@]}" "$1" | awk '{print $1}'
}

# Строки "/путь:sha256" через \n без завершающего перевода строки - ровно то,
# что хеширует gen_build_id.ps1, поэтому хеш одинаков на любой ОС.
LIST="$(
    cd "$ROOT"
    find . -type f ! -name ".build_id" | LC_ALL=C sort | while read -r f; do
        printf '%s:%s\n' "/${f#./}" "$(hash_file "$f")"
    done
)"
FINAL_HASH="$(printf '%s' "$LIST" | "${HASH_CMD[@]}" | awk '{print $1}')"

printf '%s' "$FINAL_HASH" > "$ROOT/.build_id"
echo "www build id: $FINAL_HASH"
