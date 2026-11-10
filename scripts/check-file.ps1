# Compiles one co-op file (no link) to check it builds, without touching the
# real build: several of these can run at the same time.
#   powershell -ExecutionPolicy Bypass -File scripts\check-file.ps1 src\soh\Network\Coop\CoopMinimap.cpp
param([Parameter(Mandatory = $true)][string]$File)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
$full = (Resolve-Path (Join-Path $root $File)).Path
$name = [IO.Path]::GetFileNameWithoutExtension($full)
$out = Join-Path $root "build\check\$name"
New-Item -ItemType Directory -Force $out | Out-Null
& $msbuild (Join-Path $root "build\soh\soh\soht_coop.vcxproj") -t:ClCompile -p:Configuration=Release -p:Platform=x64 `
    "-p:SelectedFiles=$full" "-p:CoopCheckDir=$out" "-p:IntDir=$out\int\" `
    -p:BuildProjectReferences=false "-p:ForceImportBeforeCppTargets=$(Join-Path $root 'tools\check-one.props')" -nologo -v:m
exit $LASTEXITCODE
