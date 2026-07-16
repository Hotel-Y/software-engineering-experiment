# CLI 集成测试总入口：可选择先构建 sbm.exe，再运行 30 个跨进程文件系统用例。
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $true
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$executable = Join-Path $root "bin\sbm.exe"
$suite = Join-Path $root "tests\integration_cli_test.ps1"

# 默认先构建，-SkipBuild 仅用于确认 bin\sbm.exe 已是目标版本的快速复测。
if (!$SkipBuild) {
    & (Join-Path $root "build.ps1")
}

# 在进入套件前分别检查被测程序和测试脚本，错误信息能明确指出缺少哪一项。
if (!(Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found: $executable. Run without -SkipBuild first."
}
if (!(Test-Path -LiteralPath $suite -PathType Leaf)) {
    throw "Integration test suite not found: $suite"
}

Write-Host "=== Running CLI integration tests (BC-01 through BC-30) ==="
& $suite -Executable $executable
if ($LASTEXITCODE -ne 0) {
    throw "Integration tests failed."
}

Write-Host "All 30 CLI integration tests passed."
