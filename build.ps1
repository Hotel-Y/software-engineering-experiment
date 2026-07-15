$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $true
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"
$bin = Join-Path $root "bin"
$openssl = Join-Path $root "third_party\openssl\mingw64"
if (!(Test-Path (Join-Path $openssl "include\openssl\evp.h"))) {
    throw "OpenSSL development package is missing. Run .\setup_openssl.ps1 first."
}
New-Item -ItemType Directory -Force -Path $build, $bin | Out-Null

$sources = @(
    (Join-Path $root "src\main.cpp"),
    (Join-Path $root "src\BackupManager.cpp"),
    (Join-Path $root "src\ArchiveManager.cpp"),
    (Join-Path $root "src\Manifest.cpp"),
    (Join-Path $root "src\FileUtils.cpp")
)

g++ -std=c++17 -Wall -Wextra -pedantic -O2 `
    -I (Join-Path $root "include") `
    -I (Join-Path $openssl "include") `
    @sources `
    -L (Join-Path $openssl "lib") `
    -lcrypto -lws2_32 -lcrypt32 -lshell32 `
    -o (Join-Path $bin "sbm.exe")
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

Copy-Item -LiteralPath (Join-Path $openssl "bin\libcrypto-3-x64.dll") -Destination $bin -Force

Write-Host "Built: $(Join-Path $bin 'sbm.exe')"
