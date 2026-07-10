$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $true
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $root "bin"
New-Item -ItemType Directory -Force -Path $bin | Out-Null

g++ -std=c++17 -Wall -Wextra -pedantic -O2 -mwindows `
    (Join-Path $root "src\gui_win.cpp") `
    -o (Join-Path $bin "sbm_gui.exe") `
    -luser32 -lgdi32
if ($LASTEXITCODE -ne 0) {
    throw "GUI build failed."
}

Write-Host "Built: $(Join-Path $bin 'sbm_gui.exe')"
