<#
.SYNOPSIS
    Fetches the signed PawnIO module that idimus_hw needs for CPU temperature and
    package power on Windows, and places it where the built executables look for it.

.DESCRIPTION
    Ring-0 MSR reads (CPU temperature + RAPL power) on Windows go through the PawnIO
    signed kernel driver. idimus_hw loads a signed Pawn module at runtime, searching:
        1. %IDIMUS_PAWNIO_DIR%
        2. <exe>\modules
        3. <exe>
    The signed module binary is a release artifact of namazso/PawnIO.Modules and is
    NOT bundled with this repo. This script downloads it (pinned to the same version
    as the third_party/pawnio-modules submodule) and copies the module matching your
    CPU vendor into a 'modules' folder next to each built idimus executable.

    Intel CPUs use IntelMSR.bin; AMD (Family 17h+ / Zen) use AMDFamily17.bin.

    This only installs the *module* consumed by PawnIO. The PawnIO driver itself
    (PawnIOLib.dll) must be installed separately from https://pawnio.eu /
    https://github.com/namazso/PawnIO/releases. The script warns if it is missing.

.PARAMETER Version
    PawnIO.Modules release tag to download. Defaults to the version the submodule
    is pinned to.

.PARAMETER Destination
    Executable directory to install into. A 'modules' subfolder is created inside it
    and the .bin placed there. If omitted, the script auto-discovers built idimus
    executables under the repo and installs next to each.

.PARAMETER Module
    Override the module base name (e.g. 'IntelMSR', 'AMDFamily17'). If omitted, it is
    chosen from the detected CPU vendor.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\windows\setup-pawnio.ps1

.EXAMPLE
    # Install next to a specific build
    .\scripts\windows\setup-pawnio.ps1 -Destination console\build\bin\Release
#>
[CmdletBinding()]
param(
    [string]$Version = "0.1.6",
    [string]$Destination,
    [string]$Module
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Info($m)  { Write-Host "[*] $m" }
function Ok($m)    { Write-Host "[+] $m" -ForegroundColor Green }
function Warn($m)  { Write-Host "[!] $m" -ForegroundColor Yellow }
function Fail($m)  { Write-Host "[x] $m" -ForegroundColor Red; exit 1 }

# Repo root is two levels up from this script (scripts\windows\).
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# --- Pick the module for this CPU -------------------------------------------
if (-not $Module) {
    $vendor = (Get-CimInstance Win32_Processor | Select-Object -First 1).Manufacturer
    switch -Wildcard ($vendor) {
        "*Intel*"       { $Module = "IntelMSR" }
        "*AMD*"         { $Module = "AMDFamily17" }
        "AuthenticAMD"  { $Module = "AMDFamily17" }
        "GenuineIntel"  { $Module = "IntelMSR" }
        default { Fail "Unrecognized CPU vendor '$vendor'. Pass -Module explicitly (e.g. -Module IntelMSR)." }
    }
    Info "CPU vendor '$vendor' -> module $Module.bin"
} else {
    Info "Using module $Module.bin (override)"
}

# --- Warn if the PawnIO driver isn't installed ------------------------------
$pawnLib = @(
    "$env:ProgramFiles\PawnIO\PawnIOLib.dll",
    "C:\Program Files\PawnIO\PawnIOLib.dll"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($pawnLib) {
    Ok "PawnIO driver found: $pawnLib"
} else {
    Warn "PawnIOLib.dll not found. The module will be installed, but CPU temperature/power"
    Warn "will stay 'n/a' until you install the PawnIO driver from https://pawnio.eu"
}

# --- Download + extract the signed module release ---------------------------
$url = "https://github.com/namazso/PawnIO.Modules/releases/download/$Version/release_$($Version -replace '\.','_').zip"
$work = Join-Path $env:TEMP "idimus_pawnio_$Version"
$zip  = "$work.zip"
Info "Downloading PawnIO modules $Version ..."
Info "  $url"
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
Expand-Archive -Path $zip -DestinationPath $work -Force

$binSrc = Get-ChildItem $work -Recurse -Filter "$Module.bin" | Select-Object -First 1
if (-not $binSrc) {
    Fail "$Module.bin not found in release $Version. Available: $((Get-ChildItem $work -Recurse -Filter '*.bin').Name -join ', ')"
}
Ok "Extracted $($binSrc.Name) ($([math]::Round($binSrc.Length/1KB,1)) KB)"

# --- Resolve install targets ------------------------------------------------
$targets = @()
if ($Destination) {
    $targets += (Resolve-Path $Destination).Path
} else {
    Info "Discovering built idimus executables under $RepoRoot ..."
    $exes = Get-ChildItem $RepoRoot -Recurse -File -Include "idimus_monitor.exe","idimus_hw_dump.exe" -ErrorAction SilentlyContinue
    $targets = $exes | ForEach-Object { $_.DirectoryName } | Sort-Object -Unique
    if (-not $targets) {
        Warn "No built executables found. Build first, or pass -Destination <exe folder>."
        Warn "Falling back to installing into the current directory."
        $targets = @((Get-Location).Path)
    }
}

# --- Copy into a 'modules' folder next to each target -----------------------
foreach ($t in $targets) {
    $modDir = Join-Path $t "modules"
    New-Item -ItemType Directory -Force -Path $modDir | Out-Null
    Copy-Item $binSrc.FullName -Destination $modDir -Force
    Ok "Installed -> $(Join-Path $modDir "$Module.bin")"
}

Remove-Item $zip -Force -ErrorAction SilentlyContinue
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Ok "Done. Run the monitor elevated (admin) to read CPU temperature/power:"
Write-Host "    sudo <path>\idimus_monitor.exe" -ForegroundColor Cyan
