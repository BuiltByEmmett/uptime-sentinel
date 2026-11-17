# Runs PlatformIO from the project venv with every cache and temp folder
# kept on the same drive as the project. Usage: .\pio.ps1 run -t upload
$root = Split-Path -Parent $PSScriptRoot
$env:PLATFORMIO_CORE_DIR = Join-Path $root '.platformio'
$env:PIP_CACHE_DIR       = Join-Path $root '.pip-cache'
$env:TEMP                = Join-Path $root '.tmp'
$env:TMP                 = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
& (Join-Path $PSScriptRoot '.venv\Scripts\pio.exe') @args
exit $LASTEXITCODE
