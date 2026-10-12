$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent   # project folder (scripts\..)
Set-Location (Join-Path $root "splat")
python -m splat split adam.yaml | Out-Null
Set-Location $root
python scripts\splat_to_toml.py
Copy-Item symbols.splat.toml symbols.toml -Force
python scripts\update_stubs.py | Select-Object -First 1
# Clear old output: N64Recomp doesn't delete funcs_N.c files from earlier runs, and the runtime
# build compiles every .c in the folder (stale files call functions by outdated names).
if (Test-Path RecompiledFuncs) { Remove-Item RecompiledFuncs\* -Force }
.\N64Recomp.exe config.toml
"exit code: $LASTEXITCODE"
