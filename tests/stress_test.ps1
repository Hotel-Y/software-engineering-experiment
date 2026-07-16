param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [ValidateSet("Quick", "Full")]
    [string]$Profile = "Quick",

    [string]$WorkRoot = "",

    [string[]]$Cases = @(),

    [switch]$KeepWork
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$Executable = (Resolve-Path -LiteralPath $Executable).Path
$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if ([string]::IsNullOrWhiteSpace($WorkRoot)) {
    $WorkRoot = Join-Path $projectRoot "build\stress-work"
}
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
$WorkRoot = (Resolve-Path -LiteralPath $WorkRoot).Path

$settings = if ($Profile -eq "Full") {
    [PSCustomObject]@{
        SmallFiles = 20000
        LargeMiB = 256
        RandomMiB = 128
        Depth = 50
        RewriteCycles = 30
        ConcurrentJobs = 6
        ConcurrentFiles = 1000
        ScheduleCount = 40
        ScheduleKeep = 8
        SoakCycles = 15
        RequiredFreeBytes = 4GB
    }
}
else {
    [PSCustomObject]@{
        SmallFiles = 1500
        LargeMiB = 16
        RandomMiB = 8
        Depth = 30
        RewriteCycles = 6
        ConcurrentJobs = 3
        ConcurrentFiles = 200
        ScheduleCount = 10
        ScheduleKeep = 3
        SoakCycles = 3
        RequiredFreeBytes = 512MB
    }
}

$driveName = ([IO.Path]::GetPathRoot($WorkRoot)).TrimEnd('\')
$drive = [IO.DriveInfo]::new($driveName)
if ($drive.AvailableFreeSpace -lt $settings.RequiredFreeBytes) {
    throw "Not enough free space for $Profile stress profile. Available: $($drive.AvailableFreeSpace) bytes."
}

$suiteRoot = Join-Path $WorkRoot ("sbm_stress_{0}_{1}" -f $PID, [Guid]::NewGuid().ToString("N").Substring(0, 8))
$resultsRoot = Join-Path $projectRoot "build\stress-results"
New-Item -ItemType Directory -Force -Path $suiteRoot, $resultsRoot | Out-Null

$script:Results = [Collections.Generic.List[object]]::new()
$script:Failures = [Collections.Generic.List[string]]::new()
$script:LastCommandOutput = ""

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (!$Condition) {
        throw $Message
    }
}

function Invoke-Sbm {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $Executable @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }

    $script:LastCommandOutput = $output -join "`n"
    if ($exitCode -ne 0) {
        throw "sbm failed ($exitCode): $($Arguments -join ' ')`n$script:LastCommandOutput"
    }
}

function Write-LargeFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][long]$Bytes,
        [switch]$Random
    )

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    $bufferSize = 1MB
    $buffer = New-Object byte[] $bufferSize
    $rng = $null
    if ($Random) {
        $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    }
    else {
        for ($index = 0; $index -lt $buffer.Length; ++$index) {
            $buffer[$index] = 0x41
        }
    }

    $stream = [IO.File]::Open($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $remaining = $Bytes
        while ($remaining -gt 0) {
            $count = [int][Math]::Min($buffer.Length, $remaining)
            if ($Random) {
                $rng.GetBytes($buffer)
            }
            $stream.Write($buffer, 0, $count)
            $remaining -= $count
        }
    }
    finally {
        $stream.Dispose()
        if ($null -ne $rng) {
            $rng.Dispose()
        }
    }
}

function Assert-FileHashEqual {
    param(
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Actual
    )
    Assert-True (Test-Path -LiteralPath $Actual -PathType Leaf) "Missing restored file: $Actual"
    $expectedHash = (Get-FileHash -LiteralPath $Expected -Algorithm SHA256).Hash
    $actualHash = (Get-FileHash -LiteralPath $Actual -Algorithm SHA256).Hash
    Assert-True ($expectedHash -eq $actualHash) "SHA-256 mismatch: $Expected vs $Actual"
}

function Assert-TreesEqual {
    param(
        [Parameter(Mandatory = $true)][string]$ExpectedRoot,
        [Parameter(Mandatory = $true)][string]$ActualRoot
    )

    $expectedFiles = @(Get-ChildItem -LiteralPath $ExpectedRoot -File -Recurse)
    $actualFiles = @(Get-ChildItem -LiteralPath $ActualRoot -File -Recurse)
    Assert-True ($expectedFiles.Count -eq $actualFiles.Count) `
        "File count mismatch: $($expectedFiles.Count) vs $($actualFiles.Count)"

    foreach ($expected in $expectedFiles) {
        $relative = $expected.FullName.Substring($ExpectedRoot.Length).TrimStart(
            [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
        Assert-FileHashEqual $expected.FullName (Join-Path $ActualRoot $relative)
    }
}

function Invoke-StressCase {
    param(
        [Parameter(Mandatory = $true)][string]$Id,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    if ($Cases.Count -gt 0 -and $Id -notin $Cases) {
        return
    }

    $caseRoot = Join-Path $suiteRoot $Id
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        $details = & $Body $caseRoot
        $watch.Stop()
        $detailText = ($details | ForEach-Object { "$_" }) -join "; "
        $script:Results.Add([PSCustomObject]@{
            Case = $Id
            Name = $Name
            Profile = $Profile
            Passed = $true
            Seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
            Details = $detailText
        })
        Write-Host "[$Id] PASS - $Name ($([Math]::Round($watch.Elapsed.TotalSeconds, 2)) s)"
    }
    catch {
        $watch.Stop()
        $message = $_.Exception.Message
        $script:Failures.Add("[$Id] $Name`: $message")
        $script:Results.Add([PSCustomObject]@{
            Case = $Id
            Name = $Name
            Profile = $Profile
            Passed = $false
            Seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
            Details = $message
        })
        Write-Host "[$Id] FAIL - $Name`: $message" -ForegroundColor Red
    }
}

Write-Host "Simple Backup Manager stress tests"
Write-Host "Profile: $Profile"
Write-Host "Workspace: $suiteRoot"
Write-Host ""

Invoke-StressCase "ST-01" "many-small-files backup and restore" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $restore = Join-Path $caseRoot "restore"
    New-Item -ItemType Directory -Path $source | Out-Null

    for ($index = 0; $index -lt $settings.SmallFiles; ++$index) {
        $directory = Join-Path $source ("dir_{0:D3}" -f ($index % 100))
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        [IO.File]::WriteAllText(
            (Join-Path $directory ("file_{0:D6}.txt" -f $index)),
            "record-$index-" + ("x" * 48))
    }

    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("verify", $backup)
    Invoke-Sbm @("restore", $backup, $restore)
    Assert-TreesEqual $source $restore
    "files=$($settings.SmallFiles)"
}

Invoke-StressCase "ST-02" "large compressible archive round-trip" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $archive = Join-Path $caseRoot "compressible.sba"
    $unpacked = Join-Path $caseRoot "unpacked"
    $restore = Join-Path $caseRoot "restore"
    $largeFile = Join-Path $source "compressible.bin"
    Write-LargeFile $largeFile ($settings.LargeMiB * 1MB)

    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("pack", $backup, $archive)
    Invoke-Sbm @("unpack", $archive, $unpacked)
    Invoke-Sbm @("verify", $unpacked)
    Invoke-Sbm @("restore", $unpacked, $restore)
    Assert-FileHashEqual $largeFile (Join-Path $restore "compressible.bin")
    "inputMiB=$($settings.LargeMiB);archiveBytes=$((Get-Item $archive).Length)"
}

Invoke-StressCase "ST-03" "encrypted random-data archive round-trip" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $archive = Join-Path $caseRoot "encrypted.sba"
    $unpacked = Join-Path $caseRoot "unpacked"
    $restore = Join-Path $caseRoot "restore"
    $randomFile = Join-Path $source "random.bin"
    Write-LargeFile $randomFile ($settings.RandomMiB * 1MB) -Random

    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("pack", $backup, $archive, "--password=stress-secret")
    Invoke-Sbm @("unpack", $archive, $unpacked, "--password=stress-secret")
    Invoke-Sbm @("verify", $unpacked)
    Invoke-Sbm @("restore", $unpacked, $restore)
    Assert-FileHashEqual $randomFile (Join-Path $restore "random.bin")
    "inputMiB=$($settings.RandomMiB);archiveBytes=$((Get-Item $archive).Length)"
}

Invoke-StressCase "ST-04" "deep-directory round-trip" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $restore = Join-Path $caseRoot "restore"
    $current = $source
    New-Item -ItemType Directory -Path $current | Out-Null

    for ($level = 1; $level -le $settings.Depth; ++$level) {
        # Keep each segment intentionally short so 50 levels exercise depth
        # without the test fixture itself exceeding the legacy MAX_PATH limit.
        $current = Join-Path $current "d"
        New-Item -ItemType Directory -Path $current | Out-Null
        [IO.File]::WriteAllText((Join-Path $current "level.txt"), "depth-$level")
    }

    Invoke-Sbm @("backup", $source, $backup)
    Invoke-Sbm @("verify", $backup)
    Invoke-Sbm @("restore", $backup, $restore)
    Assert-TreesEqual $source $restore
    "depth=$($settings.Depth)"
}

Invoke-StressCase "ST-05" "repeated overwrite and verification" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $backup = Join-Path $caseRoot "backup"
    $restore = Join-Path $caseRoot "restore"
    New-Item -ItemType Directory -Path $source | Out-Null
    foreach ($index in 1..200) {
        [IO.File]::WriteAllText((Join-Path $source ("item_{0:D3}.txt" -f $index)), "initial-$index")
    }

    Invoke-Sbm @("backup", $source, $backup)
    for ($cycle = 1; $cycle -le $settings.RewriteCycles; ++$cycle) {
        [IO.File]::WriteAllText((Join-Path $source "cycle.txt"), "cycle-$cycle")
        [IO.File]::WriteAllText((Join-Path $source "item_001.txt"), "updated-$cycle")
        Invoke-Sbm @("backup", $source, $backup, "--overwrite")
        Invoke-Sbm @("verify", $backup)
    }
    Invoke-Sbm @("restore", $backup, $restore)
    Assert-TreesEqual $source $restore
    "cycles=$($settings.RewriteCycles)"
}

Invoke-StressCase "ST-06" "concurrent independent backups" {
    param($caseRoot)
    $jobs = [Collections.Generic.List[object]]::new()
    for ($job = 1; $job -le $settings.ConcurrentJobs; ++$job) {
        $source = Join-Path $caseRoot "source_$job"
        $backup = Join-Path $caseRoot "backup_$job"
        New-Item -ItemType Directory -Path $source | Out-Null
        for ($index = 1; $index -le $settings.ConcurrentFiles; ++$index) {
            [IO.File]::WriteAllText(
                (Join-Path $source ("file_{0:D5}.txt" -f $index)),
                "job-$job-record-$index")
        }
        $stdout = Join-Path $caseRoot "job_$job.out.log"
        $stderr = Join-Path $caseRoot "job_$job.err.log"
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $Executable
        $startInfo.Arguments = 'backup "{0}" "{1}"' -f $source, $backup
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        Assert-True $process.Start() "Failed to start concurrent backup process $job."
        $jobs.Add([PSCustomObject]@{
            Process = $process
            Source = $source
            Backup = $backup
            Stdout = $stdout
            Stderr = $stderr
        })
    }

    $peakBytes = 0L
    foreach ($job in $jobs) {
        $outputText = $job.Process.StandardOutput.ReadToEnd()
        $errorText = $job.Process.StandardError.ReadToEnd()
        $job.Process.WaitForExit()
        [IO.File]::WriteAllText($job.Stdout, $outputText)
        [IO.File]::WriteAllText($job.Stderr, $errorText)
        $exitCode = $job.Process.ExitCode
        if ($exitCode -ne 0) {
            throw "Concurrent backup failed ($exitCode): $errorText"
        }
        $peakBytes = [Math]::Max($peakBytes, [long]$job.Process.PeakWorkingSet64)
        $job.Process.Dispose()
        Invoke-Sbm @("verify", $job.Backup)
        Assert-FileHashEqual (Join-Path $job.Source "file_00001.txt") `
            (Join-Path $job.Backup "file_00001.txt")
    }
    "jobs=$($settings.ConcurrentJobs);filesPerJob=$($settings.ConcurrentFiles);maxPeakWorkingSetBytes=$peakBytes"
}

Invoke-StressCase "ST-07" "scheduled snapshot retention under load" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $snapshots = Join-Path $caseRoot "snapshots"
    New-Item -ItemType Directory -Path $source | Out-Null
    foreach ($index in 1..200) {
        [IO.File]::WriteAllText((Join-Path $source ("item_{0:D3}.txt" -f $index)), "snapshot-$index")
    }

    Invoke-Sbm @(
        "schedule", $source, $snapshots, "0", "$($settings.ScheduleCount)",
        "--keep=$($settings.ScheduleKeep)")
    $kept = @(Get-ChildItem -LiteralPath $snapshots -Directory)
    Assert-True ($kept.Count -eq $settings.ScheduleKeep) `
        "Expected $($settings.ScheduleKeep) snapshots, found $($kept.Count)."
    foreach ($snapshot in $kept) {
        Invoke-Sbm @("verify", $snapshot.FullName)
    }
    "created=$($settings.ScheduleCount);kept=$($settings.ScheduleKeep)"
}

Invoke-StressCase "ST-08" "repeated encrypted end-to-end soak" {
    param($caseRoot)
    $source = Join-Path $caseRoot "source"
    $sourceFile = Join-Path $source "payload.bin"
    Write-LargeFile $sourceFile 4MB -Random
    [IO.File]::WriteAllText((Join-Path $source "metadata.txt"), "stress-soak")

    for ($cycle = 1; $cycle -le $settings.SoakCycles; ++$cycle) {
        $backup = Join-Path $caseRoot "backup_$cycle"
        $archive = Join-Path $caseRoot "archive_$cycle.sba"
        $unpacked = Join-Path $caseRoot "unpacked_$cycle"
        $restore = Join-Path $caseRoot "restore_$cycle"
        Invoke-Sbm @("backup", $source, $backup)
        Invoke-Sbm @("pack", $backup, $archive, "--password=soak-$cycle")
        Invoke-Sbm @("unpack", $archive, $unpacked, "--password=soak-$cycle")
        Invoke-Sbm @("verify", $unpacked)
        Invoke-Sbm @("restore", $unpacked, $restore)
        Assert-FileHashEqual $sourceFile (Join-Path $restore "payload.bin")
        Remove-Item -LiteralPath $backup, $archive, $unpacked, $restore -Recurse -Force
    }
    "cycles=$($settings.SoakCycles);payloadMiB=4"
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$csvPath = Join-Path $resultsRoot "stress_${Profile}_$timestamp.csv"
$jsonPath = Join-Path $resultsRoot "stress_${Profile}_$timestamp.json"
$script:Results | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
$script:Results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $jsonPath -Encoding UTF8

Write-Host ""
Write-Host "Stress results: $csvPath"

if ($script:Failures.Count -gt 0) {
    Write-Host "Stress test failures:" -ForegroundColor Red
    $script:Failures | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    Write-Host "Workspace kept at: $suiteRoot"
    exit 1
}

if ($KeepWork) {
    Write-Host "Workspace kept at: $suiteRoot"
}
else {
    $resolvedWorkRoot = [IO.Path]::GetFullPath($WorkRoot).TrimEnd('\') + '\'
    $resolvedSuiteRoot = [IO.Path]::GetFullPath($suiteRoot)
    if (!$resolvedSuiteRoot.StartsWith($resolvedWorkRoot, [StringComparison]::OrdinalIgnoreCase) -or
        !(Split-Path -Leaf $resolvedSuiteRoot).StartsWith("sbm_stress_")) {
        throw "Refusing to remove unsafe stress workspace: $resolvedSuiteRoot"
    }
    Remove-Item -LiteralPath $resolvedSuiteRoot -Recurse -Force
}

Write-Host "All stress tests passed: $($script:Results.Count)/$($script:Results.Count)."
exit 0
