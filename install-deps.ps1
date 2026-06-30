# ─────────────────────────────────────────────────────────────────────
# WetGregFirmware — Windows Development Dependencies Installer
#
# Mirrors install-deps.sh for Windows 10/11. Checks for each dep,
# installs what's missing, makes sure Docker Desktop is running, then
# launches the DevTool.
#
#   Docker Desktop        winget Docker.DockerDesktop
#   Python 3 + Tkinter    winget Python.Python.3.12   (Tk ships with python.org build)
#   Git                   winget Git.Git
#   picotool              pico-sdk-tools release zip  → %USERPROFILE%\.picotool\
#   pyserial              pip --user
#
# Usage (from this directory):
#   powershell -ExecutionPolicy Bypass -File .\install-deps.ps1
#   powershell -ExecutionPolicy Bypass -File .\install-deps.ps1 -SkipLaunch
#   powershell -ExecutionPolicy Bypass -File .\install-deps.ps1 -CheckOnly
# ─────────────────────────────────────────────────────────────────────

[CmdletBinding()]
param(
    [switch]$SkipLaunch,
    [switch]$CheckOnly,
    [int]$DockerStartTimeoutSec = 180
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Info($m) { Write-Host "[info] $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "[ok]   $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[warn] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[fail] $m" -ForegroundColor Red }

function Test-Cmd($name) { [bool](Get-Command $name -ErrorAction SilentlyContinue) }

function Invoke-Winget($id, $label) {
    if (-not (Test-Cmd 'winget')) { Fail "winget missing — install '$label' manually."; return $false }
    Info "winget install $id"
    winget install --id $id --exact --silent --accept-package-agreements --accept-source-agreements
    return ($LASTEXITCODE -eq 0)
}

# ── 1. Git ────────────────────────────────────────────────────────────
if (Test-Cmd 'git') { Ok "git: $(git --version)" }
else {
    if ($CheckOnly) { Fail "git missing" }
    else { Invoke-Winget 'Git.Git' 'Git' | Out-Null; $env:Path = "$env:Path;C:\Program Files\Git\cmd" }
}

# ── 2. Python 3 + Tkinter ─────────────────────────────────────────────
$pythonCmd = $null
foreach ($p in @('python','python3','py')) {
    if (Test-Cmd $p) {
        $v = & $p --version 2>&1
        if ($v -match 'Python 3') { $pythonCmd = $p; break }
    }
}
if ($pythonCmd) { Ok "python: $(& $pythonCmd --version)" }
else {
    if ($CheckOnly) { Fail "Python 3 missing" }
    else { Invoke-Winget 'Python.Python.3.12' 'Python 3.12' | Out-Null; $pythonCmd = 'python' }
}

if ($pythonCmd) {
    & $pythonCmd -c "import tkinter" 2>$null
    if ($LASTEXITCODE -eq 0) { Ok "tkinter present" }
    else { Warn "tkinter missing — reinstall Python from python.org with the 'tcl/tk and IDLE' option ticked." }
}

# ── 3. Docker Desktop ─────────────────────────────────────────────────
if (Test-Cmd 'docker') {
    Ok "docker CLI: $((docker --version) 2>&1)"
} else {
    if ($CheckOnly) { Fail "Docker Desktop missing" }
    else {
        Invoke-Winget 'Docker.DockerDesktop' 'Docker Desktop' | Out-Null
        Warn "Docker Desktop installed — you may need to sign out/in once, then re-run this script."
    }
}

function Test-DockerDaemon {
    try { docker info --format '{{.ServerVersion}}' 2>$null | Out-Null; return ($LASTEXITCODE -eq 0) }
    catch { return $false }
}

function Get-DockerOsType {
    try { return (docker info --format '{{.OSType}}' 2>$null) } catch { return $null }
}

function Switch-DockerToLinux {
    $cli = "$env:ProgramFiles\Docker\Docker\DockerCli.exe"
    if (-not (Test-Path $cli)) { Warn "DockerCli.exe not at $cli — switch manually via the tray icon."; return $false }
    Info "Switching Docker Desktop to Linux containers..."
    & $cli -SwitchLinuxEngine | Out-Null
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline) {
        if ((Get-DockerOsType) -eq 'linux') { Ok "Docker now in Linux containers mode"; return $true }
        Start-Sleep -Seconds 3
    }
    Warn "Engine did not report OSType=linux within 120s — check Docker Desktop manually."
    return $false
}

if (Test-Cmd 'docker' -and -not $CheckOnly) {
    if (Test-DockerDaemon) { Ok "Docker daemon is running" }
    else {
        Info "Starting Docker Desktop..."
        $dd = "$env:ProgramFiles\Docker\Docker\Docker Desktop.exe"
        if (Test-Path $dd) {
            Start-Process -FilePath $dd | Out-Null
            $deadline = (Get-Date).AddSeconds($DockerStartTimeoutSec)
            $ready = $false
            while ((Get-Date) -lt $deadline) {
                if (Test-DockerDaemon) { $ready = $true; break }
                Start-Sleep -Seconds 3
            }
            if ($ready) { Ok "Docker daemon ready" }
            else { Warn "Docker daemon did not respond within $DockerStartTimeoutSec s — open Docker Desktop manually and wait for 'Engine running'." }
        } else { Warn "Docker Desktop executable not at expected path: $dd" }
    }

    # The firmware build uses ubuntu:24.04 — engine MUST be in Linux containers mode.
    if (Test-DockerDaemon) {
        $os = Get-DockerOsType
        if ($os -eq 'linux') { Ok "Docker engine mode: linux" }
        elseif ($os) { Warn "Docker engine is in '$os' containers mode — firmware build needs Linux."; Switch-DockerToLinux | Out-Null }
        else { Warn "Could not determine Docker OSType." }
    }
}

# ── 4. picotool ───────────────────────────────────────────────────────
# Raspberry Pi publishes prebuilt Windows picotool in pico-sdk-tools.
$PicotoolDir = Join-Path $env:USERPROFILE '.picotool'
$PicotoolExe = Join-Path $PicotoolDir 'picotool.exe'

function Resolve-Picotool {
    if (Test-Cmd 'picotool') { return (Get-Command picotool).Source }
    if (Test-Path $PicotoolExe) { return $PicotoolExe }
    return $null
}

$pt = Resolve-Picotool
if ($pt) { Ok "picotool: $pt" }
elseif (-not $CheckOnly) {
    Info "Downloading prebuilt picotool from raspberrypi/pico-sdk-tools..."
    try {
        $rel = Invoke-RestMethod 'https://api.github.com/repos/raspberrypi/pico-sdk-tools/releases/latest' -Headers @{'User-Agent'='wetgreg-installer'}
        $asset = $rel.assets | Where-Object { $_.name -match 'picotool.*win.*x64.*\.zip$' -or $_.name -match 'picotool.*x64.*win.*\.zip$' } | Select-Object -First 1
        if (-not $asset) { $asset = $rel.assets | Where-Object { $_.name -match 'picotool.*\.zip$' -and $_.name -match 'win' } | Select-Object -First 1 }
        if (-not $asset) { throw "no Windows picotool asset found in release $($rel.tag_name)" }

        New-Item -ItemType Directory -Force -Path $PicotoolDir | Out-Null
        $zip = Join-Path $env:TEMP $asset.name
        Info "  $($asset.name)"
        Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -UseBasicParsing
        Expand-Archive -Path $zip -DestinationPath $PicotoolDir -Force
        Remove-Item $zip -Force

        # The zip usually unpacks into a subdir like picotool/picotool.exe — flatten.
        $found = Get-ChildItem -Path $PicotoolDir -Recurse -Filter 'picotool.exe' | Select-Object -First 1
        if ($found -and ($found.FullName -ne $PicotoolExe)) {
            Copy-Item $found.FullName $PicotoolExe -Force
        }

        if (Test-Path $PicotoolExe) {
            $userPath = [Environment]::GetEnvironmentVariable('Path','User')
            if ($userPath -notlike "*$PicotoolDir*") {
                [Environment]::SetEnvironmentVariable('Path', "$userPath;$PicotoolDir", 'User')
                Ok "Added $PicotoolDir to user PATH (new shells will pick it up)"
            }
            $env:Path = "$env:Path;$PicotoolDir"
            Ok "picotool installed: $PicotoolExe"
        } else { Warn "picotool.exe not found after extraction — inspect $PicotoolDir" }
    } catch {
        Warn "Automatic picotool install failed: $($_.Exception.Message)"
        Warn "Manual route: download a Windows build from https://github.com/raspberrypi/pico-sdk-tools/releases"
        Warn "and drop picotool.exe into $PicotoolDir, or use BOOTSEL-button + drag-and-drop flashing."
    }
}

# ── 5. Git submodules ─────────────────────────────────────────────────
if (Test-Cmd 'git' -and -not $CheckOnly) {
    Info "Initialising git submodules (FreeRTOS-Kernel, picowota)..."
    Push-Location $RepoRoot
    try { git submodule update --init --recursive; Ok "Submodules initialised" }
    catch { Warn "submodule init failed: $($_.Exception.Message)" }
    finally { Pop-Location }
}

# ── 6. pyserial (for DevTool serial monitor) ──────────────────────────
if ($pythonCmd -and -not $CheckOnly) {
    Info "Installing pyserial..."
    & $pythonCmd -m pip install --user --quiet pyserial
    if ($LASTEXITCODE -eq 0) { Ok "pyserial installed" } else { Warn "pyserial install returned $LASTEXITCODE" }
}

# ── 7. Note about serial monitor on Windows ───────────────────────────
Warn "DevTool's serial monitor probes /dev/ttyACM* — that's Linux-only."
Warn "Build, flash, device-info, reboot, erase all work on Windows."
Warn "For serial output use PuTTY/Tera Term on the Pico's assigned COM port."

# ── 8. Launch the DevTool ─────────────────────────────────────────────
$DevTool = Join-Path $RepoRoot 'tools\devtool\devtool.py'
if ($CheckOnly) {
    Info "Check-only mode — not launching DevTool."
} elseif ($SkipLaunch) {
    Info "-SkipLaunch set — DevTool not launched."
    Info "Run manually: $pythonCmd `"$DevTool`""
} elseif (Test-Path $DevTool) {
    Info "Launching DevTool GUI..."
    if (-not (Test-DockerDaemon)) {
        Warn "Docker daemon still not ready — build will fail until 'Engine running' shows in Docker Desktop."
    }
    Start-Process -FilePath $pythonCmd -ArgumentList "`"$DevTool`"" -WorkingDirectory $RepoRoot
    Ok "DevTool launched — Flash tab → Clean Build & Flash"
} else { Warn "DevTool not found at $DevTool" }

Write-Host ""
Write-Host "════════════════════════════════════════"  -ForegroundColor Green
Write-Host "  WetGregFirmware setup pass complete"     -ForegroundColor Green
Write-Host "════════════════════════════════════════"  -ForegroundColor Green
Write-Host "Flashing tips:"
Write-Host "  • Plug the Wet Greg in over USB."
Write-Host "  • If picotool is installed, the DevTool reboots the board into BOOTSEL automatically."
Write-Host "  • Otherwise: hold BOOTSEL, plug in → board mounts as drive 'RP2350' → DevTool copies the .uf2."
