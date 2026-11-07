# Migrated from the morpher mjt-dump line (tools/ live smoke suite),
# adapted to the mjdump repo layout: artifact dir comes from -ArtifactDir
# or MJDUMP_SMOKE_ARTIFACT_DIR (default build\windows\x64\<Mode>),
# products are mjdump.exe / mjserver.dll, scratch output goes under
# build\smoke\<script>\<stamp>.
param(
    [string]$JavaHome = "$env:USERPROFILE\.jdks\corretto-1.8.0_352",
    [ValidateSet("debug", "release")]
    [string]$Mode = "release",
    [string]$ArtifactDir = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($ArtifactDir)) {
    $ArtifactDir = $env:MJDUMP_SMOKE_ARTIFACT_DIR
}
if ([string]::IsNullOrWhiteSpace($ArtifactDir)) {
    $ArtifactDir = Join-Path $repoRoot "build\windows\x64\$Mode"
}
$artifactDir = $ArtifactDir
$mjserver = Join-Path $artifactDir "mjserver.dll"
if (-not (Test-Path -LiteralPath $mjserver)) {
    throw "mjserver.dll not found: $mjserver. Run: xmake f -m $Mode --with_tests=y -y; xmake build mjserver"
}

$java = Join-Path $JavaHome "bin\java.exe"
$javac = Join-Path $JavaHome "bin\javac.exe"
if (-not (Test-Path -LiteralPath $java)) {
    throw "java.exe not found: $java"
}
if (-not (Test-Path -LiteralPath $javac)) {
    throw "javac.exe not found: $javac"
}

$stamp = "$(Get-Date -Format 'yyyyMMdd_HHmmss_ffff')_$PID"
$root = Join-Path $repoRoot "build\smoke\smoke_p0_agent_args\$stamp"
$srcDir = Join-Path $root "src"
$classesDir = Join-Path $root "classes"
New-Item -ItemType Directory -Force -Path $srcDir, $classesDir | Out-Null

$sourcePath = Join-Path $srcDir "ArgsProbe.java"
$source = @"
import java.lang.management.ManagementFactory;

public class ArgsProbe {
    public static void main(String[] args) {
        System.out.println("ARGS=" + ManagementFactory.getRuntimeMXBean().getInputArguments());
    }
}
"@
Set-Content -LiteralPath $sourcePath -Value $source -Encoding ASCII
& $javac -d $classesDir $sourcePath
if ($LASTEXITCODE -ne 0) {
    throw "javac failed for ArgsProbe"
}

function Invoke-Probe {
    param(
        [string]$Name,
        [bool]$HideAgentArgs
    )

    $dumpDir = Join-Path $root $Name
    New-Item -ItemType Directory -Force -Path $dumpDir | Out-Null
    $hideText = if ($HideAgentArgs) { "true" } else { "false" }
    $agentArg = "-agentpath:$mjserver=out=$dumpDir,include=ArgsProbe,hideAgentArgs=$hideText,silent=true"
    $output = & $java $agentArg "-Xmx64m" "-Dmjserver.marker=keep" "-cp" $classesDir "ArgsProbe" 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "ArgsProbe failed for $Name`n$output"
    }
    return ($output -join "`n")
}

$hidden = Invoke-Probe "hidden" $true
if ($hidden.Contains("-agentpath") -or $hidden.Contains("mjserver.dll")) {
    throw "hideAgentArgs=true leaked agent path: $hidden"
}
if (-not $hidden.Contains("-Xmx64m")) {
    throw "hideAgentArgs=true removed unrelated -Xmx argument: $hidden"
}
if (-not $hidden.Contains("-Dmjserver.marker=keep")) {
    throw "hideAgentArgs=true removed unrelated -D argument containing marker text: $hidden"
}

$visible = Invoke-Probe "visible" $false
if (-not ($visible.Contains("-agentpath") -or $visible.Contains("mjserver"))) {
    throw "hideAgentArgs=false did not preserve visible agent argument: $visible"
}

Write-Host "P0 agent argument hiding smoke passed: $root"
