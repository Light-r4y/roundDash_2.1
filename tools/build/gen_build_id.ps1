# Хеширует содержимое fw/www (имя+SHA256 каждого файла, отсортировано по пути)
# и пишет итоговый хеш в fw/www/.build_id - этот файл сам попадает в образ
# раздела www. Страница /ota сравнивает его до и после загрузки образа как
# запасной признак успеха (см. fw/tasks/webcfg/ota_page.h), прошивка сможет
# отдавать его как ETag для статики. Версия прошивки (fw/version.txt) для этого
# не годится: веб-интерфейс обновляется через /ota отдельно от прошивки и может
# поменяться без смены версии.
param(
    [string]$WwwDir = (Join-Path $PSScriptRoot "..\..\fw\www")
)

$root = (Resolve-Path $WwwDir).Path
$sha256 = [System.Security.Cryptography.SHA256]::Create()

$entries = Get-ChildItem -Recurse -File -Force $root |
    Where-Object { $_.Name -ne ".build_id" } |
    Sort-Object FullName |
    ForEach-Object {
        $rel = $_.FullName.Substring($root.Length).Replace('\', '/')
        $bytes = [System.IO.File]::ReadAllBytes($_.FullName)
        $hash = ($sha256.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join ''
        "$rel`:$hash"
    }

$joined = $entries -join "`n"
$finalBytes = [System.Text.Encoding]::UTF8.GetBytes($joined)
$finalHash = ($sha256.ComputeHash($finalBytes) | ForEach-Object { $_.ToString('x2') }) -join ''

Set-Content -NoNewline -Encoding ascii -Path (Join-Path $root ".build_id") -Value $finalHash
Write-Host "www build id: $finalHash"
