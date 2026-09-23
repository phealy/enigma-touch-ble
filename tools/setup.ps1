[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$pythonDir = Join-Path $env:LOCALAPPDATA "Programs\Python\Python313"
$python = Join-Path $pythonDir "python.exe"

if (-not (Test-Path $python)) {
    winget install `
        --id Python.Python.3.13 `
        --exact `
        --scope user `
        --accept-package-agreements `
        --accept-source-agreements `
        --disable-interactivity
}

$env:Path = "$pythonDir;$(Join-Path $pythonDir 'Scripts');$env:Path"
& $python -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) {
    throw "pip upgrade failed with exit code $LASTEXITCODE."
}
& $python -m pip install -r (Join-Path $root "requirements.txt")
if ($LASTEXITCODE -ne 0) {
    throw "Python dependency installation failed with exit code $LASTEXITCODE."
}

$drive = $root.Substring(0, 1).ToLowerInvariant()
$rootLinux = "/mnt/$drive$($root.Substring(2).Replace('\', '/'))"
$command = "sed 's/\r$//' '$rootLinux/tools/setup_wsl.sh' | bash -s"
& wsl.exe -d Ubuntu-24.04 -- bash --noprofile --norc -c $command
if ($LASTEXITCODE -ne 0) {
    throw "WSL tool installation failed with exit code $LASTEXITCODE."
}

Write-Host "Environment ready."
Write-Host "Build with: .\tools\build.ps1"
