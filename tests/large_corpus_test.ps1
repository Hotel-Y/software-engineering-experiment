# 大型异构语料系统/性能测试：七个阶段覆盖备份到逐文件 SHA-256 对比的完整链路。
param(
    [Parameter(Mandatory = $true)][string]$CorpusPath,
    [string]$Executable,
    [switch]$KeepWork,
    [string]$CaptureSyncDirectory = ""
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

# 未指定程序时使用项目 bin 目录；所有中间结果放入唯一系统临时目录。
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $root "bin\sbm.exe"
}
$exe = (Resolve-Path -LiteralPath $Executable).Path
$corpus = (Resolve-Path -LiteralPath $CorpusPath).Path
$work = Join-Path ([IO.Path]::GetTempPath()) `
    ("sbm_large_{0}_{1}" -f $PID, [Guid]::NewGuid().ToString("N").Substring(0, 8))
$backup = Join-Path $work "backup"
$archive = Join-Path $work "silesia.sba"
$unpacked = Join-Path $work "unpacked"
$restore = Join-Path $work "restore"
$script:Case = 0
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$script:CaptureEnabled = ![string]::IsNullOrWhiteSpace($CaptureSyncDirectory)
if ($script:CaptureEnabled) {
    New-Item -ItemType Directory -Path $CaptureSyncDirectory -Force | Out-Null
}

function Wait-CaptureResume {
    param([Parameter(Mandatory = $true)][string]$Id)
    if (!$script:CaptureEnabled) { return }

    # ready/resume 文件只用于协调真实测试截图，不影响任何测试断言。
    $ready = Join-Path $CaptureSyncDirectory "$Id.ready"
    $resume = Join-Path $CaptureSyncDirectory "$Id.resume"
    Set-Content -LiteralPath $ready -Value (Get-Date -Format "o") -Encoding ASCII
    $deadline = (Get-Date).AddMinutes(5)
    while (!(Test-Path -LiteralPath $resume)) {
        if ((Get-Date) -gt $deadline) {
            throw "Timed out waiting for screenshot capture acknowledgement: $Id"
        }
        Start-Sleep -Milliseconds 100
    }
    Remove-Item -LiteralPath $resume -Force
}

function Invoke-LargeStage {
    param([string]$Name, [scriptblock]$Body)
    # 为七个阶段分配稳定编号，并在阶段脚本块无异常时记录 PASS。
    ++$script:Case
    $id = "LARGE-{0:D2}" -f $script:Case
    if ($script:CaptureEnabled) {
        try { Clear-Host } catch {}
        try { $host.UI.RawUI.WindowTitle = "SBM REAL TEST - $id" } catch {}
        Write-Host "Simple Backup Manager - REAL TEST EXECUTION"
        Write-Host "Case ID: $id"
        Write-Host "Stage: $Name"
        Write-Host "Corpus: $corpus"
        Write-Host "Started: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
        Write-Host ""
    }
    & $Body
    Write-Host ("[$id] PASS - $Name")
    if ($script:CaptureEnabled -and $script:Case -lt 7) {
        Write-Host ""
        Write-Host "CAPTURE READY - actual execution completed" -ForegroundColor Green
        Wait-CaptureResume -Id $id
    }
}

function Invoke-Sbm {
    # 大语料阶段要求每条核心命令成功，非零退出码立即终止并保留上下文。
    param([string[]]$Arguments)
    & $exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Large-corpus command failed ($LASTEXITCODE): $($Arguments -join ' ')"
    }
}

if (Test-Path -LiteralPath $work) {
    Remove-Item -LiteralPath $work -Recurse -Force
}
New-Item -ItemType Directory -Path $work | Out-Null

# 先取得文件数与总字节数，确保输入确实达到“大型、异构”测试门槛。
$sourceFiles = @(Get-ChildItem -LiteralPath $corpus -File -Recurse)
$sourceBytes = ($sourceFiles | Measure-Object Length -Sum).Sum

Invoke-LargeStage "validate a heterogeneous corpus of at least 100 MB and 10 files" {
    if ($sourceFiles.Count -lt 10 -or $sourceBytes -lt 100MB) {
        throw "Corpus is too small: $($sourceFiles.Count) files, $sourceBytes bytes"
    }
}

Invoke-LargeStage "backup the complete corpus" {
    Invoke-Sbm @("backup", $corpus, $backup)
}

Invoke-LargeStage "verify the large backup manifest and checksums" {
    Invoke-Sbm @("verify", $backup)
}

Invoke-LargeStage "pack the large backup with block compression" {
    Invoke-Sbm @("pack", $backup, $archive)
    if (!(Test-Path -LiteralPath $archive)) { throw "Large archive was not created" }
}

Invoke-LargeStage "inspect every archive compression pipeline" {
    $inspection = & python (Join-Path $root "sba_inspect.py") $archive
    if ($LASTEXITCODE -ne 0 -or ($inspection -join "`n") -notmatch "pipeline round-trip.*all OK") {
        throw "Large archive inspector validation failed"
    }
}

Invoke-LargeStage "unpack and verify the large archive" {
    Invoke-Sbm @("unpack", $archive, $unpacked)
    Invoke-Sbm @("verify", $unpacked)
}

Invoke-LargeStage "restore all corpus files and compare SHA-256 hashes" {
    Invoke-Sbm @("restore", $unpacked, $restore)
    $restoredFiles = @(Get-ChildItem -LiteralPath $restore -File -Recurse)
    if ($restoredFiles.Count -ne $sourceFiles.Count) {
        throw "Restored file count mismatch: $($restoredFiles.Count) vs $($sourceFiles.Count)"
    }
    # 保持相对路径逐个定位还原文件，并比较完整内容哈希。
    foreach ($sourceFile in $sourceFiles) {
        $relative = $sourceFile.FullName.Substring($corpus.Length).TrimStart(
            [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
        $restoredFile = Join-Path $restore $relative
        if (!(Test-Path -LiteralPath $restoredFile)) { throw "Missing restored corpus file: $relative" }
        $sourceHash = (Get-FileHash -LiteralPath $sourceFile.FullName -Algorithm SHA256).Hash
        $restoreHash = (Get-FileHash -LiteralPath $restoredFile -Algorithm SHA256).Hash
        if ($sourceHash -ne $restoreHash) { throw "Corpus hash mismatch: $relative" }
    }
}

# 汇总规模、归档大小与总耗时，既提供正确性结果，也保留性能观察数据。
$stopwatch.Stop()
$archiveBytes = (Get-Item -LiteralPath $archive).Length
Write-Host "Large corpus tests passed: $script:Case/7."
Write-Host "Source: $($sourceFiles.Count) files, $sourceBytes bytes"
Write-Host "Archive: $archiveBytes bytes"
Write-Host ("Elapsed: {0:N1} seconds" -f $stopwatch.Elapsed.TotalSeconds)

if ($script:CaptureEnabled) {
    Write-Host ""
    Write-Host "CAPTURE READY - actual execution completed" -ForegroundColor Green
    Wait-CaptureResume -Id "LARGE-07"
}

# 默认清理大体积中间文件；调试时可用 -KeepWork 保留现场。
if (!$KeepWork) {
    Remove-Item -LiteralPath $work -Recurse -Force
}
else {
    Write-Host "Workspace kept at: $work"
}
exit 0
