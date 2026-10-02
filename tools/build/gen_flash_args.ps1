# Пишет flash_args для папки релиза: берёт fw/build/flash_args (флаги esptool и
# смещения разделов - одно место истины, partitions.csv) и заменяет пути
# сборки на имена файлов релиза. С ним новая плата прошивается одной командой
# esptool ... write-flash "@flash_args" из папки релиза - см. build.bat/build.sh.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$BuildFlashArgs,
    [Parameter(Mandatory = $true)][string]$OutFile
)

$rename = @{
    'roundGauge.bin' = "roundGauge_firmware_v$Version.bin"
    'www.bin'        = "roundGauge_www_v$Version.bin"
    'media.bin'      = "roundGauge_media_v$Version.bin"
}

$lines = Get-Content $BuildFlashArgs | ForEach-Object {
    if ($_ -match '^(0x[0-9a-fA-F]+)\s+(\S+)$') {
        $file = Split-Path $Matches[2] -Leaf
        if ($rename.ContainsKey($file)) { $file = $rename[$file] }
        "$($Matches[1]) $file"
    } else {
        $_
    }
}
Set-Content -Encoding ascii -Path $OutFile -Value $lines
