# Builds SoH with our co-op code in it.
#   powershell -ExecutionPolicy Bypass -File scripts\build-soh.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\build-soh.ps1 -Config Release
#   powershell -ExecutionPolicy Bypass -File scripts\build-soh.ps1 -Full   # also redo SoH's asset copy
# First run takes a long time (vcpkg + all of SoH). After that, only what changed.
param(
    [ValidateSet("Debug", "Release")][string]$Config = "Debug",
    [switch]$Full
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

# The few hooks we had to add inside SoH (enemy sync needs to see colliders and
# skeletons). Applied once; if it's already there git says so and we move on.
$patch = Join-Path $root "patches\soh-coop-hooks.patch"
# (through cmd so PowerShell 5 doesn't turn git's stderr into an exception)
cmd /c "git -C `"$src`" apply --reverse --check `"$patch`" 2>nul"
if ($LASTEXITCODE) {
    git -C $src apply $patch
    if ($LASTEXITCODE) { throw "Could not apply $patch (SoH files changed by hand?)" }
    Write-Host "Applied patches\soh-coop-hooks.patch"
}

& $cmake -S $src -B $build -G "Visual Studio 17 2022" -T v143 -A x64 "-DCMAKE_PROJECT_soh_INCLUDE=$inject"
if ($LASTEXITCODE) { throw "CMake configure failed" }

if (-not (Test-Path (Join-Path $build "soh\soh.o2r"))) {
    & $cmake --build $build --config $Config --target GenerateSohOtr --parallel
    if ($LASTEXITCODE) { throw "soh.o2r generation failed" }
}

# After each link, SoH copies its 20 000 asset yml files twice (6+ min with the
# antivirus). They never change since SoH is pinned, so skip that once they're there.
$msbuildArgs = @()
$assetCopies = @((Join-Path $build "soh\assets"), (Join-Path $root "external\Shipwright\x64\$Config\assets"))
if (-not $Full -and -not ($assetCopies | Where-Object { -not (Test-Path $_) })) {
    Write-Host "Skipping SoH's asset copy (already done, use -Full to force it)"
    $msbuildArgs = @("--", "/p:PostBuildEventUseInBuild=false")
}

& $cmake --build $build --config $Config --target soh --parallel @msbuildArgs
if ($LASTEXITCODE) { throw "Build failed" }
Write-Host "Done: external\Shipwright\x64\$Config\soh.exe"
