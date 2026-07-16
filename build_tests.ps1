# Simple Backup Manager Unit Tests Build Script
$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $true
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"
$bin = Join-Path $root "bin"
$openssl = Join-Path $root "third_party\openssl\mingw64"
$msys2 = "D:\msys2\ucrt64"
if (!(Test-Path (Join-Path $openssl "include\openssl\evp.h"))) {
    throw "OpenSSL development package is missing. Run .\setup_openssl.ps1 first."
}
New-Item -ItemType Directory -Force -Path $build, $bin | Out-Null

$sources = @(
    (Join-Path $root "src\BackupManager.cpp"),
    (Join-Path $root "src\ArchiveManager.cpp"),
    (Join-Path $root "src\Manifest.cpp"),
    (Join-Path $root "src\FileUtils.cpp")
)

# All test source files
$testSources = @(
    (Join-Path $root "tests\test_main.cpp"),
    (Join-Path $root "tests\test_fileutils.cpp"),
    (Join-Path $root "tests\test_manifest.cpp"),
    (Join-Path $root "tests\gui_defaults_test.cpp")
)

# Use the MSYS2 UCRT64 g++ which has Catch2 installed
$gpp = Join-Path $msys2 "bin\g++.exe"

& $gpp -std=c++17 -Wall -Wextra -O2 `
    -I (Join-Path $root "include") `
    -I (Join-Path $openssl "include") `
    -I (Join-Path $msys2 "include") `
    @sources @testSources `
    -L (Join-Path $openssl "lib") `
    -lcrypto -lws2_32 -lcrypt32 -lshell32 `
    -o (Join-Path $bin "sbm_test.exe")
if ($LASTEXITCODE -ne 0) {
    throw "Test build failed."
}

Write-Host "Built: $(Join-Path $bin 'sbm_test.exe')"

# Run tests
Write-Host "`n=== Running unit tests ==="
& (Join-Path $bin "sbm_test.exe")
if ($LASTEXITCODE -ne 0) {
    throw "Some tests failed."
}
Write-Host "All unit tests passed."
