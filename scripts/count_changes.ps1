# Measures how often the picture really changes on screen (not how often the game renders): starts adam.exe,
# waits until -At seconds, captures a region of the window as fast as possible for -Dur seconds and counts changes.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\count_changes.ps1 -At 17 -Dur 3   (17 s = lunar scene)
param([double]$At = 17, [double]$Dur = 2)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W2 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
[W2]::SetProcessDPIAware() | Out-Null
$exeDir = Join-Path (Split-Path $PSScriptRoot -Parent) "runtime\build"
# --game adam skips the launcher.
$p = Start-Process -FilePath "$exeDir\adam.exe" -ArgumentList "--game", "adam" -WorkingDirectory $exeDir -PassThru -RedirectStandardOutput "$env:TEMP\burst_out.txt" -RedirectStandardError "$env:TEMP\burst_err.txt"
$start = Get-Date
while (((Get-Date) - $start).TotalSeconds -lt $At) { Start-Sleep -Milliseconds 50 }
$p.Refresh(); $h = $p.MainWindowHandle; [W2]::SetForegroundWindow($h) | Out-Null
$r = New-Object W2+RECT; [W2]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object W2+POINT; [W2]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = $r.R; $hh = $r.B
# region: middle of the lunar surface
$rx = $pt.X + [int]($w * 0.25); $ry = $pt.Y + [int]($hh * 0.55); $rw = [int]($w * 0.5); $rh = [int]($hh * 0.3)
$bmp = New-Object System.Drawing.Bitmap $rw, $rh; $g = [System.Drawing.Graphics]::FromImage($bmp)
$md5 = [System.Security.Cryptography.MD5]::Create()
$t0 = Get-Date; $prev = $null; $changes = @(); $n = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Dur) {
  $g.CopyFromScreen($rx, $ry, 0, 0, $bmp.Size)
  $ms = New-Object System.IO.MemoryStream; $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Bmp)
  $hash = [BitConverter]::ToString($md5.ComputeHash($ms.ToArray())); $ms.Dispose(); $n++
  $t = ((Get-Date) - $t0).TotalMilliseconds
  if ($prev -ne $null -and $hash -ne $prev) { $changes += $t }
  $prev = $hash
}
Stop-Process -Id $p.Id -Force
"captures: $n in $Dur s ({0:N0}/s); picture changed {1} times ({2:N1}/s)" -f ($n / $Dur), $changes.Count, ($changes.Count / $Dur)
if ($changes.Count -gt 1) { $iv = for ($i = 1; $i -lt $changes.Count; $i++) { [math]::Round($changes[$i] - $changes[$i-1]) }; "ms between changes: " + ($iv -join ' ') }
