# Migrated from the morpher mjt-dump line (tools/ live smoke suite),
# adapted to the mjdump repo layout: artifact dir comes from -ArtifactDir
# or MJDUMP_SMOKE_ARTIFACT_DIR (default build\windows\x64\<Mode>),
# products are mjdump.exe / mjserver.dll, scratch output goes under
# build\smoke\<script>\<stamp>.
param(
    [string]$Java8Home = "$env:USERPROFILE\.jdks\corretto-1.8.0_352",
    [string]$Java17Home = "$env:USERPROFILE\.jdks\corretto-17.0.5",
    [string]$Java21Home = "$env:USERPROFILE\.jdks\corretto-21.0.4",
    [ValidateSet("debug", "release")]
    [string]$Mode = "release",
    [switch]$SkipJdk21,
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
$mjdump = Join-Path $artifactDir "mjdump.exe"
if (-not (Test-Path -LiteralPath $mjdump)) {
    throw "mjdump.exe not found: $mjdump. Run: xmake f -m $Mode --with_tests=y -y; xmake build mjdump"
}

function Require-Tool {
    param([string]$Path, [string]$Name)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Name not found: $Path"
    }
}

function New-SmokeClass {
    param(
        [string]$WorkRoot,
        [string]$ClassName,
        [string]$Javac
    )

    $srcDir = Join-Path $WorkRoot "src"
    New-Item -ItemType Directory -Force -Path $srcDir | Out-Null
    $sourcePath = Join-Path $srcDir "$ClassName.java"
    $source = @"
public class $ClassName {
    public static void main(String[] args) throws Exception {
        long deadline = System.currentTimeMillis() + 30000L;
        while (System.currentTimeMillis() < deadline) {
            Thread.sleep(200L);
        }
    }
}
"@
    Set-Content -LiteralPath $sourcePath -Value $source -Encoding ASCII
    & $Javac -d $srcDir $sourcePath
    if ($LASTEXITCODE -ne 0) {
        throw "javac failed for $ClassName"
    }
    return $srcDir
}

function Start-SmokeJvm {
    param([string]$Java, [string]$ClassPath, [string]$ClassName)
    $proc = Start-Process -FilePath $Java -ArgumentList @("-cp", $ClassPath, $ClassName) `
        -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 1200
    if ($proc.HasExited) {
        throw "target JVM exited early for $ClassName"
    }
    return $proc
}

function Invoke-Status {
    param([string]$OutputDir)
    $status = & $mjdump status -o $OutputDir
    if ($LASTEXITCODE -ne 0) {
        throw "mjdump status failed for $OutputDir"
    }
    return $status
}

function Assert-StatusContains {
    param([string[]]$Status, [string]$Needle)
    $joined = $Status -join "`n"
    if (-not $joined.Contains($Needle)) {
        throw "status missing '$Needle'"
    }
}

function Invoke-PositiveJava8 {
    param([string]$Root)
    $java = Join-Path $Java8Home "bin\java.exe"
    $javac = Join-Path $Java8Home "bin\javac.exe"
    Require-Tool $java "Java 8"
    Require-Tool $javac "javac 8"

    $work = Join-Path $Root "java8_cache_bait"
    $outDir = Join-Path $work "dump"
    $srcDir = New-SmokeClass $work "SmokeP2Java8" $javac
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $proc = Start-SmokeJvm $java $srcDir "SmokeP2Java8"
    try {
        & $mjdump dump -p $proc.Id --external --cache-bait -o $outDir --class SmokeP2Java8 --wait-ms 10000
        if ($LASTEXITCODE -ne 0) {
            throw "Java 8 cache-bait external dump failed"
        }
        $status = Invoke-Status $outDir
        Assert-StatusContains $status "cacheBaitState: completed"
        Assert-StatusContains $status "p2WalkSource: systemDictionary"
        Assert-StatusContains $status "p2ByteWrites: 1"

        $original = Join-Path $srcDir "SmokeP2Java8.class"
        $dumped = Get-ChildItem -Path $outDir -Recurse -Filter "SmokeP2Java8.class" |
            Select-Object -First 1
        if (-not $dumped) {
            throw "dumped SmokeP2Java8.class not found"
        }
        $originalHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $original).Hash
        $dumpedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dumped.FullName).Hash
        if ($originalHash -ne $dumpedHash) {
            throw "Java 8 dumped class hash mismatch"
        }
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

function Invoke-NegativeJava8NoBait {
    param([string]$Root)
    $java = Join-Path $Java8Home "bin\java.exe"
    $javac = Join-Path $Java8Home "bin\javac.exe"

    $work = Join-Path $Root "java8_no_bait"
    $outDir = Join-Path $work "dump"
    $srcDir = New-SmokeClass $work "SmokeP2Java8NoBait" $javac
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $proc = Start-SmokeJvm $java $srcDir "SmokeP2Java8NoBait"
    try {
        & $mjdump dump -p $proc.Id --external -o $outDir --class SmokeP2Java8NoBait --wait-ms 10000
        if ($LASTEXITCODE -eq 0) {
            throw "Java 8 no-bait negative unexpectedly succeeded"
        }
        $status = Invoke-Status $outDir
        Assert-StatusContains $status "status: failed"
        Assert-StatusContains $status "errorCode: no_cached_class_bytes"
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

function Invoke-SourceGateNegative {
    param(
        [string]$Root,
        [string]$Name,
        [string]$JavaHome,
        [string]$ClassName
    )

    $java = Join-Path $JavaHome "bin\java.exe"
    $javac = Join-Path $JavaHome "bin\javac.exe"
    Require-Tool $java $Name
    Require-Tool $javac "$Name javac"

    $work = Join-Path $Root $Name
    $outDir = Join-Path $work "dump"
    $srcDir = New-SmokeClass $work $ClassName $javac
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $proc = Start-SmokeJvm $java $srcDir $ClassName
    try {
        & $mjdump dump -p $proc.Id --external -o $outDir --class $ClassName --wait-ms 10000
        if ($LASTEXITCODE -eq 0) {
            throw "$Name negative unexpectedly succeeded"
        }
        $status = Invoke-Status $outDir
        Assert-StatusContains $status "status: failed"
        Assert-StatusContains $status "p2WalkSource: cldg"
        Assert-StatusContains $status "errorCode: source_unavailable"
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

$stamp = "$(Get-Date -Format 'yyyyMMdd_HHmmss_ffff')_$PID"
$root = Join-Path $repoRoot "build\smoke\smoke_p2_external\$stamp"
New-Item -ItemType Directory -Force -Path $root | Out-Null

Invoke-PositiveJava8 $root
Invoke-NegativeJava8NoBait $root
Invoke-SourceGateNegative $root "corretto17" $Java17Home "SmokeP2Jdk17"
if (-not $SkipJdk21) {
    Invoke-SourceGateNegative $root "corretto21" $Java21Home "SmokeP2Jdk21"
}

Write-Host "P2 external smoke passed: $root"
