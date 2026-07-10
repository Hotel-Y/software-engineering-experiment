$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "bin\sbm.exe"

function Select-Folder($textBox) {
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        $textBox.Text = $dialog.SelectedPath
    }
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

function Run-Command($argsList, $outputBox) {
    if (!(Test-Path $exe)) {
        throw "Please run build.ps1 first."
    }
    $outputBox.Text = "Running..."
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo.FileName = $exe
    foreach ($arg in $argsList) {
        [void]$process.StartInfo.ArgumentList.Add($arg)
    }
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.UseShellExecute = $false
    [void]$process.Start()
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $outputBox.Text = ($stdout + "`r`n" + $stderr).Trim()
}

$form = New-Object System.Windows.Forms.Form
$form.Text = "Simple Backup Manager"
$form.Size = New-Object System.Drawing.Size(760, 520)
$form.StartPosition = "CenterScreen"

$font = New-Object System.Drawing.Font("Segoe UI", 9)
$form.Font = $font

$sourceBox = New-Object System.Windows.Forms.TextBox
$sourceBox.SetBounds(120, 24, 500, 24)
$backupBox = New-Object System.Windows.Forms.TextBox
$backupBox.SetBounds(120, 60, 500, 24)
$archiveBox = New-Object System.Windows.Forms.TextBox
$archiveBox.SetBounds(120, 96, 500, 24)
$restoreBox = New-Object System.Windows.Forms.TextBox
$restoreBox.SetBounds(120, 132, 500, 24)

$labels = @(
    @("Source", 24),
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
$browseSource.SetBounds(635, 24, 36, 24)
$browseSource.Add_Click({ Select-Folder $sourceBox })

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

$compressCheck = New-Object System.Windows.Forms.CheckBox
$compressCheck.Text = "RLE compression"
$compressCheck.SetBounds(120, 172, 140, 24)

$passwordLabel = New-Object System.Windows.Forms.Label
$passwordLabel.Text = "Password"
$passwordLabel.SetBounds(285, 176, 70, 20)

$passwordBox = New-Object System.Windows.Forms.TextBox
$passwordBox.SetBounds(360, 172, 180, 24)

$outputBox = New-Object System.Windows.Forms.TextBox
$outputBox.Multiline = $true
$outputBox.ScrollBars = "Vertical"
$outputBox.SetBounds(24, 260, 700, 190)

$backupButton = New-Object System.Windows.Forms.Button
$backupButton.Text = "Backup"
$backupButton.SetBounds(24, 212, 100, 32)
$backupButton.Add_Click({
    Run-Command @("backup", $sourceBox.Text, $backupBox.Text, "--overwrite") $outputBox
})

$packButton = New-Object System.Windows.Forms.Button
$packButton.Text = "Pack"
$packButton.SetBounds(140, 212, 100, 32)
$packButton.Add_Click({
    $args = @("pack", $backupBox.Text, $archiveBox.Text)
    if ($compressCheck.Checked) { $args += "--compress=rle" }
    if ($passwordBox.Text.Length -gt 0) { $args += "--password=$($passwordBox.Text)" }
    Run-Command $args $outputBox
})

$unpackButton = New-Object System.Windows.Forms.Button
$unpackButton.Text = "Unpack"
$unpackButton.SetBounds(256, 212, 100, 32)
$unpackButton.Add_Click({
    $args = @("unpack", $archiveBox.Text, $backupBox.Text)
    if ($passwordBox.Text.Length -gt 0) { $args += "--password=$($passwordBox.Text)" }
    Run-Command $args $outputBox
})

$restoreButton = New-Object System.Windows.Forms.Button
$restoreButton.Text = "Restore"
$restoreButton.SetBounds(372, 212, 100, 32)
$restoreButton.Add_Click({
    Run-Command @("restore", $backupBox.Text, $restoreBox.Text) $outputBox
})

$verifyButton = New-Object System.Windows.Forms.Button
$verifyButton.Text = "Verify"
$verifyButton.SetBounds(488, 212, 100, 32)
$verifyButton.Add_Click({
    Run-Command @("verify", $backupBox.Text) $outputBox
})

$form.Controls.AddRange(@(
    $sourceBox, $backupBox, $archiveBox, $restoreBox,
    $browseSource, $browseBackup, $browseArchive, $browseRestore,
    $compressCheck, $passwordLabel, $passwordBox,
    $backupButton, $packButton, $unpackButton, $restoreButton, $verifyButton,
    $outputBox
))

[void]$form.ShowDialog()
