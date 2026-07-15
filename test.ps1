$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Invoke-ExpectFailure {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $Executable @Arguments 2>$null
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($exitCode -eq 0) {
        throw $Message
    }
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "build.ps1")
& (Join-Path $root "build_gui.ps1")

$guiDefaultsTest = Join-Path $root "bin\gui_defaults_test.exe"
g++ -std=c++17 -Wall -Wextra -pedantic -O2 `
    -I (Join-Path $root "include") `
    (Join-Path $root "tests\gui_defaults_test.cpp") `
    -o $guiDefaultsTest
if ($LASTEXITCODE -ne 0) {
    throw "GUI smart-default test build failed."
}
& $guiDefaultsTest
if ($LASTEXITCODE -ne 0) {
    throw "GUI smart-default tests failed."
}

$exe = Join-Path $root "bin\sbm.exe"
$sandbox = Join-Path $root "tmp_test"
if (Test-Path $sandbox) {
    Remove-Item -LiteralPath $sandbox -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $sandbox | Out-Null

$source = Join-Path $sandbox "source"
$backup = Join-Path $sandbox "backup"
$restore = Join-Path $sandbox "restore"
New-Item -ItemType Directory -Force -Path (Join-Path $source "docs"), (Join-Path $source "src"), (Join-Path $source "empty"), (Join-Path $source "中文目录") | Out-Null
Set-Content -LiteralPath (Join-Path $source "docs\a.txt") -Value "alpha" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $source "src\b.cpp") -Value "int main() { return 0; }" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $source "中文目录\说明.txt") -Value "中文路径回归测试" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $source "skip.bin") -Value "binary-like" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $source "docs\repeat.txt") -Value ("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") -Encoding UTF8
$knownTime = Get-Date "2024-01-02T03:04:05"
(Get-Item -LiteralPath (Join-Path $source "docs\a.txt")).LastWriteTime = $knownTime

& $exe backup $source $backup --ext=.txt,.cpp
Invoke-ExpectFailure $exe @("backup", $source, $backup) `
    "Existing backup should require --overwrite"
& $exe verify $backup
& $exe restore $backup $restore

if (!(Test-Path (Join-Path $restore "docs\a.txt"))) {
    throw "Expected restored docs\a.txt"
}
if (!(Test-Path (Join-Path $restore "src\b.cpp"))) {
    throw "Expected restored src\b.cpp"
}
if (!(Test-Path (Join-Path $restore "中文目录\说明.txt"))) {
    throw "Expected restored Unicode path"
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
Invoke-ExpectFailure $exe @("pack", $backup, (Join-Path $backup "unsafe.sba")) `
    "Archive output inside backup should fail"
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
$wrongPasswordOutput = Join-Path $sandbox "wrong_password_unpack"
Invoke-ExpectFailure $exe @("unpack", $secureArchive, $wrongPasswordOutput, "--password=wrong") `
    "Wrong archive password should fail"
if (Test-Path $wrongPasswordOutput) {
    throw "Failed encrypted unpack should not leave partial output"
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
Invoke-ExpectFailure $exe @("backup", $source, $insideBackup) `
    "Backup inside source should fail"

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
Invoke-ExpectFailure $exe @("verify", $backup) `
    "Verify should fail after backup corruption"

$compressionSource = Join-Path $sandbox "compression_source"
$compressionBackup = Join-Path $sandbox "compression_backup"
$compressionArchive = Join-Path $sandbox "compression.sba"
$compressionUnpack = Join-Path $sandbox "compression_unpack"
New-Item -ItemType Directory -Force -Path $compressionSource | Out-Null
$repeatPath = Join-Path $compressionSource "repeat-large.bin"
$randomPath = Join-Path $compressionSource "random.bin"
[IO.File]::WriteAllText($repeatPath, ("ABCD0123456789" * 160000), [Text.UTF8Encoding]::new($false))
$randomBytes = New-Object byte[] (1024 * 1024)
[Random]::new(20260713).NextBytes($randomBytes)
[IO.File]::WriteAllBytes($randomPath, $randomBytes)
& $exe backup $compressionSource $compressionBackup
& $exe pack $compressionBackup $compressionArchive
$inspection = & python (Join-Path $root "sba_inspect.py") $compressionArchive
if ($LASTEXITCODE -ne 0 -or ($inspection -join "`n") -notmatch "pipeline round-trip.*all OK") {
    throw "Archive inspector did not validate the compression pipeline"
}
if (($inspection -join "`n") -notmatch "raw-stored") {
    throw "Expected incompressible data to use raw-store bypass"
}
& $exe unpack $compressionArchive $compressionUnpack
foreach ($name in @("repeat-large.bin", "random.bin")) {
    $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $compressionSource $name)).Hash
    $unpackedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $compressionUnpack $name)).Hash
    if ($sourceHash -ne $unpackedHash) {
        throw "Compression round trip failed for $name"
    }
}

$nonEmptyOutput = Join-Path $sandbox "nonempty_unpack"
New-Item -ItemType Directory -Force -Path $nonEmptyOutput | Out-Null
$sentinel = Join-Path $nonEmptyOutput "keep.txt"
Set-Content -LiteralPath $sentinel -Value "keep" -Encoding UTF8
Invoke-ExpectFailure $exe @("unpack", $compressionArchive, $nonEmptyOutput) `
    "Unpack into a non-empty directory should fail"
if ((Get-Content -Raw -LiteralPath $sentinel) -notmatch "keep") {
    throw "Rejected unpack modified the existing output directory"
}

$trailingArchive = Join-Path $sandbox "trailing-data.sba"
Copy-Item -LiteralPath $compressionArchive -Destination $trailingArchive
$append = [IO.File]::Open($trailingArchive, [IO.FileMode]::Append, [IO.FileAccess]::Write)
try {
    $append.WriteByte(0x7f)
}
finally {
    $append.Dispose()
}
$trailingOutput = Join-Path $sandbox "trailing-output"
Invoke-ExpectFailure $exe @("unpack", $trailingArchive, $trailingOutput) `
    "Archive trailing data should be rejected"
if (Test-Path $trailingOutput) {
    throw "Rejected archive should not leave partial output"
}

$missingDirectoryBackup = Join-Path $sandbox "missing-directory-backup"
& $exe backup $source $missingDirectoryBackup --ext=.txt,.cpp
Remove-Item -LiteralPath (Join-Path $missingDirectoryBackup "empty") -Force
Invoke-ExpectFailure $exe @("verify", $missingDirectoryBackup) `
    "Verify should fail when a manifest directory is missing"

$escapeSource = Join-Path $sandbox "escape-source.txt"
$maliciousBackup = Join-Path $sandbox "malicious-backup"
$maliciousRestore = Join-Path $sandbox "malicious-restore"
Set-Content -LiteralPath $escapeSource -Value "must remain unchanged" -Encoding UTF8
New-Item -ItemType Directory -Force -Path $maliciousBackup | Out-Null
Set-Content -LiteralPath (Join-Path $maliciousBackup "manifest.sbm") `
    -Value "SBM1`nF|..\escape-source.txt|1|0|0" -Encoding ASCII
Invoke-ExpectFailure $exe @("restore", $maliciousBackup, $maliciousRestore) `
    "Manifest path traversal should be rejected"
if ((Get-Content -Raw -LiteralPath $escapeSource) -notmatch "must remain unchanged") {
    throw "Manifest path traversal modified a file outside the restore root"
}

Invoke-ExpectFailure $exe @("backup", $source, (Join-Path $sandbox "bad-size"), "--max-size=-1") `
    "Negative --max-size should be rejected"
Invoke-ExpectFailure $exe @("backup", $source, (Join-Path $sandbox "bad-date"), "--modified-before=2024-02-30") `
    "Invalid calendar date should be rejected"
Invoke-ExpectFailure $exe @("pack", $compressionBackup, (Join-Path $sandbox "empty-password.sba"), "--password=") `
    "Empty archive password should be rejected"
Invoke-ExpectFailure $exe @("schedule", $source, (Join-Path $sandbox "bad-schedule"), "0", "1x") `
    "Non-numeric schedule count should be rejected"

Write-Host "All tests passed."
exit 0
