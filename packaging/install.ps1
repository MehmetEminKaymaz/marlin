# Marlin installer for Windows.
#
#   irm https://github.com/MehmetEminKaymaz/marlin/releases/latest/download/install.ps1 | iex
#
# Downloads the compiler package from the GitHub release, checks its SHA-256,
# unpacks it into %LOCALAPPDATA%\Programs\Marlin (replacing an older version),
# adds its bin directory to your user PATH and runs `marlin doctor`.
# Running it again updates Marlin.
#
# Options (environment variables, set before running):
#   $env:MARLIN_VERSION = "0.1.0"     install this version instead of the latest
#   $env:MARLIN_HOME = "D:\marlin"    install directory
#   $env:MARLIN_NO_MODIFY_PATH = "1"  do not change PATH
#   $env:MARLIN_INSTALL_FROM = "..."  install this package archive (for testing)
#   $env:MARLIN_UNINSTALL = "1"       remove Marlin
#
# Marlin links programs with clang and the Microsoft C libraries: Visual
# Studio Build Tools with the C++ and Clang components must be installed
# (`marlin doctor` checks and prints the command).
#
# Works with Windows PowerShell 5.1 and PowerShell 7.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # the progress bar makes downloads very slow in 5.1

$Repo = 'MehmetEminKaymaz/marlin'
$Asset = 'marlin-windows-x86_64.zip'
$HomeDir = if ($env:MARLIN_HOME) { $env:MARLIN_HOME } else { Join-Path $env:LOCALAPPDATA 'Programs\Marlin' }
$BinDir = Join-Path $HomeDir 'bin'

function Fail($message) {
    Write-Host "marlin install: $message" -ForegroundColor Red
    throw "marlin install failed"
}

function Get-UserPath { [Environment]::GetEnvironmentVariable('Path', 'User') }

function Set-UserPathWithout($dir) {
    $parts = @((Get-UserPath) -split ';' | Where-Object { $_ -and ($_.TrimEnd('\') -ne $dir.TrimEnd('\')) })
    [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'User')
}

if ($env:MARLIN_UNINSTALL -eq '1') {
    if (Test-Path $HomeDir) { Remove-Item -Recurse -Force $HomeDir }
    Set-UserPathWithout $BinDir
    Write-Host "Marlin removed ($HomeDir)."
    return
}

if (-not [Environment]::Is64BitOperatingSystem -or $env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {
    Fail 'only 64-bit x86 Windows is supported for now'
}

# Windows PowerShell 5.1 does not enable TLS 1.2 by default.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

if ($env:MARLIN_VERSION) {
    $Base = "https://github.com/$Repo/releases/download/v$($env:MARLIN_VERSION.TrimStart('v'))"
} else {
    $Base = "https://github.com/$Repo/releases/latest/download"
}

$Tmp = Join-Path ([IO.Path]::GetTempPath()) ("marlin-install-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $Tmp | Out-Null
try {
    # --- download and verify ---------------------------------------------------
    $Archive = Join-Path $Tmp $Asset
    if ($env:MARLIN_INSTALL_FROM) {
        Write-Host "Installing from $env:MARLIN_INSTALL_FROM"
        Copy-Item $env:MARLIN_INSTALL_FROM $Archive
    } else {
        Write-Host "Downloading $Asset ..."
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "$Base/$Asset" -OutFile $Archive
            Invoke-WebRequest -UseBasicParsing -Uri "$Base/SHA256SUMS" -OutFile (Join-Path $Tmp 'SHA256SUMS')
        } catch {
            Fail "download failed from $Base ($($_.Exception.Message))"
        }
        $line = Get-Content (Join-Path $Tmp 'SHA256SUMS') | Where-Object { $_ -match " $([regex]::Escape($Asset))$" }
        if (-not $line) { Fail "$Asset is not listed in SHA256SUMS" }
        $expected = ($line -split ' ')[0]
        $actual = (Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLower()
        if ($actual -ne $expected.ToLower()) { Fail "checksum mismatch for $Asset; try again" }
    }

    # --- unpack ------------------------------------------------------------------
    # The archive has one top-level directory (marlin-<version>-windows-x86_64\).
    $Unpacked = Join-Path $Tmp 'unpacked'
    Expand-Archive -Path $Archive -DestinationPath $Unpacked
    $Top = Get-ChildItem $Unpacked -Directory | Select-Object -First 1
    if (-not $Top -or -not (Test-Path (Join-Path $Top.FullName 'bin\marlin.exe'))) { Fail 'unexpected package layout' }

    $Parent = Split-Path $HomeDir -Parent
    if (-not (Test-Path $Parent)) { New-Item -ItemType Directory -Path $Parent | Out-Null }
    if (Test-Path $HomeDir) {
        try { Remove-Item -Recurse -Force $HomeDir }
        catch { Fail "could not replace $HomeDir (is marlin or VS Code using it? close them and try again)" }
    }
    Move-Item $Top.FullName $HomeDir
} finally {
    Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
}

$Marlin = Join-Path $BinDir 'marlin.exe'
Write-Host "Installed $(& $Marlin --version) into $HomeDir"

# --- PATH -------------------------------------------------------------------------
if (-not $env:MARLIN_NO_MODIFY_PATH) {
    $userPath = Get-UserPath
    $present = @($userPath -split ';' | Where-Object { $_.TrimEnd('\') -eq $BinDir.TrimEnd('\') }).Count -gt 0
    if (-not $present) {
        [Environment]::SetEnvironmentVariable('Path', ((@($userPath, $BinDir) | Where-Object { $_ }) -join ';'), 'User')
        Write-Host "Added $BinDir to your user PATH. Open a new terminal to use 'marlin'."
    }
    if (-not (($env:Path -split ';') -contains $BinDir)) { $env:Path = "$env:Path;$BinDir" }
}

# --- check ------------------------------------------------------------------------
Write-Host ''
& $Marlin doctor
