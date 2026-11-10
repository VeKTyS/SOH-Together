# Makes a zip to drop into an existing Ship of Harkinian 9.3.1 folder: it only
# adds SOH-Together.exe next to soh.exe, so the mods, config, saves, oot.o2r and
# soh.o2r already there are used as they are (SoH only checks soh.o2r's version
# number, and ours is the same 9.3.1).
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1
# Release by default (SoH only has Debug and Release). Debug stops on asserts
# that a normal SoH skips (a texture pack missing a piece of a model crashes it),
# and it's way slower.
# Never put oot.o2r / the ROM in there: that's Nintendo's stuff, everyone uses their own.
param(
    [ValidateSet("Debug", "Release")][string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "external\Shipwright\x64\$Config\soh.exe"
if (-not (Test-Path $exe)) { throw "Missing $exe, build first (scripts\build-soh.ps1 -Config $Config)" }

$id = (git -C $root rev-parse --short=12 HEAD).Trim()
$name = "SOH-Together-$id-$Config"
$out = Join-Path $root "build\package\$name"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

# renamed so the normal soh.exe stays there and still works
Copy-Item $exe (Join-Path $out "SOH-Together.exe")

@"
SOH-Together test build ($id, $Config)

1. Drop SOH-Together.exe into your Ship of Harkinian 9.3.1 folder, next to
   soh.exe. Nothing gets replaced: your mods, config and saves are used as they are.
2. Start SOH-Together.exe (soh.exe is still the normal game).
3. Open the menu (Esc) > Network > Co-op, pick a name.
4. The host clicks "Host a game" and gives you the address shown.
   You paste it in "Their address" and click "Join".

If Windows asks about the firewall, allow it (private networks).
Not on the same network? The host must open UDP port 27720 on their router,
or you both install Tailscale and use the Tailscale address.

Both players must use this exact same SOH-Together.exe, otherwise the
connection is refused with "Different game build".
"@ | Set-Content -Encoding utf8 (Join-Path $out "README.txt")

$zip = "$out.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# Windows' own tar makes zips much faster than Compress-Archive
tar.exe -a -c -f $zip -C $out SOH-Together.exe README.txt
if ($LASTEXITCODE) { throw "zip failed" }
Write-Host "Package: $zip"
