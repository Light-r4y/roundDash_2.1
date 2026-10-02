# Подставляет {{VERSION}}/{{BUILD_DATE}}/{{AP_SSID_PREFIX}}/{{AP_PASSWORD}} в
# release_readme_template.txt и пишет результат в OutFile - см.
# build.bat/build.sh. Префикс SSID и пароль точки доступа по умолчанию читаются
# прямо из fw/common/config/conf.h, а не дублируются здесь вручную - одно место
# истины.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$BuildDate,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [Parameter(Mandatory = $true)][string]$TemplateFile,
    [Parameter(Mandatory = $true)][string]$ConfHeader
)

$confContent = Get-Content -Raw $ConfHeader
$apSsidPrefix = [regex]::Match($confContent, '#define\s+RG_WIFI_AP_SSID_PREFIX\s+"([^"]*)"').Groups[1].Value
$apPassword = [regex]::Match($confContent, '#define\s+RG_WIFI_AP_PASS_DEFAULT\s+"([^"]*)"').Groups[1].Value

$content = Get-Content -Raw $TemplateFile
$content = $content.Replace('{{VERSION}}', $Version).Replace('{{BUILD_DATE}}', $BuildDate).
    Replace('{{AP_SSID_PREFIX}}', $apSsidPrefix).Replace('{{AP_PASSWORD}}', $apPassword)
Set-Content -NoNewline -Path $OutFile -Value $content
