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

if (!$SkipBuild) {
    & (Join-Path $root "build.ps1")
}

if (!(Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found: $executable"
}
if (!(Test-Path -LiteralPath $suite -PathType Leaf)) {
    throw "Stress test suite not found: $suite"
}

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
