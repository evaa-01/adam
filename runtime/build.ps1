# Builds runtime\build\adam.exe with clang-cl + Ninja (same toolchain as Zelda64Recomp).
# Usage: powershell -ExecutionPolicy Bypass -File runtime\build.ps1 [-Config Debug|RelWithDebInfo|Release] [-Clean] [-Live]
# -Live builds the live shell (no recompiled game code; see src\live.cpp) into runtime\build-live instead.
param(
    [string]$Config = "RelWithDebInfo",
    [switch]$Clean,
    [switch]$Live
)
$ErrorActionPreference = "Stop"

$vsRoot = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = "$vsRoot\VC\Auxiliary\Build\vcvars64.bat"
$ninja  = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$llvm   = "C:\Program Files\LLVM\bin"

# Import the VS developer environment (INCLUDE, LIB, PATH...) into this PowerShell session.
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$env:PATH = "$llvm;" + $env:PATH

$src = $PSScriptRoot
$build = Join-Path $src $(if ($Live) { "build-live" } else { "build" })
$liveFlag = if ($Live) { "-DADAM_LIVE=ON" } else { "-DADAM_LIVE=OFF" }
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

# Our changes to the checkouts outside this repo live in runtime\patches\: rt64-*.patch for RT64, recompfrontend-*.patch
# for RecompFrontend, n64modernruntime-*.patch for N64ModernRuntime. All are checked out next to the project folder
# (..\rt64, ..\RecompFrontend, ..\N64ModernRuntime), like CMakeLists.txt expects. Apply each patch that isn't applied yet; stop if one doesn't fit (checkout at another commit?).
# (git's stderr output would be a terminating error under "Stop", and the "already applied?" check fails by design.)
$deps = Split-Path (Split-Path $src -Parent) -Parent
$patchTargets = @{ "rt64" = (Join-Path $deps "rt64"); "recompfrontend" = (Join-Path $deps "RecompFrontend"); "n64modernruntime" = (Join-Path $deps "N64ModernRuntime") }
foreach ($prefix in $patchTargets.Keys) {
    $repo = $patchTargets[$prefix]
    foreach ($patch in Get-ChildItem (Join-Path $src "patches\$prefix-*.patch")) {
        $ErrorActionPreference = "Continue"
        git -C $repo apply --reverse --check $patch.FullName 2>$null
        $applied = ($LASTEXITCODE -eq 0)
        if (-not $applied) { git -C $repo apply $patch.FullName }
        $ok = $applied -or ($LASTEXITCODE -eq 0)
        $ErrorActionPreference = "Stop"
        if (-not $ok) { Write-Error "Could not apply $($patch.Name) to $repo" }
        if (-not $applied) { "Applied $($patch.Name) to $repo" }
    }
}

cmake -S $src -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl "-DCMAKE_BUILD_TYPE=$Config" $liveFlag
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $build
exit $LASTEXITCODE
