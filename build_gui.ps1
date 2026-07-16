# 单独构建轻量 Win32 图形界面；核心业务仍由同目录下的 sbm.exe 执行。
$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $true
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $root "bin"
New-Item -ItemType Directory -Force -Path $bin | Out-Null

# -mwindows 隐藏控制台窗口，后面的系统库提供窗口、字体和通用控件支持。
g++ -std=c++17 -Wall -Wextra -pedantic -O2 -mwindows `
    -I (Join-Path $root "include") `
    (Join-Path $root "src\gui_win.cpp") `
    -o (Join-Path $bin "sbm_gui.exe") `
    -luser32 -lgdi32 -lcomctl32 -lole32
if ($LASTEXITCODE -ne 0) {
    throw "GUI build failed."
}

Write-Host "Built: $(Join-Path $bin 'sbm_gui.exe')"
