# Makes a zip to send to a friend so they can test co-op on their PC.
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1
# Never put oot.o2r / the ROM in there: that's Nintendo's stuff, everyone uses their own.
param(
    [ValidateSet("Debug", "RelWithDebInfo", "Release")][string]$Config = "Debug"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "external\Shipwright\x64\$Config\soh.exe"
$sohArchive = Join-Path $root "build\soh\soh\soh.o2r"
foreach ($f in $exe, $sohArchive) { if (-not (Test-Path $f)) { throw "Missing $f, build first (scripts\build-soh.ps1)" } }

$id = (git -C $root rev-parse --short=12 HEAD).Trim()
$name = "SOH-Together-$id-$Config"
$out = Join-Path $root "build\package\$name"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

Copy-Item $exe, $sohArchive $out
# SoH needs these yml files to turn the friend's ROM into oot.o2r on first launch
$assets = Join-Path (Split-Path $exe) "assets"
if (-not (Test-Path $assets)) { throw "Missing $assets (the build copies it at the end, let it finish)" }
Copy-Item -Recurse $assets (Join-Path $out "assets")
# SoH's CMake downloads this one at configure time (gamepad mappings)
$controllerDb = Join-Path $root "build\soh\gamecontrollerdb.txt"
if (Test-Path $controllerDb) { Copy-Item $controllerDb $out }

@"
SOH-Together test build ($id, $Config)

1. Put this folder anywhere (not inside your usual Ship of Harkinian folder).
2. Start soh.exe. The first time it asks for your Ocarina of Time ROM and
   builds oot.o2r from it (takes a minute).
3. Open the menu (Esc) > Network > Co-op, pick a name.
4. The host clicks "Host a game" and gives you the address shown.
   You paste it in "Their address" and click "Join".

If Windows asks about the firewall, allow it (private networks).
Not on the same network? The host must open UDP port 27720 on their router,
or you both install Tailscale and use the Tailscale address.

Both players must use this exact same soh.exe, otherwise the connection is
refused with "Different game build".
"@ | Set-Content -Encoding utf8 (Join-Path $out "README.txt")

$zip = "$out.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$out\*" -DestinationPath $zip
Write-Host "Package: $zip"
