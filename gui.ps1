$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "bin\sbm.exe"

function Select-Folder($textBox) {
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        $textBox.Text = $dialog.SelectedPath
        return $true
    }
    return $false
}

function Select-Archive($textBox, $save) {
    if ($save) {
        $dialog = New-Object System.Windows.Forms.SaveFileDialog
    } else {
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
    }
    $dialog.Filter = "Simple Backup Archive (*.sba)|*.sba|All Files (*.*)|*.*"
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        $textBox.Text = $dialog.FileName
    }
}

function Get-UniqueOutputPath {
    param(
        [Parameter(Mandatory = $true)][string]$Candidate,
        [switch]$PreserveExtension
    )
    if (!(Test-Path -LiteralPath $Candidate)) {
        return $Candidate
    }
    $parent = [System.IO.Path]::GetDirectoryName($Candidate)
    if ($PreserveExtension) {
        $stem = [System.IO.Path]::GetFileNameWithoutExtension($Candidate)
        $extension = [System.IO.Path]::GetExtension($Candidate)
    }
    else {
        $stem = [System.IO.Path]::GetFileName($Candidate)
        $extension = ""
    }
    for ($index = 2; $index -lt 10000; $index++) {
        $alternative = Join-Path $parent ("{0}_{1}{2}" -f $stem, $index, $extension)
        if (!(Test-Path -LiteralPath $alternative)) {
            return $alternative
        }
    }
    return $Candidate
}

function Set-SmartDefaults($sourceBox, $backupBox, $archiveBox, $restoreBox, $statusLabel) {
    $inputPath = $sourceBox.Text.Trim().Trim('"')
    if ([string]::IsNullOrWhiteSpace($inputPath) -or !(Test-Path -LiteralPath $inputPath)) {
        $statusLabel.Text = "Input not found. Choose an existing source, backup, or .sba file."
        return
    }

    $item = Get-Item -LiteralPath $inputPath
    $fullPath = $item.FullName
    $sourceBox.Text = $fullPath
    if (!$item.PSIsContainer) {
        if ([System.IO.Path]::GetExtension($fullPath) -ine ".sba") {
            $statusLabel.Text = "Only .sba files can be detected as archives."
            return
        }
        $parent = [System.IO.Path]::GetDirectoryName($fullPath)
        $name = [System.IO.Path]::GetFileNameWithoutExtension($fullPath)
        $archiveBox.Text = $fullPath
        $backupBox.Text = Get-UniqueOutputPath (Join-Path $parent ($name + "_unpacked"))
        $restoreBox.Text = Get-UniqueOutputPath (Join-Path $parent ($name + "_restored"))
        $statusLabel.Text = "Recommended: Unpack. Safe output paths were generated automatically."
        return
    }

    $parent = Split-Path -Parent $fullPath
    $name = Split-Path -Leaf $fullPath
    if ([string]::IsNullOrWhiteSpace($name)) { $name = "source" }
    if (Test-Path -LiteralPath (Join-Path $fullPath "manifest.sbm") -PathType Leaf) {
        $backupBox.Text = $fullPath
        $archiveBox.Text = Get-UniqueOutputPath (Join-Path $parent ($name + ".sba")) -PreserveExtension
        $restoreBox.Text = Get-UniqueOutputPath (Join-Path $parent ($name + "_restored"))
        $statusLabel.Text = "Recommended: Verify / Pack / Restore. A manifest backup was detected."
        return
    }

    $backupBox.Text = Get-UniqueOutputPath (Join-Path $parent ($name + "_backup"))
    $archiveBox.Text = Get-UniqueOutputPath ($backupBox.Text + ".sba") -PreserveExtension
    $restoreBox.Text = Get-UniqueOutputPath (Join-Path $parent ($name + "_restored"))
    $statusLabel.Text = "Recommended: Backup. Defaults use all files, auto compression, and no overwrite."
}

function Run-Command {
    param(
        [Parameter(Mandatory = $true)][object[]]$ArgsList,
        [Parameter(Mandatory = $true)]$OutputBox,
        [string]$Heading = "",
        [switch]$Append
    )
    if (!(Test-Path $exe)) {
        throw "Please run build.ps1 first."
    }
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $result = (& $exe @ArgsList 2>&1 | Out-String).Trim()
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    if ([string]::IsNullOrWhiteSpace($result)) {
        $result = "(command produced no output)"
    }
    if (![string]::IsNullOrWhiteSpace($Heading)) {
        $result = $Heading + [Environment]::NewLine + $result
    }
    if ($Append) {
        $outputBox.AppendText([Environment]::NewLine + [Environment]::NewLine + $result)
    }
    else {
        $outputBox.Text = $result
    }
    return ($exitCode -eq 0)
}

function Get-BackupPreparation([string]$Source, [string]$Backup) {
    if (![string]::IsNullOrWhiteSpace($Backup) -and
        (Test-Path -LiteralPath (Join-Path $Backup "manifest.sbm") -PathType Leaf)) {
        return "Ready"
    }
    if (![string]::IsNullOrWhiteSpace($Source) -and
        (Test-Path -LiteralPath $Source -PathType Container)) {
        return "CreateFromSource"
    }
    return "Unavailable"
}

function Ensure-BackupReady {
    param(
        [string]$Source,
        [string]$Backup,
        [string]$RequestedAction,
        $OutputBox
    )
    $preparation = Get-BackupPreparation $Source $Backup
    if ($preparation -eq "Ready") {
        return $true
    }
    if ($preparation -eq "Unavailable") {
        $outputBox.Text = "$RequestedAction needs a manifest backup. For an .sba input, click Unpack; otherwise choose an existing source or backup directory."
        return $false
    }
    return Run-Command -ArgsList @("backup", $Source, $Backup) -OutputBox $OutputBox -Heading "Step 1/2: creating the prerequisite backup"
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "Simple Backup Manager"
$form.Size = New-Object System.Drawing.Size(780, 580)
$form.StartPosition = "CenterScreen"

$font = New-Object System.Drawing.Font("Segoe UI", 9)
$form.Font = $font

$sourceBox = New-Object System.Windows.Forms.TextBox
$sourceBox.SetBounds(120, 24, 410, 24)
$backupBox = New-Object System.Windows.Forms.TextBox
$backupBox.SetBounds(120, 60, 500, 24)
$archiveBox = New-Object System.Windows.Forms.TextBox
$archiveBox.SetBounds(120, 96, 500, 24)
$restoreBox = New-Object System.Windows.Forms.TextBox
$restoreBox.SetBounds(120, 132, 500, 24)

$labels = @(
    @("Input", 24),
    @("Backup", 60),
    @("Archive", 96),
    @("Restore", 132)
)
foreach ($item in $labels) {
    $label = New-Object System.Windows.Forms.Label
    $label.Text = $item[0]
    $label.SetBounds(24, $item[1] + 3, 80, 20)
    $form.Controls.Add($label)
}

$browseSource = New-Object System.Windows.Forms.Button
$browseSource.Text = "..."
$browseSource.SetBounds(540, 24, 36, 24)
$browseSource.Add_Click({
    if (Select-Folder $sourceBox) {
        Set-SmartDefaults $sourceBox $backupBox $archiveBox $restoreBox $statusLabel
    }
})

$autoButton = New-Object System.Windows.Forms.Button
$autoButton.Text = "Auto Fill"
$autoButton.SetBounds(585, 22, 86, 28)
$autoButton.Add_Click({
    Set-SmartDefaults $sourceBox $backupBox $archiveBox $restoreBox $statusLabel
})

$browseBackup = New-Object System.Windows.Forms.Button
$browseBackup.Text = "..."
$browseBackup.SetBounds(635, 60, 36, 24)
$browseBackup.Add_Click({ Select-Folder $backupBox })

$browseArchive = New-Object System.Windows.Forms.Button
$browseArchive.Text = "..."
$browseArchive.SetBounds(635, 96, 36, 24)
$browseArchive.Add_Click({ Select-Archive $archiveBox $true })

$browseRestore = New-Object System.Windows.Forms.Button
$browseRestore.Text = "..."
$browseRestore.SetBounds(635, 132, 36, 24)
$browseRestore.Add_Click({ Select-Folder $restoreBox })

$compressionLabel = New-Object System.Windows.Forms.Label
$compressionLabel.Text = "Compression: LZ77 + Huffman (block-wise)"
$compressionLabel.SetBounds(120, 176, 255, 20)

$passwordLabel = New-Object System.Windows.Forms.Label
$passwordLabel.Text = "Password"
$passwordLabel.SetBounds(390, 176, 70, 20)

$passwordBox = New-Object System.Windows.Forms.TextBox
$passwordBox.SetBounds(465, 172, 175, 24)

$statusLabel = New-Object System.Windows.Forms.Label
$statusLabel.Text = "Default rules: all files, automatic compression, no overwrite."
$statusLabel.SetBounds(24, 202, 700, 36)
$sourceBox.Add_Leave({
    Set-SmartDefaults $sourceBox $backupBox $archiveBox $restoreBox $statusLabel
})

$outputBox = New-Object System.Windows.Forms.TextBox
$outputBox.Multiline = $true
$outputBox.ScrollBars = "Vertical"
$outputBox.SetBounds(24, 294, 720, 218)

$backupButton = New-Object System.Windows.Forms.Button
$backupButton.Text = "Backup"
$backupButton.SetBounds(24, 246, 100, 32)
$backupButton.Add_Click({
    [void](Run-Command -ArgsList @("backup", $sourceBox.Text, $backupBox.Text) -OutputBox $outputBox)
})

$packButton = New-Object System.Windows.Forms.Button
$packButton.Text = "Pack"
$packButton.SetBounds(140, 246, 100, 32)
$packButton.Add_Click({
    $preparation = Get-BackupPreparation $sourceBox.Text $backupBox.Text
    if (!(Ensure-BackupReady $sourceBox.Text $backupBox.Text "Pack" $outputBox)) { return }
    $args = @("pack", $backupBox.Text, $archiveBox.Text)
    if ($passwordBox.Text.Length -gt 0) { $args += "--password=$($passwordBox.Text)" }
    [void](Run-Command -ArgsList $args -OutputBox $outputBox `
        -Heading $(if ($preparation -eq "CreateFromSource") { "Step 2/2: packing the backup" } else { "" }) `
        -Append:($preparation -eq "CreateFromSource"))
})

$unpackButton = New-Object System.Windows.Forms.Button
$unpackButton.Text = "Unpack"
$unpackButton.SetBounds(256, 246, 100, 32)
$unpackButton.Add_Click({
    if (!(Test-Path -LiteralPath $archiveBox.Text -PathType Leaf)) {
        $outputBox.Text = "Archive file not found. To create one from a source directory, click Pack; the prerequisite Backup will run automatically."
        return
    }
    $args = @("unpack", $archiveBox.Text, $backupBox.Text)
    if ($passwordBox.Text.Length -gt 0) { $args += "--password=$($passwordBox.Text)" }
    [void](Run-Command -ArgsList $args -OutputBox $outputBox)
})

$restoreButton = New-Object System.Windows.Forms.Button
$restoreButton.Text = "Restore"
$restoreButton.SetBounds(372, 246, 100, 32)
$restoreButton.Add_Click({
    $preparation = Get-BackupPreparation $sourceBox.Text $backupBox.Text
    if (!(Ensure-BackupReady $sourceBox.Text $backupBox.Text "Restore" $outputBox)) { return }
    [void](Run-Command -ArgsList @("restore", $backupBox.Text, $restoreBox.Text) -OutputBox $outputBox `
        -Heading $(if ($preparation -eq "CreateFromSource") { "Step 2/2: restoring the backup" } else { "" }) `
        -Append:($preparation -eq "CreateFromSource"))
})

$verifyButton = New-Object System.Windows.Forms.Button
$verifyButton.Text = "Verify"
$verifyButton.SetBounds(488, 246, 100, 32)
$verifyButton.Add_Click({
    $preparation = Get-BackupPreparation $sourceBox.Text $backupBox.Text
    if (!(Ensure-BackupReady $sourceBox.Text $backupBox.Text "Verify" $outputBox)) { return }
    [void](Run-Command -ArgsList @("verify", $backupBox.Text) -OutputBox $outputBox `
        -Heading $(if ($preparation -eq "CreateFromSource") { "Step 2/2: verifying the backup" } else { "" }) `
        -Append:($preparation -eq "CreateFromSource"))
})

$form.Controls.AddRange(@(
    $sourceBox, $backupBox, $archiveBox, $restoreBox,
    $browseSource, $autoButton, $browseBackup, $browseArchive, $browseRestore,
    $compressionLabel, $passwordLabel, $passwordBox, $statusLabel,
    $backupButton, $packButton, $unpackButton, $restoreButton, $verifyButton,
    $outputBox
))

[void]$form.ShowDialog()
