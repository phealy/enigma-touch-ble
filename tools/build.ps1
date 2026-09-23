[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$idfBuild = Join-Path $build "esp-idf"
$firmware = Join-Path $build "enigma_ble_firmware.bin"

New-Item -ItemType Directory -Path $build -Force | Out-Null
$idf = Get-Command idf.py -ErrorAction SilentlyContinue
if ($idf) {
    Push-Location $root
    try {
        & $idf.Source -B $idfBuild build
        if ($LASTEXITCODE -eq 0) {
            & $idf.Source -B $idfBuild merge-bin -o $firmware
        }
    } finally {
        Pop-Location
    }
} else {
    $drive = $root.Substring(0, 1).ToLowerInvariant()
    $rootLinux = "/mnt/$drive$($root.Substring(2).Replace('\', '/'))"
    $command = "sed 's/\r$//' '$rootLinux/tools/build_wsl.sh' | bash -s -- '$rootLinux'"
    & wsl.exe -d Ubuntu-24.04 -- bash --noprofile --norc -c $command
}
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $firmware)) {
    throw "Firmware build failed with exit code $LASTEXITCODE."
}

Write-Host "Built $firmware"
