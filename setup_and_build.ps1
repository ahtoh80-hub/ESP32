# Скрипт сборки: активирует окружение EIM (C:\Espressif) и запускает idf.py build.
# Запускается отсоединённо; статус — в build_check.log / build_exit.txt.
$ErrorActionPreference = 'Continue'
$proj = $PSScriptRoot

try {
    # Официальная активация этой установки ESP-IDF v6.0.2:
    # задаёт IDF_PATH, IDF_TOOLS_PATH, PATH с тулчейнами и алиас idf.py. [C:\Espressif\tools]
    . 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1' 2>&1 | Out-Null
} catch {
    # Ошибки профиля (например, незаданные функции автодополнения) не критичны.
}

$py = 'C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe'
$idfPy = 'C:\esp\v6.0.2\esp-idf\tools\idf.py'

if (-not (Test-Path $py) -or -not (Test-Path $idfPy)) {
    'FAIL: python or idf.py not found' | Out-File "$proj\build_check.log" -Encoding utf8
    '2' | Out-File "$proj\build_exit.txt"
    exit 2
}

Set-Location $proj
& $py $idfPy build *> "$proj\build_check.log"
"$LASTEXITCODE" | Out-File "$proj\build_exit.txt"
exit $LASTEXITCODE
