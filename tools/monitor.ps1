[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Port
)

$ErrorActionPreference = "Stop"
$python = Join-Path $env:LOCALAPPDATA "Programs\Python\Python313\python.exe"
& $python -m serial.tools.miniterm $Port 115200 --exit-char 29
if ($LASTEXITCODE -ne 0) {
    throw "Serial monitor failed with exit code $LASTEXITCODE."
}
