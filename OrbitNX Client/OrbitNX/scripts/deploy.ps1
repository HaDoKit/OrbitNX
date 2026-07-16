# Non-interactive deploy for VS Code tasks / automation.
# Sends the built .nro to the console via nxlink, same tool SendToSwitch.bat
# uses for manual double-click use. IP resolution order:
#   1. $env:ORBITNX_SWITCH_IP
#   2. switch-ip.txt next to this project (first non-empty line)
#   3. fallback default (matches the IP hardcoded in SendToSwitch.bat)

$ErrorActionPreference = "Stop"

$DefaultIp = "192.168.1.111"
$projectRoot = Split-Path -Parent $PSScriptRoot
$ipFile = Join-Path $projectRoot "switch-ip.txt"

function Get-SwitchIp {
    if ($env:ORBITNX_SWITCH_IP) {
        return $env:ORBITNX_SWITCH_IP.Trim()
    }

    if (Test-Path $ipFile) {
        $line = Get-Content $ipFile | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith("#") } | Select-Object -First 1
        if ($line) {
            return $line.Trim()
        }
    }
    else {
        # First run: seed switch-ip.txt with the default so it becomes the
        # single source of truth for future deploys from this machine.
        @(
            "# IP address of the Nintendo Switch this machine deploys/tests against."
            "# Also enter this same IP into OrbitNX's Settings on the PC side."
            $DefaultIp
        ) | Set-Content -Path $ipFile -Encoding utf8
        Write-Host "Created $ipFile with default IP $DefaultIp - edit it to point at your console."
    }

    return $DefaultIp
}

$switchIp = Get-SwitchIp
$nro = Join-Path $projectRoot "OrbitNX.nro"

if (-not (Test-Path $nro)) {
    Write-Error "OrbitNX.nro not found at $nro - build first (scripts/build.ps1 or Ctrl+Shift+B)."
    exit 1
}

$nxlink = Get-Command "nxlink.exe" -ErrorAction SilentlyContinue
if (-not $nxlink) {
    # Matches the path SendToSwitch.bat uses directly.
    $nxlink = "C:\devkitPro\tools\bin\nxlink.exe"
    if (-not (Test-Path $nxlink)) {
        Write-Error "nxlink.exe not found on PATH or at $nxlink - check your devkitPro install."
        exit 1
    }
}
else {
    $nxlink = $nxlink.Source
}

Write-Host "Deploying $nro to $switchIp via nxlink..."
& $nxlink -a $switchIp -s $nro
if ($LASTEXITCODE -ne 0) {
    Write-Error "nxlink failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}
