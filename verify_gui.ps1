$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Gui {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr c, string cls, string ttl);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hDlg, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint msg, IntPtr w, StringBuilder l, uint flags, uint timeout, out IntPtr result);
  public static IntPtr FindMainWindow(string cls) {
    for (int i = 0; i < 60; i++) {
      IntPtr h = FindWindowExW(IntPtr.Zero, IntPtr.Zero, cls, null); // C# null => real NULL pointer
      if (h != IntPtr.Zero) return h;
      System.Threading.Thread.Sleep(100);
    }
    return IntPtr.Zero;
  }
  public static string ReadText(IntPtr h) {
    IntPtr len;
    SendMessageTimeoutW(h, 0x000E, IntPtr.Zero, IntPtr.Zero, 0x0002, 2000, out len); // WM_GETTEXTLENGTH
    int n = (int)len;
    if (n <= 0) return "";
    StringBuilder sb = new StringBuilder(n + 1);
    IntPtr r;
    SendMessageTimeoutW(h, 0x000D, (IntPtr)(n + 1), sb, 0x0002, 5000, out r); // WM_GETTEXT
    return sb.ToString();
  }
  public static bool Click(IntPtr h) {
    IntPtr r;
    SendMessageTimeoutW(h, 0x00F5, IntPtr.Zero, IntPtr.Zero, 0x0002 | 0x0001, 30000, out r); // BM_CLICK, SMTO_ABORTIFHUNG|SMTO_BLOCK
    return true;
  }
}
"@

# Find the GUI main window (class SimpleBackupManagerGui).
# Must call FindWindowExW with a NULL title pointer (C# null), not PowerShell
# $null which marshals to "" and would fail to match the real window title.
$main = [Gui]::FindMainWindow("SimpleBackupManagerGui")
if ($main -eq [IntPtr]::Zero) { throw "GUI window not found. Run fill_gui.ps1 first." }

$out = [Gui]::GetDlgItem($main, 106)   # output box

function Click-Button($id, $label) {
  $btn = [Gui]::GetDlgItem($main, $id)
  if ($btn -eq [IntPtr]::Zero) { Write-Host "  [$label] button not found"; return }
  Write-Host ""
  Write-Host "=== Click: $label ==="
  [Gui]::Click($btn) | Out-Null
  Start-Sleep -Milliseconds 150
  $text = [Gui]::ReadText($out)
  Write-Host $text
}

Click-Button 201 "Backup"
Click-Button 202 "Pack"
Click-Button 203 "Unpack"
Click-Button 205 "Verify"
Click-Button 204 "Restore"

Write-Host ""
Write-Host "=== 文件检查 (gui_run) ==="
$run = Join-Path $root "gui_run"
Write-Host ("backup 文件数 : " + (@(Get-ChildItem -Recurse -File (Join-Path $run "backup") -ErrorAction SilentlyContinue)).Count)
Write-Host ("secure.sba 存在: " + (Test-Path (Join-Path $run "secure.sba")))
Write-Host ("restore 文件数 : " + (@(Get-ChildItem -Recurse -File (Join-Path $run "restore") -ErrorAction SilentlyContinue)).Count)
