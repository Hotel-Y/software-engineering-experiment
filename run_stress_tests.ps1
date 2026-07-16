# 压力测试总入口：选择 Quick/Full 规模，可限定用例并决定是否保留测试现场。
param(
    [ValidateSet("Quick", "Full")]
    [string]$Profile = "Quick",

    [switch]$SkipBuild,

    [string[]]$Cases = @(),

    [switch]$KeepWork
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $true
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$executable = Join-Path $root "bin\sbm.exe"
$suite = Join-Path $root "tests\stress_test.ps1"

# 与集成测试一致，默认先重新构建，防止对旧二进制做性能结论。
if (!$SkipBuild) {
    & (Join-Path $root "build.ps1")
}

if (!(Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found: $executable"
}
if (!(Test-Path -LiteralPath $suite -PathType Leaf)) {
    throw "Stress test suite not found: $suite"
}

# 只把用户实际指定的开关传给套件，保持 stress_test.ps1 的默认行为。
$arguments = @{
    Executable = $executable
    Profile = $Profile
}
if ($KeepWork) {
    $arguments.KeepWork = $true
}
if ($Cases.Count -gt 0) {
    $arguments.Cases = $Cases
}

& $suite @arguments
if ($LASTEXITCODE -ne 0) {
    throw "$Profile stress tests failed."
}

Write-Host "$Profile stress test run completed successfully."
