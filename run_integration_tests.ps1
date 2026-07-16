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

if (!$SkipBuild) {
    & (Join-Path $root "build.ps1")
}

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
