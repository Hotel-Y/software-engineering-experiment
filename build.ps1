# 构建核心命令行程序：定位源码与本地 OpenSSL，调用 MinGW g++，最后复制运行时 DLL。
$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $true
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"
$bin = Join-Path $root "bin"
$openssl = Join-Path $root "third_party\openssl\mingw64"
# 在编译前检查头文件，可把“依赖未安装”与普通编译错误清楚地区分开。
if (!(Test-Path (Join-Path $openssl "include\openssl\evp.h"))) {
    throw "OpenSSL development package is missing. Run .\setup_openssl.ps1 first."
}
New-Item -ItemType Directory -Force -Path $build, $bin | Out-Null

# 核心程序的五个翻译单元在此集中列出，避免遗漏业务模块。
$sources = @(
    (Join-Path $root "src\main.cpp"),
    (Join-Path $root "src\BackupManager.cpp"),
    (Join-Path $root "src\ArchiveManager.cpp"),
    (Join-Path $root "src\Manifest.cpp"),
    (Join-Path $root "src\FileUtils.cpp")
)

# 使用 C++17、优化和严格警告，并链接 OpenSSL 及 Windows 所需系统库。
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

# 可执行文件运行时需要 libcrypto，因此与 sbm.exe 一起放入 bin。
Copy-Item -LiteralPath (Join-Path $openssl "bin\libcrypto-3-x64.dll") -Destination $bin -Force

Write-Host "Built: $(Join-Path $bin 'sbm.exe')"
