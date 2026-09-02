# go.ps1 - run idf.py without having to remember export.ps1
#
# idf.py is NOT a file in this directory. It lives at C:\esp\esp-idf\tools\idf.py,
# and export.ps1 defines it as a PowerShell function in whatever shell you source it
# into. That function vanishes when you close the terminal. This wrapper sets the
# environment up fresh each time, so you can just run:
#
#   .\go.ps1 build
#   .\go.ps1 -p COM5 -b 115200 flash
#   .\go.ps1 -p COM5 monitor          (Ctrl+] to quit the monitor)
#   .\go.ps1 fullclean
#
param([Parameter(ValueFromRemainingArguments = $true)] $Rest)

$idf = 'C:\esp\esp-idf\export.ps1'
if (-not (Test-Path $idf)) {
    Write-Error "ESP-IDF not found at $idf"
    exit 1
}

. $idf *> $null
Set-Location $PSScriptRoot
idf.py @Rest
exit $LASTEXITCODE
