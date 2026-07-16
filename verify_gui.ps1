# 对已打开并填好的 GUI 依次发送按钮点击消息，读取输出框用于快速界面验收。
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# SendMessageTimeoutW 带超时保护，避免业务执行卡住时测试脚本永久等待。
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
      IntPtr h = FindWindowExW(IntPtr.Zero, IntPtr.Zero, cls, null); // C# null 会传递真正的空指针。
      if (h != IntPtr.Zero) return h;
      System.Threading.Thread.Sleep(100);
    }
    return IntPtr.Zero;
  }
  public static string ReadText(IntPtr h) {
    IntPtr len;
    SendMessageTimeoutW(h, 0x000E, IntPtr.Zero, IntPtr.Zero, 0x0002, 2000, out len); // 读取文本长度。
    int n = (int)len;
    if (n <= 0) return "";
    StringBuilder sb = new StringBuilder(n + 1);
    IntPtr r;
    SendMessageTimeoutW(h, 0x000D, (IntPtr)(n + 1), sb, 0x0002, 5000, out r); // 读取完整文本。
    return sb.ToString();
  }
  public static bool Click(IntPtr h) {
    IntPtr r;
    SendMessageTimeoutW(h, 0x00F5, IntPtr.Zero, IntPtr.Zero, 0x0002 | 0x0001, 30000, out r); // 点击并在窗口无响应时中止。
    return true;
  }
}
"@

# 按窗口类名查找主窗口。标题参数必须由 C# 传真正的 NULL；PowerShell 的 $null
# 可能封送为空字符串，从而无法匹配实际窗口标题。
$main = [Gui]::FindMainWindow("SimpleBackupManagerGui")
if ($main -eq [IntPtr]::Zero) { throw "GUI window not found. Run fill_gui.ps1 first." }

$out = [Gui]::GetDlgItem($main, 106)   # 106 是多行输出框。

function Click-Button($id, $label) {
  # 点击指定按钮，稍等界面刷新，再把当前输出框内容打印到终端。
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
