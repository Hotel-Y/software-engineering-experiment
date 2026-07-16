# CLI 集成测试套件：每个编号用例都调用真实 sbm.exe，跨越进程、文件系统、清单、
# 归档与还原边界验证结果，而不是直接调用 C++ 内部函数。
param(
    [Parameter(Mandatory = $true)][string]$Executable
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

# 每次运行使用唯一临时目录，并集中保存通过数、失败详情及最近一次命令输出。
$script:Executable = (Resolve-Path -LiteralPath $Executable).Path
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$suiteRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ("sbm_broad_{0}_{1}" -f $PID, [Guid]::NewGuid().ToString("N").Substring(0, 8))
$script:Passed = 0
$script:Total = 0
$script:Failures = [System.Collections.Generic.List[string]]::new()
$script:LastOutput = ""

if (Test-Path -LiteralPath $suiteRoot) {
    Remove-Item -LiteralPath $suiteRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $suiteRoot | Out-Null

function Invoke-Sbm {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$ExpectFailure
    )

    # 统一捕获 stdout/stderr 与退出码；负向用例由 ExpectFailure 明确声明。
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $script:Executable @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    $script:LastOutput = $output -join "`n"

    if ($ExpectFailure) {
        if ($exitCode -eq 0) {
            throw "Command should fail: $($Arguments -join ' ')"
        }
    }
    elseif ($exitCode -ne 0) {
        throw "Command failed ($exitCode): $($Arguments -join ' ')`n$script:LastOutput"
    }
}

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (!$Condition) {
        throw $Message
    }
}

function Assert-SameFile {
    param(
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Actual
    )
    # 使用 SHA-256 对比完整内容，适用于文本、二进制和零字节文件。
    $expectedHash = (Get-FileHash -LiteralPath $Expected -Algorithm SHA256).Hash
    $actualHash = (Get-FileHash -LiteralPath $Actual -Algorithm SHA256).Hash
    Assert-True ($expectedHash -eq $actualHash) "File hashes differ: $Expected vs $Actual"
}

function Invoke-BroadCase {
    param(
        [Parameter(Mandatory = $true)][string]$Id,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    # 每个用例在独立子目录执行；异常被记录后继续跑后续用例，最终一次性汇总。
    ++$script:Total
    $caseRoot = Join-Path $suiteRoot $Id
    New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
    try {
        & $Body $caseRoot
        ++$script:Passed
        Write-Host "[$Id] PASS - $Name"
    }
    catch {
        $detail = "[$Id] FAIL - $Name`: $($_.Exception.Message)"
        $script:Failures.Add($detail)
        Write-Host $detail -ForegroundColor Red
    }
}

# ---- 基础路径与文件类型：BC-01～BC-07 -----------------------------------
Invoke-BroadCase "BC-01" "reject a missing source directory" {
    param($caseRoot)
    Invoke-Sbm @("backup", (Join-Path $caseRoot "missing"), (Join-Path $caseRoot "backup")) -ExpectFailure
}

Invoke-BroadCase "BC-02" "backup and verify an empty source" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("verify", $backup)
    Assert-True (Test-Path -LiteralPath (Join-Path $backup "manifest.sbm")) "Empty backup manifest is missing"
}

Invoke-BroadCase "BC-03" "round-trip a zero-byte file" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $restore = Join-Path $caseRoot "restore"
    New-Item -ItemType Directory -Path $source | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $source "empty.bin"), [byte[]]@())
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("restore", $backup, $restore)
    Assert-True ((Get-Item -LiteralPath (Join-Path $restore "empty.bin")).Length -eq 0) "Zero-byte file changed size"
}

Invoke-BroadCase "BC-04" "round-trip all byte values" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $restore = Join-Path $caseRoot "restore"
    New-Item -ItemType Directory -Path $source | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $source "bytes.bin"), [byte[]](0..255))
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("restore", $backup, $restore)
    Assert-SameFile (Join-Path $source "bytes.bin") (Join-Path $restore "bytes.bin")
}

Invoke-BroadCase "BC-05" "round-trip Unicode directories and file names" {
    param($caseRoot)
    $sourceName = -join @([char]0x6E90, [char]0x76EE, [char]0x5F55)
    $backupName = -join @([char]0x5907, [char]0x4EFD, [char]0x76EE, [char]0x5F55)
    $restoreName = -join @([char]0x8FD8, [char]0x539F, [char]0x76EE, [char]0x5F55)
    $directoryName = -join @([char]0x4E2D, [char]0x6587, [char]0x5B50, [char]0x76EE, [char]0x5F55)
    $fileName = (-join @([char]0x8BF4, [char]0x660E, [char]0x6587, [char]0x4EF6)) + ".txt"
    $content = (-join @([char]0x4E2D, [char]0x6587, [char]0x5185, [char]0x5BB9)) + "-ok"
    $source = Join-Path $caseRoot $sourceName
    $backup = Join-Path $caseRoot $backupName
    $restore = Join-Path $caseRoot $restoreName
    $relativeFile = Join-Path $directoryName $fileName
    New-Item -ItemType Directory -Path (Join-Path $source $directoryName) -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $source $relativeFile) -Value $content -Encoding UTF8
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("restore", $backup, $restore)
    Assert-SameFile (Join-Path $source $relativeFile) (Join-Path $restore $relativeFile)
}

Invoke-BroadCase "BC-06" "round-trip a deeply nested path" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $restore = Join-Path $caseRoot "restore"
    $deep = $source
    foreach ($index in 1..14) { $deep = Join-Path $deep ("level_{0:D2}" -f $index) }
    New-Item -ItemType Directory -Path $deep -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $deep "deep.txt") -Value "deep" -Encoding UTF8
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("restore", $backup, $restore)
    $relativeDirectory = $deep.Substring($source.Length).TrimStart(
        [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
    $relative = Join-Path $relativeDirectory "deep.txt"
    Assert-True (Test-Path -LiteralPath (Join-Path $restore $relative)) "Deep file was not restored"
}

Invoke-BroadCase "BC-07" "round-trip many small files" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $restore = Join-Path $caseRoot "restore"
    New-Item -ItemType Directory -Path $source | Out-Null
    foreach ($index in 1..120) {
        Set-Content -LiteralPath (Join-Path $source ("file_{0:D3}.txt" -f $index)) -Value "value-$index" -Encoding UTF8
    }
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("restore", $backup, $restore)
    Assert-True (@(Get-ChildItem -LiteralPath $restore -File).Count -eq 120) "Many-file restore count mismatch"
}

# ---- 过滤规则与边界：BC-08～BC-15 ---------------------------------------
Invoke-BroadCase "BC-08" "match extensions case-insensitively" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content -LiteralPath (Join-Path $source "UPPER.TXT") -Value "text" -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $source "skip.bin") -Value "binary" -Encoding UTF8
    Invoke-Sbm @("backup", $source, $backup, "--ext=txt")
    Assert-True (Test-Path -LiteralPath (Join-Path $backup "UPPER.TXT")) "Upper-case extension did not match"
    Assert-True (!(Test-Path -LiteralPath (Join-Path $backup "skip.bin"))) "Extension filter included a non-match"
}

Invoke-BroadCase "BC-09" "support multiple extension filters" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "a.txt") "a"
    Set-Content (Join-Path $source "b.cpp") "b"
    Set-Content (Join-Path $source "c.bin") "c"
    Invoke-Sbm @("backup", $source, $backup, "--ext=.txt,.cpp")
    Assert-True (Test-Path (Join-Path $backup "a.txt")) "TXT filter match missing"
    Assert-True (Test-Path (Join-Path $backup "b.cpp")) "CPP filter match missing"
    Assert-True (!(Test-Path (Join-Path $backup "c.bin"))) "Multiple-extension filter included BIN"
}

Invoke-BroadCase "BC-10" "include a file exactly at max-size" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $source "exact.bin"), [Text.Encoding]::ASCII.GetBytes("123456789012"))
    Invoke-Sbm @("backup", $source, $backup, "--max-size=12")
    Assert-True (Test-Path (Join-Path $backup "exact.bin")) "Boundary-sized file was excluded"
}

Invoke-BroadCase "BC-11" "exclude a file above max-size" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $source "large.bin"), [Text.Encoding]::ASCII.GetBytes("1234567890123"))
    Invoke-Sbm @("backup", $source, $backup, "--max-size=12")
    Assert-True (!(Test-Path (Join-Path $backup "large.bin"))) "Oversized file was included"
}

Invoke-BroadCase "BC-12" "filter by file-name substring" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "report-final.txt") "report"
    Set-Content (Join-Path $source "notes.txt") "notes"
    Invoke-Sbm @("backup", $source, $backup, "--name-contains=report")
    Assert-True (Test-Path (Join-Path $backup "report-final.txt")) "Name filter match missing"
    Assert-True (!(Test-Path (Join-Path $backup "notes.txt"))) "Name filter included non-match"
}

Invoke-BroadCase "BC-13" "filter by relative-path substring" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path (Join-Path $source "docs"),(Join-Path $source "src") -Force | Out-Null
    Set-Content (Join-Path $source "docs\a.txt") "a"
    Set-Content (Join-Path $source "src\b.txt") "b"
    Invoke-Sbm @("backup", $source, $backup, "--path-contains=docs")
    Assert-True (Test-Path (Join-Path $backup "docs\a.txt")) "Path filter match missing"
    Assert-True (!(Test-Path (Join-Path $backup "src\b.txt"))) "Path filter included non-match"
}

Invoke-BroadCase "BC-14" "filter files modified after a date" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "old.txt") "old"
    Set-Content (Join-Path $source "new.txt") "new"
    (Get-Item (Join-Path $source "old.txt")).LastWriteTime = (Get-Date "2020-01-02T12:00:00")
    (Get-Item (Join-Path $source "new.txt")).LastWriteTime = (Get-Date "2026-01-02T12:00:00")
    Invoke-Sbm @("backup", $source, $backup, "--modified-after=2025-01-01")
    Assert-True (!(Test-Path (Join-Path $backup "old.txt"))) "Modified-after included an old file"
    Assert-True (Test-Path (Join-Path $backup "new.txt")) "Modified-after excluded a new file"
}

Invoke-BroadCase "BC-15" "filter files modified before a date" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "old.txt") "old"
    Set-Content (Join-Path $source "new.txt") "new"
    (Get-Item (Join-Path $source "old.txt")).LastWriteTime = (Get-Date "2020-01-02T12:00:00")
    (Get-Item (Join-Path $source "new.txt")).LastWriteTime = (Get-Date "2026-01-02T12:00:00")
    Invoke-Sbm @("backup", $source, $backup, "--modified-before=2021-01-01")
    Assert-True (Test-Path (Join-Path $backup "old.txt")) "Modified-before excluded an old file"
    Assert-True (!(Test-Path (Join-Path $backup "new.txt"))) "Modified-before included a new file"
}

# ---- 覆盖策略与目录位置：BC-16～BC-18 -----------------------------------
Invoke-BroadCase "BC-16" "require overwrite for an existing backup" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "a.txt") "a"
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("backup", $source, $backup) -ExpectFailure
}

Invoke-BroadCase "BC-17" "overwrite replaces stale backup contents" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "value.txt") "version-one"
    Set-Content (Join-Path $source "obsolete.txt") "remove-me"
    Invoke-Sbm @("backup", $source, $backup)
    Set-Content (Join-Path $source "value.txt") "version-two"
    Remove-Item -LiteralPath (Join-Path $source "obsolete.txt")
    Invoke-Sbm @("backup", $source, $backup, "--overwrite")
    Assert-True ((Get-Content -Raw (Join-Path $backup "value.txt")) -match "version-two") "Overwrite kept stale content"
    Assert-True (!(Test-Path (Join-Path $backup "obsolete.txt"))) "Overwrite kept a removed file"
}

Invoke-BroadCase "BC-18" "reject a backup destination inside the source" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "a.txt") "a"
    Invoke-Sbm @("backup", $source, (Join-Path $source "nested-backup")) -ExpectFailure
}

# ---- 清单完整性与路径安全：BC-19～BC-23 ---------------------------------
Invoke-BroadCase "BC-19" "detect a missing backup file" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "a.txt") "a"
    Invoke-Sbm @("backup", $source, $backup)
    Remove-Item -LiteralPath (Join-Path $backup "a.txt")
    Invoke-Sbm @("verify", $backup) -ExpectFailure
}

Invoke-BroadCase "BC-20" "detect changed backup contents" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "a.txt") "original"
    Invoke-Sbm @("backup", $source, $backup)
    Set-Content (Join-Path $backup "a.txt") "changed"
    Invoke-Sbm @("verify", $backup) -ExpectFailure
}

Invoke-BroadCase "BC-21" "reject an invalid manifest header" {
    param($caseRoot)
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $backup | Out-Null
    Set-Content -LiteralPath (Join-Path $backup "manifest.sbm") -Value "BAD1" -Encoding ASCII
    Invoke-Sbm @("verify", $backup) -ExpectFailure
}

Invoke-BroadCase "BC-22" "reject duplicate manifest paths" {
    param($caseRoot)
    $backup = Join-Path $caseRoot "backup"
    New-Item -ItemType Directory -Path $backup | Out-Null
    Set-Content -LiteralPath (Join-Path $backup "manifest.sbm") `
        -Value "SBM1`nF|same.txt|1|0|1`nF|same.txt|1|0|1" -Encoding ASCII
    Invoke-Sbm @("verify", $backup) -ExpectFailure
}

Invoke-BroadCase "BC-23" "reject manifest path traversal" {
    param($caseRoot)
    $backup = Join-Path $caseRoot "backup"
    $restore = Join-Path $caseRoot "restore"
    $outside = Join-Path $caseRoot "outside.txt"
    New-Item -ItemType Directory -Path $backup | Out-Null
    Set-Content -LiteralPath $outside -Value "unchanged" -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $backup "manifest.sbm") `
        -Value "SBM1`nF|..\outside.txt|1|0|0" -Encoding ASCII
    Invoke-Sbm @("restore", $backup, $restore) -ExpectFailure
    Assert-True ((Get-Content -Raw -LiteralPath $outside) -match "unchanged") "Traversal modified an outside file"
}

# ---- 归档、加密及事务性失败：BC-24～BC-29 -------------------------------
Invoke-BroadCase "BC-24" "round-trip a normal archive" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $archive = Join-Path $caseRoot "backup.sba"
    $unpack = Join-Path $caseRoot "unpack"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "a.txt") ("archive-data" * 100)
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("pack", $backup, $archive)
    Invoke-Sbm @("unpack", $archive, $unpack)
    Invoke-Sbm @("verify", $unpack)
    Assert-SameFile (Join-Path $backup "a.txt") (Join-Path $unpack "a.txt")
}

Invoke-BroadCase "BC-25" "round-trip an encrypted archive" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $archive = Join-Path $caseRoot "secure.sba"
    $unpack = Join-Path $caseRoot "unpack"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "secret.txt") "encrypted-content"
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("pack", $backup, $archive, "--password=wide-coverage")
    Invoke-Sbm @("unpack", $archive, $unpack, "--password=wide-coverage")
    Invoke-Sbm @("verify", $unpack)
    Assert-SameFile (Join-Path $backup "secret.txt") (Join-Path $unpack "secret.txt")
}

Invoke-BroadCase "BC-26" "reject a wrong password without partial output" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $archive = Join-Path $caseRoot "secure.sba"
    $unpack = Join-Path $caseRoot "wrong-output"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "secret.txt") "secret"
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("pack", $backup, $archive, "--password=right")
    Invoke-Sbm @("unpack", $archive, $unpack, "--password=wrong") -ExpectFailure
    Assert-True (!(Test-Path -LiteralPath $unpack)) "Wrong-password unpack left partial output"
}

Invoke-BroadCase "BC-27" "reject a truncated archive transactionally" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $archive = Join-Path $caseRoot "full.sba"
    $truncated = Join-Path $caseRoot "truncated.sba"
    $output = Join-Path $caseRoot "output"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "a.txt") ("truncate" * 500)
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("pack", $backup, $archive)
    $bytes = [IO.File]::ReadAllBytes($archive)
    $cut = [Math]::Max(1, [int]($bytes.Length / 2))
    $short = New-Object byte[] $cut
    [Array]::Copy($bytes, $short, $cut)
    [IO.File]::WriteAllBytes($truncated, $short)
    Invoke-Sbm @("unpack", $truncated, $output) -ExpectFailure
    Assert-True (!(Test-Path -LiteralPath $output)) "Truncated archive left partial output"
}

Invoke-BroadCase "BC-28" "reject archive trailing data transactionally" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $archive = Join-Path $caseRoot "full.sba"
    $output = Join-Path $caseRoot "output"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "a.txt") "trailing"
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("pack", $backup, $archive)
    $stream = [IO.File]::Open($archive, [IO.FileMode]::Append, [IO.FileAccess]::Write)
    try { $stream.WriteByte(0x7f) } finally { $stream.Dispose() }
    Invoke-Sbm @("unpack", $archive, $output) -ExpectFailure
    Assert-True (!(Test-Path -LiteralPath $output)) "Trailing-data archive left partial output"
}

Invoke-BroadCase "BC-29" "reject a non-empty unpack destination without modifying it" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $archive = Join-Path $caseRoot "backup.sba"
    $output = Join-Path $caseRoot "output"
    New-Item -ItemType Directory -Path $source,$output | Out-Null
    Set-Content (Join-Path $source "a.txt") "a"
    Set-Content (Join-Path $output "keep.txt") "keep"
    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("pack", $backup, $archive)
    Invoke-Sbm @("unpack", $archive, $output) -ExpectFailure
    Assert-True ((Get-Content -Raw (Join-Path $output "keep.txt")) -match "keep") "Rejected unpack changed the destination"
}

# ---- 定时快照保留策略：BC-30 --------------------------------------------
Invoke-BroadCase "BC-30" "retain only the requested scheduled snapshots" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $snapshots = Join-Path $caseRoot "snapshots"
    New-Item -ItemType Directory -Path $source | Out-Null
    Set-Content (Join-Path $source "a.txt") "a"
    Invoke-Sbm @("schedule", $source, $snapshots, "0", "4", "--keep=2")
    Assert-True (@(Get-ChildItem -LiteralPath $snapshots -Directory).Count -eq 2) "Snapshot retention count mismatch"
}

if ($script:Failures.Count -gt 0) {
    Write-Host ""
    Write-Host "Broad coverage failures:" -ForegroundColor Red
    $script:Failures | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    throw "Broad coverage suite failed: $($script:Passed)/$($script:Total) passed. Workspace kept at $suiteRoot"
}

Remove-Item -LiteralPath $suiteRoot -Recurse -Force
Write-Host "Broad coverage tests passed: $($script:Passed)/$($script:Total)."
exit 0
