# Copies the current build (runtime\build) into snapshots\<date>_<time>_<commit>[_<name>]\ so it can be run again
# later, e.g. to compare with an older version. The snapshot has no ROM and no saves: it gets its own data\ folder on
# first run (adam.exe --rom <rom.z64>, or Load ROM in the launcher on builds that have it). snapshots\ is gitignored:
# the exe contains the recompiled game code, so like the ROM it stays local and is never shared or committed.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\snapshot.ps1 [-Name <short-label>]
param([string]$Name = "")
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$deps = Split-Path $root -Parent   # RT64 and RecompFrontend are checked out next to the project folder
$build = Join-Path $root "runtime\build"
if (-not (Test-Path (Join-Path $build "adam.exe"))) { Write-Error "No runtime\build\adam.exe - build first." }

$commit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = [bool](git -C $root status --porcelain --untracked-files=no)
$label = (Get-Date -Format "yyyy-MM-dd_HHmm") + "_" + $commit + $(if ($dirty) { "-dirty" } else { "" }) + $(if ($Name) { "_$Name" } else { "" })
$dest = Join-Path $root "snapshots\$label"
New-Item -ItemType Directory -Force $dest | Out-Null

# The exe and everything it loads at runtime; the .pdb lets a debugger (lldb) show function names in a hang.
foreach ($file in @("adam.exe", "adam.pdb", "SDL2.dll", "dxil.dll", "dxcompiler.dll")) {
    $src = Join-Path $build $file
    if (Test-Path $src) { Copy-Item $src $dest }
}

$exe = Get-Item (Join-Path $build "adam.exe")
@"
adam snapshot $label
Built:         $($exe.LastWriteTime.ToString("yyyy-MM-dd HH:mm"))
Commit:        $(git -C $root log -1 --format="%h %s" HEAD)
Tags:          $((git -C $root tag --points-at HEAD) -join ", ")
Uncommitted:   $(if ($dirty) { "yes - this build includes changes that were not committed" } else { "no" })
RT64:          $(git -C (Join-Path $deps "rt64") log -1 --format="%h" HEAD)
RecompFrontend: $(if (Test-Path (Join-Path $deps "RecompFrontend")) { git -C (Join-Path $deps "RecompFrontend") log -1 --format="%h" HEAD } else { "-" })

First run (no ROM or saves included): adam.exe --rom $(Join-Path $root "rom\game.z64")
Builds with the launcher can also use Load ROM there. Saves go to data\saves\ in this folder.
"@ | Set-Content -Encoding UTF8 (Join-Path $dest "SNAPSHOT.txt")

"Snapshot: $dest"
