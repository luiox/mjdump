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
$mjserver = Join-Path $artifactDir "mjserver.dll"
$powerShellExe = (Get-Process -Id $PID).Path
$powerShell32Exe = Join-Path $env:WINDIR "SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
if (-not (Test-Path -LiteralPath $mjdump)) {
    throw "mjdump.exe not found: $mjdump. Run: xmake f -m $Mode --with_tests=y -y; xmake build mjdump"
}
if (-not (Test-Path -LiteralPath $mjserver)) {
    throw "mjserver.dll not found: $mjserver. Run: xmake f -m $Mode --with_tests=y -y; xmake build mjserver"
}

function Require-Tool {
    param([string]$Path, [string]$Name)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Name not found: $Path"
    }
}

function Write-ScenarioSources {
    param([string]$SourceRoot)
    $pkg = Join-Path $SourceRoot "mvp\scenario"
    New-Item -ItemType Directory -Force -Path $pkg | Out-Null
    Set-Content -LiteralPath (Join-Path $pkg "KeepTarget.java") -Encoding ASCII -Value @"
package mvp.scenario;
public class KeepTarget {
    public static String value() { return "keep"; }
}
"@
    Set-Content -LiteralPath (Join-Path $pkg "DropTarget.java") -Encoding ASCII -Value @"
package mvp.scenario;
public class DropTarget {
    public static String value() { return "drop"; }
}
"@
    Set-Content -LiteralPath (Join-Path $pkg "Main.java") -Encoding ASCII -Value @"
package mvp.scenario;
public class Main {
    public static void main(String[] args) throws Exception {
        System.out.println(KeepTarget.value() + DropTarget.value());
        long deadline = System.currentTimeMillis() + 30000L;
        while (System.currentTimeMillis() < deadline) {
            Thread.sleep(200L);
        }
    }
}
"@
}

function Compile-Scenario {
    param(
        [string]$WorkRoot,
        [string]$Javac
    )
    $src = Join-Path $WorkRoot "src"
    $classes = Join-Path $WorkRoot "classes"
    New-Item -ItemType Directory -Force -Path $src, $classes | Out-Null
    Write-ScenarioSources $src
    $sources = Get-ChildItem -Path $src -Recurse -Filter "*.java" | ForEach-Object { $_.FullName }
    & $Javac -d $classes @sources
    if ($LASTEXITCODE -ne 0) {
        throw "javac failed for scenario sources"
    }
    return $classes
}

function Invoke-Status {
    param([string]$OutputDir)
    $status = & $mjdump status -o $OutputDir
    if ($LASTEXITCODE -ne 0) {
        throw "mjdump status failed for $OutputDir"
    }
    return $status
}

function Assert-Contains {
    param([string[]]$Lines, [string]$Needle)
    $joined = $Lines -join "`n"
    if (-not $joined.Contains($Needle)) {
        throw "missing '$Needle'"
    }
}

function Assert-ClassExists {
    param([string]$OutputDir, [string]$ClassName)
    $found = Get-ChildItem -Path $OutputDir -Recurse -Filter "$ClassName.class" |
        Select-Object -First 1
    if (-not $found) {
        throw "$ClassName.class not found under $OutputDir"
    }
}

function Assert-ClassMissing {
    param([string]$OutputDir, [string]$ClassName)
    $found = Get-ChildItem -Path $OutputDir -Recurse -Filter "$ClassName.class" |
        Select-Object -First 1
    if ($found) {
        throw "$ClassName.class unexpectedly dumped at $($found.FullName)"
    }
}

function Assert-SessionListed {
    param([string]$OutputDir)
    $sessionsText = (& $mjdump sessions --json) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw "sessions --json failed"
    }
    $sessions = $sessionsText | ConvertFrom-Json
    $match = @($sessions.sessions) | Where-Object { $_.outputDir -eq $OutputDir } | Select-Object -First 1
    if (-not $match) {
        throw "session registry missing $OutputDir"
    }
}

function Invoke-P0AgentpathFilterSmoke {
    param([string]$Root)
    $java = Join-Path $Java8Home "bin\java.exe"
    $javac = Join-Path $Java8Home "bin\javac.exe"
    Require-Tool $java "Java 8"
    Require-Tool $javac "javac 8"

    $work = Join-Path $Root "p0_agentpath_filter"
    $outDir = Join-Path $work "dump"
    $classes = Compile-Scenario $work $javac
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    $agentArg = "-agentpath:$mjserver=out=$outDir,include=mvp/scenario/**,exclude=mvp/scenario/DropTarget,silent=true"
    $proc = Start-Process -FilePath $java -ArgumentList @($agentArg, "-cp", $classes, "mvp.scenario.Main") `
        -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 2500
        if ($proc.HasExited) {
            throw "P0 target JVM exited early"
        }
        Assert-ClassExists $outDir "KeepTarget"
        Assert-ClassMissing $outDir "DropTarget"
        $status = Invoke-Status $outDir
        Assert-Contains $status "status: completed"
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

function Start-ScenarioJvm {
    param(
        [string]$Java,
        [string]$Classes
    )
    $proc = Start-Process -FilePath $Java -ArgumentList @("-cp", $Classes, "mvp.scenario.Main") `
        -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 2500
    if ($proc.HasExited) {
        throw "scenario JVM exited early"
    }
    return $proc
}

function Invoke-P1PracticalNoMatchSmoke {
    param(
        [string]$Root,
        [string]$Name,
        [string]$JavaHome
    )
    $java = Join-Path $JavaHome "bin\java.exe"
    $javac = Join-Path $JavaHome "bin\javac.exe"
    Require-Tool $java $Name
    Require-Tool $javac "$Name javac"

    $work = Join-Path $Root "p1_no_match_$Name"
    $outDir = Join-Path $work "dump"
    $classes = Compile-Scenario $work $javac
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $proc = Start-ScenarioJvm $java $classes
    try {
        & $mjdump dump -p $proc.Id -o $outDir --class mvp.scenario.NoSuchClass --wait-ms 10000
        if ($LASTEXITCODE -ne 0) {
            throw "P1 no-match dump should complete successfully for $Name"
        }
        $status = Invoke-Status $outDir
        Assert-Contains $status "status: completed"
        Assert-Contains $status "classWrites: 0"
        Assert-Contains $status "sweepMatched: 0"
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

function Invoke-P1PracticalRegexSmoke {
    param([string]$Root)
    $java = Join-Path $Java17Home "bin\java.exe"
    $javac = Join-Path $Java17Home "bin\javac.exe"
    Require-Tool $java "Java 17"
    Require-Tool $javac "javac 17"

    $work = Join-Path $Root "p1_regex"
    $outDir = Join-Path $work "dump"
    $classes = Compile-Scenario $work $javac
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $proc = Start-ScenarioJvm $java $classes
    try {
        $dumpOutput = & $mjdump dump -p $proc.Id -o $outDir --regex "mvp[/.]scenario[/.]KeepTarget{1,1}" --wait-ms 10000
        if ($LASTEXITCODE -ne 0) {
            throw "P1 regex dump failed"
        }
        $dumpText = $dumpOutput -join "`n"
        if ($dumpText.Contains("sent options:")) {
            throw "default dump printed raw inject options"
        }
        if ($dumpText.Contains("KeepTarget%7B1%2C1%7D") -or $dumpText.Contains("KeepTarget{1,1}")) {
            throw "default dump printed raw regex filter"
        }
        if (-not $dumpText.Contains("[inject] options=sent")) {
            throw "default dump did not print redacted inject option summary"
        }
        Assert-ClassExists $outDir "KeepTarget"
        Assert-ClassMissing $outDir "DropTarget"
        Assert-SessionListed $outDir
        $optsPath = Join-Path $env:TEMP "mjdump-$($proc.Id).opts"
        if (Test-Path -LiteralPath $optsPath) {
            throw "inject opts file remained before second sweep: $optsPath"
        }
        $diagnosticLog = Join-Path $env:TEMP "mjdump-$($proc.Id).log"
        Remove-Item -LiteralPath $diagnosticLog -Force -ErrorAction SilentlyContinue
        $sweepOutput = & $mjdump sweep -o $outDir --class mvp.scenario.DropTarget --wait-ms 10000 --diagnostic
        if ($LASTEXITCODE -ne 0) {
            throw "P1 second sweep failed"
        }
        $sweepText = $sweepOutput -join "`n"
        if (-not $sweepText.Contains("sent options:")) {
            throw "diagnostic sweep did not print raw inject options"
        }
        if (-not $sweepText.Contains("DropTarget")) {
            throw "diagnostic sweep did not include selected class in raw options"
        }
        if (-not (Test-Path -LiteralPath $diagnosticLog)) {
            throw "diagnostic sweep did not write injection log: $diagnosticLog"
        }
        Remove-Item -LiteralPath $diagnosticLog -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $optsPath) {
            throw "second sweep unexpectedly wrote opts file: $optsPath"
        }
        Assert-ClassExists $outDir "DropTarget"
        Assert-SessionListed $outDir
        $status = Invoke-Status $outDir
        Assert-Contains $status "status: completed"
        Assert-Contains $status "classWrites: 1"
        Assert-Contains $status "sweepMatched: 1"
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

function Invoke-P1LateCompleteSmoke {
    param([string]$Root)
    $java = Join-Path $Java17Home "bin\java.exe"
    $javac = Join-Path $Java17Home "bin\javac.exe"
    Require-Tool $java "Java 17"
    Require-Tool $javac "javac 17"

    $work = Join-Path $Root "p1_late_complete"
    $outDir = Join-Path $work "dump"
    $classes = Compile-Scenario $work $javac
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $proc = Start-ScenarioJvm $java $classes
    try {
        & $mjdump dump -p $proc.Id -o $outDir --class mvp.scenario.KeepTarget --wait-ms 1000 --sweep-delay-ms 2500 2>$null
        if ($LASTEXITCODE -eq 0) {
            throw "P1 late-complete smoke unexpectedly succeeded"
        }
        Start-Sleep -Milliseconds 3200
        Assert-ClassExists $outDir "KeepTarget"
        $status = Invoke-Status $outDir
        Assert-Contains $status "status: completed"
        Assert-Contains $status "classWrites: 1"
        Assert-Contains $status "cliErrorCode: agent_timeout"
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

function Invoke-P1BadPidSmoke {
    param([string]$Root)
    $outDir = Join-Path $Root "p1_bad_pid\dump"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    & $mjdump dump -p 4294967294 -o $outDir --class mvp.scenario.KeepTarget --wait-ms 1000 2>$null
    if ($LASTEXITCODE -eq 0) {
        throw "P1 bad-pid dump unexpectedly succeeded"
    }
    $status = Invoke-Status $outDir
    Assert-Contains $status "status: failed"
    Assert-Contains $status "errorCode: bad_pid"
}

function Invoke-P1NonHotSpotSmoke {
    param([string]$Root)
    $outDir = Join-Path $Root "p1_non_hotspot\dump"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $proc = Start-Process -FilePath $powerShellExe -ArgumentList @(
        "-NoProfile",
        "-Command",
        "Start-Sleep -Seconds 30"
    ) -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 500
        if ($proc.HasExited) {
            throw "non-HotSpot target process exited early"
        }
        & $mjdump dump -p $proc.Id -o $outDir --class mvp.scenario.KeepTarget --wait-ms 1000 2>$null
        if ($LASTEXITCODE -eq 0) {
            throw "P1 non-HotSpot dump unexpectedly succeeded"
        }
        $status = Invoke-Status $outDir
        Assert-Contains $status "status: failed"
        Assert-Contains $status "errorCode: not_hotspot"
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

function Invoke-P1AccessDeniedSmoke {
    param([string]$Root)
    $outDir = Join-Path $Root "p1_access_denied\dump"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    & $mjdump dump -p 4 -o $outDir --class mvp.scenario.KeepTarget --wait-ms 1000 2>$null
    if ($LASTEXITCODE -eq 0) {
        throw "P1 access-denied dump unexpectedly succeeded"
    }
    $status = Invoke-Status $outDir
    Assert-Contains $status "status: failed"
    Assert-Contains $status "errorCode: access_denied"
}

function Invoke-P1BitnessMismatchSmoke {
    param([string]$Root)
    if (-not (Test-Path -LiteralPath $powerShell32Exe)) {
        Write-Host "Skipping P1 bitness mismatch smoke: 32-bit PowerShell not found"
        return
    }

    $outDir = Join-Path $Root "p1_bitness_mismatch\dump"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $proc = Start-Process -FilePath $powerShell32Exe -ArgumentList @(
        "-NoProfile",
        "-Command",
        "Start-Sleep -Seconds 30"
    ) -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 500
        if ($proc.HasExited) {
            throw "32-bit target process exited early"
        }
        & $mjdump dump -p $proc.Id -o $outDir --class mvp.scenario.KeepTarget --wait-ms 1000 2>$null
        if ($LASTEXITCODE -eq 0) {
            throw "P1 bitness-mismatch dump unexpectedly succeeded"
        }
        $status = Invoke-Status $outDir
        Assert-Contains $status "status: failed"
        Assert-Contains $status "errorCode: bitness_mismatch"
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

$stamp = "$(Get-Date -Format 'yyyyMMdd_HHmmss_ffff')_$PID"
$root = Join-Path $repoRoot "build\smoke\smoke_mvp_scenarios\$stamp"
New-Item -ItemType Directory -Force -Path $root | Out-Null

Invoke-P0AgentpathFilterSmoke $root
Invoke-P1PracticalNoMatchSmoke $root "corretto8" $Java8Home
Invoke-P1PracticalNoMatchSmoke $root "corretto17" $Java17Home
Invoke-P1PracticalNoMatchSmoke $root "corretto21" $Java21Home
Invoke-P1PracticalRegexSmoke $root
Invoke-P1LateCompleteSmoke $root
Invoke-P1BadPidSmoke $root
Invoke-P1NonHotSpotSmoke $root
Invoke-P1AccessDeniedSmoke $root
Invoke-P1BitnessMismatchSmoke $root

Write-Host "MVP scenario smoke passed: $root"
