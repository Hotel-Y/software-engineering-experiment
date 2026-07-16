# GUI 人工验收辅助脚本：启动原生窗口并把一套示例路径、密码填入对应控件。
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root "bin\sbm_gui.exe"
$run = Join-Path $root "gui_run"

$source  = Join-Path $run "source"
$backup  = Join-Path $run "backup"
$archive = Join-Path $run "secure.sba"
$restore = Join-Path $run "restore"

# 使用极少量 Win32 互操作查找窗口并向编辑框发送 Unicode 文本。
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Win {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr c, string cls, string ttl);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public static IntPtr FindMainWindow(string cls) {
    IntPtr h = IntPtr.Zero;
    for (int i = 0; i < 60; i++) {
      h = FindWindowExW(IntPtr.Zero, IntPtr.Zero, cls, null);
      if (h != IntPtr.Zero) return h;
      System.Threading.Thread.Sleep(100);
    }
    return IntPtr.Zero;
  }
}
"@

# 先结束旧实例，确保后续找到的是本次新启动、输入为空的窗口。
Get-Process -Name sbm_gui -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 200

Start-Process -FilePath $exe
$main = [Win]::FindMainWindow("SimpleBackupManagerGui")
if ($main -eq [IntPtr]::Zero) { throw "GUI window not found." }

# 控件 ID 与 gui_win.cpp 一致：源=101、备份=102、归档=103、还原=104、密码=105。
# GetDlgItem 根据父窗口和 ID 取得具体输入框句柄。
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Dlg {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hDlg, int id);
}
"@

$WM_SETTEXT = 0x000C
$BM_SETCHECK = 0x00A1
$BST_CHECKED = [IntPtr]1

function SetText($id, $text) {
  # WM_SETTEXT 直接填写控件，不模拟键盘，因此中文路径也能稳定输入。
  $h = [Dlg]::GetDlgItem($main, $id)
  if ($h -eq [IntPtr]::Zero) { Write-Host "control $id not found"; return }
  [Win]::SendMessageW($h, $WM_SETTEXT, [IntPtr]::Zero, $text) | Out-Null
}

SetText 101 $source
SetText 102 $backup
SetText 103 $archive
SetText 104 $restore
SetText 105 "secret123"

# LZ77 + Huffman 压缩始终启用，所以界面没有需要勾选的压缩复选框。

[Win]::SetForegroundWindow($main) | Out-Null

Write-Host "GUI window opened and fields filled:"
Write-Host "  Source  = $source"
Write-Host "  Backup  = $backup"
Write-Host "  Archive = $archive"
Write-Host "  Restore = $restore"
Write-Host "  Password= secret123  (compression LZ77+Huffman is always on)"
Write-Host ""
Write-Host "Now click in this order:  Backup -> Verify -> Pack -> Unpack -> Restore"
