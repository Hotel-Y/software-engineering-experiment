$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "build.ps1")

$exe = Join-Path $root "bin\sbm.exe"
$sandbox = Join-Path $root "tmp_test"
if (Test-Path $sandbox) {
    Remove-Item -LiteralPath $sandbox -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $sandbox | Out-Null

$source = Join-Path $sandbox "source"
$backup = Join-Path $sandbox "backup"
$restore = Join-Path $sandbox "restore"
New-Item -ItemType Directory -Force -Path (Join-Path $source "docs"), (Join-Path $source "src"), (Join-Path $source "empty") | Out-Null
Set-Content -LiteralPath (Join-Path $source "docs\a.txt") -Value "alpha" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $source "src\b.cpp") -Value "int main() { return 0; }" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $source "skip.bin") -Value "binary-like" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $source "docs\repeat.txt") -Value ("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") -Encoding UTF8
$knownTime = Get-Date "2024-01-02T03:04:05"
(Get-Item -LiteralPath (Join-Path $source "docs\a.txt")).LastWriteTime = $knownTime

& $exe backup $source $backup --ext=.txt,.cpp
& $exe verify $backup
& $exe restore $backup $restore

if (!(Test-Path (Join-Path $restore "docs\a.txt"))) {
    throw "Expected restored docs\a.txt"
}
if (!(Test-Path (Join-Path $restore "src\b.cpp"))) {
    throw "Expected restored src\b.cpp"
}
if (Test-Path (Join-Path $restore "skip.bin")) {
    throw "Extension filter failed"
}

$original = Get-Content -Raw -LiteralPath (Join-Path $source "docs\a.txt")
$restored = Get-Content -Raw -LiteralPath (Join-Path $restore "docs\a.txt")
if ($original -ne $restored) {
    throw "Restored file content mismatch"
}
$restoredTime = (Get-Item -LiteralPath (Join-Path $restore "docs\a.txt")).LastWriteTime
if ([Math]::Abs(($restoredTime - $knownTime).TotalSeconds) -gt 2) {
    throw "Restored modified time mismatch"
}

$archive = Join-Path $sandbox "backup.sba"
$unpackedBackup = Join-Path $sandbox "unpacked_backup"
$archiveRestore = Join-Path $sandbox "archive_restore"
& $exe pack $backup $archive
if (!(Test-Path $archive)) {
    throw "Expected archive file"
}
& $exe unpack $archive $unpackedBackup
& $exe verify $unpackedBackup
& $exe restore $unpackedBackup $archiveRestore
if (!(Test-Path (Join-Path $archiveRestore "docs\a.txt"))) {
    throw "Expected restored file from unpacked archive"
}

$secureArchive = Join-Path $sandbox "secure_backup.sba"
$secureUnpack = Join-Path $sandbox "secure_unpack"
$secureRestore = Join-Path $sandbox "secure_restore"
& $exe pack $backup $secureArchive --password=secret123
$ErrorActionPreference = "Continue"
& $exe unpack $secureArchive (Join-Path $sandbox "wrong_password_unpack") --password=wrong 2>$null
$wrongPasswordExit = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($wrongPasswordExit -eq 0) {
    throw "Wrong archive password should fail"
}
& $exe unpack $secureArchive $secureUnpack --password=secret123
& $exe verify $secureUnpack
& $exe restore $secureUnpack $secureRestore
if (!(Test-Path (Join-Path $secureRestore "docs\repeat.txt"))) {
    throw "Expected restored compressed/encrypted archive file"
}

$snapshotRoot = Join-Path $sandbox "snapshots"
& $exe schedule $source $snapshotRoot 0 3 --keep=2 --ext=.txt
$snapshotCount = @(Get-ChildItem -LiteralPath $snapshotRoot -Directory).Count
if ($snapshotCount -ne 2) {
    throw "Expected two retained scheduled backup snapshots"
}

$emptyRestore = Join-Path $restore "empty"
if (!(Test-Path $emptyRestore)) {
    throw "Expected restored empty directory"
}

$insideBackup = Join-Path $source "nested_backup"
$ErrorActionPreference = "Continue"
& $exe backup $source $insideBackup 2>$null
$ErrorActionPreference = "Stop"
if ($LASTEXITCODE -eq 0) {
    throw "Backup inside source should fail"
}

$sizeBackup = Join-Path $sandbox "size_backup"
& $exe backup $source $sizeBackup --max-size=12
if (!(Test-Path (Join-Path $sizeBackup "docs\a.txt"))) {
    throw "Expected small file in size-filtered backup"
}
if (Test-Path (Join-Path $sizeBackup "src\b.cpp")) {
    throw "Max-size filter failed"
}

$nameBackup = Join-Path $sandbox "name_backup"
& $exe backup $source $nameBackup --name-contains=a
if (!(Test-Path (Join-Path $nameBackup "docs\a.txt"))) {
    throw "Expected name-filtered a.txt"
}
if (Test-Path (Join-Path $nameBackup "src\b.cpp")) {
    throw "Name filter failed"
}

$pathBackup = Join-Path $sandbox "path_backup"
& $exe backup $source $pathBackup --path-contains=src
if (!(Test-Path (Join-Path $pathBackup "src\b.cpp"))) {
    throw "Expected path-filtered src\b.cpp"
}
if (Test-Path (Join-Path $pathBackup "docs\a.txt")) {
    throw "Path filter failed"
}

$timeBackup = Join-Path $sandbox "time_backup"
& $exe backup $source $timeBackup --modified-before=2024-12-31
if (!(Test-Path (Join-Path $timeBackup "docs\a.txt"))) {
    throw "Expected time-filtered docs\a.txt"
}
if (Test-Path (Join-Path $timeBackup "docs\repeat.txt")) {
    throw "Modified time filter failed"
}

Set-Content -LiteralPath (Join-Path $backup "docs\a.txt") -Value "corrupted" -Encoding UTF8
$ErrorActionPreference = "Continue"
& $exe verify $backup 2>$null
$ErrorActionPreference = "Stop"
if ($LASTEXITCODE -eq 0) {
    throw "Verify should fail after backup corruption"
}

Write-Host "All tests passed."
