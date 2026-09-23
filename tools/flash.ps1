[CmdletBinding()]
param(
    [string]$Port,
    [switch]$Erase
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$python = Join-Path $env:LOCALAPPDATA "Programs\Python\Python313\python.exe"
$firmware = Join-Path $root "build\enigma_ble_firmware.bin"
$idfBuild = Join-Path $root "build\esp-idf"

if (-not (Test-Path $firmware)) {
    throw "Firmware not found. Run .\tools\build.ps1 first."
}

if (-not $Port) {
    $candidates = @(
        Get-CimInstance Win32_SerialPort |
            Where-Object {
                $_.PNPDeviceID -match "VID_303A|VID_10C4|VID_1A86|VID_0403"
            }
    )
    if ($candidates.Count -ne 1) {
        $ports = Get-CimInstance Win32_SerialPort |
            ForEach-Object { "$($_.DeviceID): $($_.Name) [$($_.PNPDeviceID)]" }
        throw "Could not uniquely identify the ESP32 port. Pass -Port COMx.`n$($ports -join "`n")"
    }
    $Port = $candidates[0].DeviceID
}

if ($Erase) {
    & $python -m esptool --chip esp32s3 --port $Port erase-flash
    if ($LASTEXITCODE -ne 0) {
        throw "Flash erase failed with exit code $LASTEXITCODE."
    }
    & $python -m esptool `
        --chip esp32s3 `
        --port $Port `
        --baud 460800 `
        write-flash 0 `
        $firmware
} else {
    $bootloader = Join-Path $idfBuild "bootloader\bootloader.bin"
    $partitionTable = Join-Path $idfBuild "partition_table\partition-table.bin"
    $application = Join-Path $idfBuild "enigma_ble.bin"
    foreach ($image in @($bootloader, $partitionTable, $application)) {
        if (-not (Test-Path $image)) {
            throw "Build image not found: $image. Run .\tools\build.ps1 first."
        }
    }
    & $python -m esptool `
        --chip esp32s3 `
        --port $Port `
        --baud 460800 `
        write-flash `
        0x0 $bootloader `
        0x8000 $partitionTable `
        0x10000 $application
}
if ($LASTEXITCODE -ne 0) {
    throw "Firmware flash failed with exit code $LASTEXITCODE."
}

Write-Host "Flashed ESP-IDF firmware to $Port"
