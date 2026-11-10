# Builds SoH with our co-op code in it.
#   powershell -ExecutionPolicy Bypass -File scripts\build-soh.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\build-soh.ps1 -Config RelWithDebInfo
# First run takes a long time (vcpkg + all of SoH). After that, only what changed.
param(
    [ValidateSet("Debug", "RelWithDebInfo", "Release")][string]$Config = "Debug"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { throw "CMake not found at $cmake (install the C++ workload of VS 2022 Build Tools)" }

# vcpkg picks the newest Visual Studio by default, and the 2026 one on this
# machine can't build 64-bit. Point it at 2022.
$env:VCPKG_VISUAL_STUDIO_PATH = $vs

$src = Join-Path $root "external\Shipwright"
$build = Join-Path $root "build\soh"
$inject = (Join-Path $root "cmake\coop-inject.cmake") -replace '\\', '/'

& $cmake -S $src -B $build -G "Visual Studio 17 2022" -T v143 -A x64 "-DCMAKE_PROJECT_soh_INCLUDE=$inject"
if ($LASTEXITCODE) { throw "CMake configure failed" }

if (-not (Test-Path (Join-Path $build "soh\soh.o2r"))) {
    & $cmake --build $build --config $Config --target GenerateSohOtr --parallel
    if ($LASTEXITCODE) { throw "soh.o2r generation failed" }
}

& $cmake --build $build --config $Config --target soh --parallel
if ($LASTEXITCODE) { throw "Build failed" }
Write-Host "Done: external\Shipwright\x64\$Config\soh.exe"
