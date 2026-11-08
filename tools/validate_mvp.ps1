# Smoke suite entry for mjdump: builds the products, then runs the six
# live smoke scripts. Migrated from the morpher mjt-dump line, where this
# file drove script/validate_release.ps1 -Layer dump; the mjdump repo has
# no release pipeline, so the equivalent steps run inline below.
param(
    [ValidateSet("debug", "release")]
    [string]$Mode = "release",
    [switch]$SkipUnit,
    [switch]$SkipCliSmoke,
    [switch]$ContinueOnError
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$mjdumpExe = Join-Path $repoRoot "build\windows\x64\$Mode\mjdump.exe"
$failures = [System.Collections.Generic.List[string]]::new()

function Invoke-SmokeStep {
    param([string]$Label, [scriptblock]$Action)
    try {
        & $Action
        Write-Host "[ok] $Label"
    }
    catch {
        if ($ContinueOnError) {
            $failures.Add("$Label : $($_.Exception.Message)") | Out-Null
            Write-Host "[fail] $Label : $($_.Exception.Message)"
        }
        else {
            throw
        }
    }
}

Invoke-SmokeStep "build mjserver/mjdump (xmake $Mode)" {
    & xmake f -m $Mode --with_tests=y -y
    if ($LASTEXITCODE -ne 0) {
        throw "xmake configure failed"
    }
    foreach ($target in @("mjserver", "mjdump", "mjdump-unittest")) {
        & xmake build $target
        if ($LASTEXITCODE -ne 0) {
            throw "xmake build $target failed"
        }
    }
    if (-not $SkipUnit) {
        & xmake run mjdump-unittest
        if ($LASTEXITCODE -ne 0) {
            throw "mjdump-unittest failed"
        }
    }
}

if (-not $SkipCliSmoke) {
    Invoke-SmokeStep "cli help smoke" {
        & $mjdumpExe --help | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "mjdump --help exited $LASTEXITCODE"
        }
    }
}

# Grouping mirrors the morpher dump layer: four scripts were the live-smoke
# gate, two more ran against real JDKs (real apps + P2 external matrix).
$liveSmoke = @(
    "smoke_p0_agent_args.ps1",
    "smoke_inspect.ps1",
    "smoke_mvp_practical.ps1",
    "smoke_mvp_scenarios.ps1"
)
$realSamples = @(
    "smoke_real_apps.ps1",
    "smoke_p2_external.ps1"
)

$powerShellExe = (Get-Process -Id $PID).Path
foreach ($script in ($liveSmoke + $realSamples)) {
    $scriptPath = Join-Path $PSScriptRoot $script
    Invoke-SmokeStep "live smoke $script" {
        & $powerShellExe -ExecutionPolicy Bypass -File $scriptPath -Mode $Mode
        if ($LASTEXITCODE -ne 0) {
            throw "$script exited $LASTEXITCODE"
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "Smoke suite completed with failures:"
    foreach ($failure in $failures) {
        Write-Host " - $failure"
    }
    exit 1
}

Write-Host ""
Write-Host "Smoke suite passed."
