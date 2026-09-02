# go.ps1 - run idf.py without remembering export.ps1
#
# idf.py is NOT a file in this directory. It lives at C:\esp\esp-idf\tools\idf.py,
# and export.ps1 defines it as a PowerShell function inside whichever shell you
# dot-source it into - that function disappears when the terminal closes. This
# wrapper rebuilds the environment each run, so you can just do:
#
#   .\go.ps1 build
#   .\go.ps1 -p COM5 -b 115200 flash
#   .\go.ps1 -p COM5 monitor          (Ctrl+] quits the monitor)
#   .\go.ps1 size-components
#   .\go.ps1 fullclean
#
param([Parameter(ValueFromRemainingArguments = $true)] $Rest)

$idf = 'C:\esp\esp-idf\export.ps1'
if (-not (Test-Path $idf)) { Write-Error "ESP-IDF not found at $idf"; exit 1 }

. $idf *> $null
Set-Location $PSScriptRoot
idf.py @Rest
exit $LASTEXITCODE
