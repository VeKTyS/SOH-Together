# Launches our SoH build in its own folder (build/run/<Instance>), so saves and
# settings never mix with a regular SoH install.
# SoH on Windows looks for oot.o2r in the current directory first, then next to the exe.
param(
    [string]$Instance = "p1",
    [string]$OotArchive = "C:\Games\OOT\oot.o2r",
    [ValidateSet("Debug", "Release")][string]$Config = "Debug"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "external\Shipwright\x64\$Config\soh.exe"
$sohArchive = Join-Path $root "build\soh\soh\soh.o2r"
$runDir = Join-Path $root "build\run\$Instance"

if (-not (Test-Path $exe)) { throw "soh.exe not found: $exe (build SoH first)" }
if (-not (Test-Path $sohArchive)) { throw "soh.o2r not found: $sohArchive (build target GenerateSohOtr)" }
if (-not (Test-Path $OotArchive)) { throw "oot.o2r not found: $OotArchive (pass -OotArchive)" }

New-Item -ItemType Directory -Force $runDir | Out-Null
# oot.o2r is copied once (extracted from your ROM, never committed).
if (-not (Test-Path "$runDir\oot.o2r")) { Copy-Item $OotArchive "$runDir\oot.o2r" }
# soh.o2r is refreshed on every launch: it changes with each build.
Copy-Item $sohArchive "$runDir\soh.o2r" -Force

Write-Host "Launching $exe in $runDir"
Start-Process -FilePath $exe -WorkingDirectory $runDir
