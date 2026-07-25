# Non-interactive build for VS Code tasks / automation.
# Mirrors build.bat's `make` invocation, but without the pause/retry loop
# build.bat uses for manual double-click use — that loop would hang a task runner.

$ErrorActionPreference = "Stop"

if (-not $env:DEVKITPRO) {
    Write-Error "DEVKITPRO is not set. Install/configure devkitPro (see setup.bat) and ensure DEVKITPRO points at the install root."
    exit 1
}

# devkitPro's Makefile shells out to tools like sdl2-config via `` `$(DEVKITPRO)/...` `` -
# a backslash-style DEVKITPRO (C:\devkitPro) gets silently mangled to "C:devkitPro" when
# that reaches /bin/sh, so always use forward slashes here regardless of how the caller's
# environment has it set.
$env:DEVKITPRO = $env:DEVKITPRO -replace '\\', '/'
if ($env:DEVKITA64) { $env:DEVKITA64 = $env:DEVKITA64 -replace '\\', '/' }

$projectRoot = Split-Path -Parent $PSScriptRoot

# devkitPro's recursive `make -C build -f $(CURDIR)/Makefile` breaks on a space anywhere
# in the path (corrupts CURDIR mid-string, e.g. ".../visual code project/..." -> fails
# looking for ".../visual") - this reproduces identically via devkitPro's own bundled
# msys2 make, not just Git Bash, so it's not avoidable by choosing a different shell.
# Work around it with a temporary space-free subst drive rather than requiring the
# repo to live at a space-free path.
$needsSubst = $projectRoot -match ' '
$buildRoot = $projectRoot
$substDrive = $null

if ($needsSubst) {
    $substDrive = "X:"
    if (Test-Path "${substDrive}\") {
        Write-Error "$substDrive is already in use - free it or edit this script to use a different drive letter."
        exit 1
    }
    # Find the shortest prefix to subst such that everything BELOW it is space-free -
    # substituting any deeper would still leave a space in the remainder, substituting
    # shallower is unnecessary.
    $parts = $projectRoot -split '\\'
    $cutIndex = $parts.Length
    for ($i = $parts.Length - 1; $i -ge 0; $i--) {
        if ($parts[$i] -match ' ') { break }
        $cutIndex = $i
    }
    $substSource = ($parts[0..($cutIndex - 1)] -join '\')
    subst $substDrive $substSource | Out-Null
    $buildRoot = $projectRoot.Replace($substSource, $substDrive)
    Write-Host "Path contains spaces - building via temporary $substDrive => $substSource"
}

try {
    Push-Location $buildRoot
    try {
        # make's own stderr (gcc diagnostics, even harmless warnings) must not be treated as a
        # terminating error here - with the global $ErrorActionPreference = "Stop" above, any
        # stderr line from `& make` throws a NativeCommandError before $LASTEXITCODE is ever
        # checked below, making a successful build look like a failure. Only the explicit
        # $LASTEXITCODE check below should decide success/failure for this call.
        $previousEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & make
        $ErrorActionPreference = $previousEap
        if ($LASTEXITCODE -ne 0) {
            Write-Error "make failed with exit code $LASTEXITCODE"
            exit $LASTEXITCODE
        }
    }
    finally {
        Pop-Location
    }
}
finally {
    if ($substDrive) {
        subst $substDrive /D | Out-Null
    }
}
