# Packages the live shell (runtime\build-live, build it first: runtime\build.ps1 -Live) as a folder + zip in packages\
# for players, no folders: adam.exe (with the live\ inputs symbols.toml + config.toml - addresses and names, no game
# code - and the menus' assets built in; it unpacks them into data\ at startup), the DLLs it loads, the license and
# runtime\package\HOW_TO_PLAY.txt. Never includes data\ (ROM copy, saves, settings) or debug symbols. packages\ is gitignored.
# Named by the version in runtime\package\VERSION (adam-<version>[-label]); it refuses to overwrite an existing
# version: bump VERSION (and commit) for every release. The version also goes into HOW_TO_PLAY.txt ({VERSION}).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\package_live.ps1 [-Name <label>]
param([string]$Name = "")
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "runtime\build-live"
if (-not (Test-Path (Join-Path $build "adam.exe"))) {
    Write-Error "No live build in runtime\build-live - run runtime\build.ps1 -Live first."
}

$version = (Get-Content (Join-Path $root "runtime\package\VERSION") -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { Write-Error "runtime\package\VERSION must be like 1.2.3, is '$version'" }
$label = "adam-" + $version + $(if ($Name) { "-$Name" } else { "" })
$dest = Join-Path $root "packages\$label"
if ((Test-Path $dest) -or (Test-Path "$dest.zip")) {
    Write-Error "packages\$label already exists - bump runtime\package\VERSION for a new release (or delete the old package)."
}
New-Item -ItemType Directory -Force $dest | Out-Null

foreach ($file in @("adam.exe", "SDL2.dll", "dxil.dll", "dxcompiler.dll")) {
    Copy-Item (Join-Path $build $file) $dest
}
(Get-Content -Raw (Join-Path $root "runtime\package\HOW_TO_PLAY.txt")).Replace("{VERSION}", $version) |
    Set-Content -NoNewline -Encoding utf8 (Join-Path $dest "HOW_TO_PLAY.txt")
Copy-Item (Join-Path (Split-Path $root -Parent) "N64ModernRuntime\COPYING") (Join-Path $dest "LICENSE.txt")

# Nothing generated from the ROM may be in the package.
$forbidden = Get-ChildItem -Recurse $dest | Where-Object { $_.Name -match '\.(z64|n64|v64|bin|eep|pdb)$' -or $_.FullName -match '\\data\\' }
if ($forbidden) { Write-Error "Package contains files that must not be shipped: $($forbidden.FullName -join ', ')" }

$zip = "$dest.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $dest "*") -DestinationPath $zip
"Package: $dest"
"Zip:     $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
