$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $true
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "build.ps1")

$exe = Join-Path $root "bin\sbm.exe"
$demo = Join-Path $root "demo_workspace"
if (Test-Path $demo) {
    Remove-Item -LiteralPath $demo -Recurse -Force
}

$source = Join-Path $demo "source"
$backup = Join-Path $demo "backup"
$archive = Join-Path $demo "secure_backup.sba"
$unpacked = Join-Path $demo "unpacked_backup"
$restore = Join-Path $demo "restore"
$snapshots = Join-Path $demo "snapshots"

New-Item -ItemType Directory -Force -Path (Join-Path $source "docs"), (Join-Path $source "src"), (Join-Path $source "empty") | Out-Null
Set-Content -LiteralPath (Join-Path $source "docs\report.txt") -Value "software backup report" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $source "docs\repeat.txt") -Value ("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") -Encoding UTF8
Set-Content -LiteralPath (Join-Path $source "src\main.cpp") -Value "int main() { return 0; }" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $source "video.bin") -Value "this file is intentionally filtered out" -Encoding UTF8

Write-Host "1. Backup with custom filters"
& $exe backup $source $backup --overwrite --ext=.txt,.cpp --max-size=2048

Write-Host "2. Verify backup"
& $exe verify $backup

Write-Host "3. Pack with RLE compression and OpenSSL AES-256-GCM encryption"
& $exe pack $backup $archive --compress=rle --password=secret123

Write-Host "4. Wrong password should fail"
$oldPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $exe unpack $archive (Join-Path $demo "wrong_password") --password=wrong 2>$null
$wrongExit = $LASTEXITCODE
$ErrorActionPreference = $oldPreference
if ($wrongExit -eq 0) {
    throw "Wrong password unexpectedly succeeded."
}
Write-Host "Wrong password failed as expected."

Write-Host "5. Unpack with correct password and restore"
& $exe unpack $archive $unpacked --password=secret123
& $exe verify $unpacked
& $exe restore $unpacked $restore

Write-Host "6. Scheduled backup with retention"
& $exe schedule $source $snapshots 0 3 --keep=2 --ext=.txt

Write-Host "Demo completed. Workspace:"
Write-Host $demo
