[CmdletBinding()]
param(
    [string]$Name = "Enigma Touch BLE",
    [string]$Script,
    [double]$LineDelay = 0.1,
    [double]$FinalWait = 1.0
)

$python = Join-Path $env:LOCALAPPDATA "Programs\Python\Python313\python.exe"
$arguments = @(
    (Join-Path $PSScriptRoot "nus_terminal.py"),
    "--name", $Name,
    "--line-delay", $LineDelay,
    "--final-wait", $FinalWait
)
if ($Script) {
    $arguments += @("--script", $Script)
}
& $python @arguments
