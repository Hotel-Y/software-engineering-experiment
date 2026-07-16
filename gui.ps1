# Windows Forms 备用界面：实现与原生 GUI 相同的智能默认值，并统一调用 bin\sbm.exe。
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "bin\sbm.exe"

function Select-Folder($textBox) {
    # 用户确认后把文件夹选择结果直接写回对应文本框。
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        $textBox.Text = $dialog.SelectedPath
        return $true
    }
    return $false
}

function Select-Archive($textBox, $save) {
    # Pack 使用保存对话框，Unpack 使用打开对话框，两者都优先筛选 .sba。
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
    # 候选路径被占用时追加数字后缀，PreserveExtension 保证序号位于 .sba 之前。
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
    # 一个输入可识别为普通源目录、含清单的备份目录或 SBA 归档文件。
    $inputPath = $sourceBox.Text.Trim().Trim('"')
    if ([string]::IsNullOrWhiteSpace($inputPath) -or !(Test-Path -LiteralPath $inputPath)) {
        $statusLabel.Text = "Input not found. Choose an existing source, backup, or .sba file."
        return
    }

    $item = Get-Item -LiteralPath $inputPath
    $fullPath = $item.FullName
    $sourceBox.Text = $fullPath
    # 文件输入只接受 .sba，并为它生成安全的解包及还原输出目录。
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
    # manifest.sbm 是备份目录的身份标志；没有清单的目录则视作待备份源目录。
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
    # 所有按钮最终都进入此函数，集中捕获退出码、标准输出和错误输出。
    if (!(Test-Path $exe)) {
        throw "Please run build.ps1 first."
    }
    # CLI 失败需要显示在输出框中，所以调用期间暂时不让 PowerShell 自动终止。
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
    # 返回值描述依赖备份的操作能否直接执行，或是否可以从源目录自动补建备份。
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
    # 对 Pack、Restore、Verify 复用同一前置检查，避免各按钮规则不一致。
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

# ---- 界面布局 -------------------------------------------------------------
# 先创建窗体与输入控件，再为按钮绑定事件，最后统一加入 Controls 集合。
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

# 标签位置与对应文本框保持相同纵坐标，便于后续整体调整布局。
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

# 源目录选择完成后立即触发智能填充；Auto Fill 可供手工粘贴路径后再次触发。
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

# 多行输出框展示 CLI 原始结果，也是人工验收和错误定位的主要依据。
$outputBox = New-Object System.Windows.Forms.TextBox
$outputBox.Multiline = $true
$outputBox.ScrollBars = "Vertical"
$outputBox.SetBounds(24, 294, 720, 218)

# ---- 操作按钮 -------------------------------------------------------------
# 事件处理器只组装参数；备份、压缩、加密等业务不在界面层重复实现。
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
    # 若当前输入是普通源目录，Pack 会先自动 Backup，再追加显示第二阶段结果。
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
    # Unpack 必须以真实归档文件为输入，避免把“尚未创建的默认归档路径”误当成源。
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

# 控件全部配置完后一次加入窗体，最后用模态窗口启动消息循环。
$form.Controls.AddRange(@(
    $sourceBox, $backupBox, $archiveBox, $restoreBox,
    $browseSource, $autoButton, $browseBackup, $browseArchive, $browseRestore,
    $compressionLabel, $passwordLabel, $passwordBox, $statusLabel,
    $backupButton, $packButton, $unpackButton, $restoreButton, $verifyButton,
    $outputBox
))

[void]$form.ShowDialog()
