# Runs adam.exe and follows a list of timed steps, then stops it. Keys are only sent while adam's window
# is in the foreground.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\run_game.ps1 -Steps "20 shot,21 key 0D,29 shot" [-Out <dir>]
# Steps are "<seconds> shot" (window screenshot -> <Out>\in_<t>s.png), "<seconds> ocr" (shot + its text), "<seconds> state" (game state) or "<seconds> key <VK hex>" (0D = Enter/Start,
# 58 = X/A, 78 = F9 = RAM dump), "<seconds> click <x> <y>" (left click, window pixels as in the screenshots), "<seconds> move <x> <y>", "<seconds> wheel <notches>". Saves adam stdout/stderr to <Out> (default %TEMP%\adam_run).
[CmdletBinding(PositionalBinding = $false)]
# -ExeDir runs another build, e.g. a snapshot folder (snapshots\...; it needs its own stored ROM: adam.exe --rom once).
param([Parameter(Mandatory)][string[]]$Steps, [string]$Out = (Join-Path $env:TEMP "adam_run"),
      [string]$ExeDir = "", [switch]$Launcher)
# (Windows PowerShell 5.1 has no $PSScriptRoot inside param(), so the default is set here.)
if (-not $ExeDir) { $ExeDir = Join-Path (Split-Path $PSScriptRoot -Parent) "runtime\build" }
# One comma-separated string (needed with -File) or an array both work.
$Steps = ($Steps -join ',') -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hgt, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
}
"@
# Real pixel coordinates for GetWindowRect/CopyFromScreen when Windows display scaling is above 100%.
[W]::SetProcessDPIAware() | Out-Null
$exeDir = $ExeDir
# --game adam skips the launcher and starts the game right away; -Launcher stays in the launcher instead.
$startArgs = @{ FilePath = "$exeDir\adam.exe"; WorkingDirectory = $exeDir; PassThru = $true
    RedirectStandardOutput = "$Out\adam_stdout.txt"; RedirectStandardError = "$Out\adam_stderr.txt" }
if (-not $Launcher) { $startArgs.ArgumentList = @("--game", "adam") }
# The game writes its state (game mode, pause, menu, speed...) here; "<t> state" prints it (main.cpp write_state_file).
$stateFile = Join-Path $Out "state.json"
Remove-Item $stateFile -ErrorAction SilentlyContinue
$env:ADAM_STATE_FILE = $stateFile
$p = Start-Process @startArgs
$start = Get-Date
$heldKeys = @{}  # keys still held by "down" steps: released at the end, even if adam exits early (else Windows keeps them down)
foreach ($step in $Steps) {
    $parts = $step.Split(' ')
    $t = [double]$parts[0]
    $wait = $t - ((Get-Date) - $start).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
    $p.Refresh()
    if ($p.HasExited) { "exited early, code $($p.ExitCode)"; break }
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { "no window at ${t}s"; continue }
    [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 150
    if ($parts[1] -eq 'shot' -or $parts[1] -eq 'ocr') {
        # Only when adam is really in front: the capture copies the screen, so otherwise it would show other windows.
        if ([W]::GetForegroundWindow() -ne $h) { "skip $($parts[1]) at ${t}s: window not in front"; continue }
        $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
        $bmp.Save("$Out\in_${t}s.png"); $g.Dispose(); $bmp.Dispose()
        "shot at ${t}s"
        if ($parts[1] -eq 'ocr') {
            # "<t> ocr": the shot plus its text (scripts\ocr.ps1 at 2x: "x y w h  text" per line). Takes ~1-2 s, so
            # leave that much room before the next step.
            & (Join-Path $PSScriptRoot "ocr.ps1") -Image "$Out\in_${t}s.png" -Scale 2 | ForEach-Object { "  ocr: $_" }
        }
    } elseif ($parts[1] -eq 'state') {
        # "<t> state": the game's own state (JSON line, updated 5x/s): game_mode, logic_running (0 = game's pause screen),
        # menu, paused (P), speed, rewinding. Doesn't need the window in front.
        $st = Get-Content $stateFile -ErrorAction SilentlyContinue
        "state at ${t}s: $(if ($st) { $st } else { '(none yet)' })"
    } elseif ($parts[1] -eq 'title') {
        # "<t> title": print the window title (game time and frame counter), no screenshot needed.
        "title at ${t}s: $($p.MainWindowTitle)"
    } elseif ($parts[1] -eq 'size') {
        # "<t> size <width> <height>": resize the window (outer size, keeps its position; 0x2 = SWP_NOMOVE).
        [W]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, [int]$parts[2], [int]$parts[3], 0x2) | Out-Null
        "size $($parts[2])x$($parts[3]) at ${t}s"
    } elseif ($parts[1] -eq 'click') {
        # "<t> click <x> <y>": left click at a point of the window, in screenshot pixels (window rect incl. title bar).
        if ([W]::GetForegroundWindow() -ne $h) { "skip click at ${t}s: window not in front"; continue }
        $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
        [W]::SetCursorPos($r.L + [int]$parts[2], $r.T + [int]$parts[3]) | Out-Null; Start-Sleep -Milliseconds 100
        [W]::mouse_event(0x2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [W]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero)
        # Park the pointer on the title bar: hovering highlights menu items and would change later screenshots.
        Start-Sleep -Milliseconds 100; [W]::SetCursorPos($r.L + 8, $r.T + 8) | Out-Null
        "click $($parts[2]),$($parts[3]) at ${t}s"
    } elseif ($parts[1] -eq 'move') {
        # "<t> move <x> <y>": put the mouse cursor on a point of the window (no click), e.g. before "wheel" (which parks it after).
        $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
        [W]::SetCursorPos($r.L + [int]$parts[2], $r.T + [int]$parts[3]) | Out-Null
        "move $($parts[2]),$($parts[3]) at ${t}s"
    } elseif ($parts[1] -eq 'wheel') {
        # "<t> wheel <notches>": mouse wheel at the cursor (negative = scroll down; move the cursor with a click first).
        [W]::mouse_event(0x800, 0, 0, [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$parts[2] * 120), 0), [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 100; $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
        [W]::SetCursorPos($r.L + 8, $r.T + 8) | Out-Null  # park the pointer again (see click)
        "wheel $($parts[2]) at ${t}s"
    } elseif (@('key', 'down', 'up') -contains $parts[1]) {
        # "key" taps a key; "down" / "up" hold and release it (e.g. "30 down 09,40 up 09" holds Tab for 10 s).
        if ([W]::GetForegroundWindow() -ne $h) { "skip $($parts[1]) at ${t}s: window not in front"; continue }
        $vk = [byte][Convert]::ToInt32($parts[2], 16)
        # Real scan code too: SDL (adam's input since RecompFrontend) identifies keys by scan code.
        # Arrows, Ins/Del/Home/End/PgUp/PgDn are "extended" keys: without that flag (1) their scan code means the numpad.
        $scan = [byte][W]::MapVirtualKey($vk, 0)
        $ext = if (@(0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E) -contains $vk) { 1 } else { 0 }
        if ($parts[1] -ne 'up') { [W]::keybd_event($vk, $scan, $ext, [UIntPtr]::Zero) }
        if ($parts[1] -eq 'key') { Start-Sleep -Milliseconds 200 }
        if ($parts[1] -ne 'down') { [W]::keybd_event($vk, $scan, 2 -bor $ext, [UIntPtr]::Zero) }
        if ($parts[1] -eq 'down') { $heldKeys[$vk] = @($scan, $ext) } elseif ($parts[1] -eq 'up') { $heldKeys.Remove($vk) }
        "$($parts[1]) $($parts[2]) at ${t}s"
    }
}
foreach ($vk in @($heldKeys.Keys)) {
    [W]::keybd_event([byte]$vk, [byte]$heldKeys[$vk][0], 2 -bor $heldKeys[$vk][1], [UIntPtr]::Zero)
    "released key $('{0:X2}' -f $vk)"
}
$p.Refresh()
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
