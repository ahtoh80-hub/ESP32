# Временный скрипт проверки сборки: активирует ESP-IDF и запускает idf.py build.
# Лог пишется в build_check.log рядом со скриптом.
$ErrorActionPreference = 'Continue'
$proj = $PSScriptRoot

. 'C:\esp\v6.0.2\esp-idf\export.ps1' 2>&1 | Out-Null

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Output 'FAIL: idf.py not found after export.ps1'
    exit 2
}

Set-Location $proj
idf.py build *> "$proj\build_check.log"
exit $LASTEXITCODE
