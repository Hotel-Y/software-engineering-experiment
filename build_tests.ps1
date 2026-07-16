# Catch2 单元测试构建入口：编译业务模块与测试源码，并在构建成功后立即运行测试。
$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $true
}

# 单元测试使用安装了 Catch2 的 MSYS2 UCRT64 编译器，OpenSSL 仍复用项目本地依赖。
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"
$bin = Join-Path $root "bin"
$openssl = Join-Path $root "third_party\openssl\mingw64"
$msys2 = "D:\msys2\ucrt64"
if (!(Test-Path (Join-Path $openssl "include\openssl\evp.h"))) {
    throw "OpenSSL development package is missing. Run .\setup_openssl.ps1 first."
}
New-Item -ItemType Directory -Force -Path $build, $bin | Out-Null

# 业务源码不包含 CLI main.cpp，测试框架会提供自己的程序入口。
$sources = @(
    (Join-Path $root "src\BackupManager.cpp"),
    (Join-Path $root "src\ArchiveManager.cpp"),
    (Join-Path $root "src\Manifest.cpp"),
    (Join-Path $root "src\FileUtils.cpp")
)

# 汇总所有测试翻译单元；新增单元测试文件时需要同步加入此列表。
$testSources = @(
    (Join-Path $root "tests\test_main.cpp"),
    (Join-Path $root "tests\test_fileutils.cpp"),
    (Join-Path $root "tests\test_manifest.cpp"),
    (Join-Path $root "tests\gui_defaults_test.cpp")
)

# 选择含 Catch2 头文件和运行库的 UCRT64 g++。
$gpp = Join-Path $msys2 "bin\g++.exe"

# 使用与正式程序相同的 C++17、业务头文件和 OpenSSL 链接参数。
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

# 构建完成后直接运行测试可执行文件，非零退出码视为整套单元测试失败。
Write-Host "`n=== Running unit tests ==="
& (Join-Path $bin "sbm_test.exe")
if ($LASTEXITCODE -ne 0) {
    throw "Some tests failed."
}
Write-Host "All unit tests passed."
