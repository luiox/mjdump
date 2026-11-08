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
    [string]$MjtTool = "",
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
if ([string]::IsNullOrWhiteSpace($MjtTool)) {
    $MjtTool = $env:MJDUMP_SMOKE_MJT_TOOL
}
if ([string]::IsNullOrWhiteSpace($MjtTool)) {
    Write-Host "mjt.exe not provided (-MjtTool / MJDUMP_SMOKE_MJT_TOOL): skipping mjt downstream checks"
    $mjt = $null
} elseif (-not (Test-Path -LiteralPath $MjtTool)) {
    throw "mjt.exe not found: $MjtTool"
} else {
    $mjt = $MjtTool
}
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
    Start-Sleep -Milliseconds 1500
    if ($proc.HasExited) {
        throw "target JVM exited early for $ClassName"
    }
    return $proc
}

function Assert-StatusContains {
    param([string[]]$Status, [string]$Needle)
    $joined = $Status -join "`n"
    if (-not $joined.Contains($Needle)) {
        throw "status missing '$Needle'"
    }
}

function Assert-OutputContains {
    param([string[]]$Output, [string]$Needle, [string]$Label)
    $joined = $Output -join "`n"
    if (-not $joined.Contains($Needle)) {
        throw "$Label missing '$Needle'"
    }
}

function Invoke-PracticalDumpSmoke {
    param(
        [string]$Root,
        [string]$Name,
        [string]$JavaHome,
        [string]$ClassName,
        [string]$ModuleFilter = ""
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
        & $mjdump inspect -p $proc.Id -o $outDir --external --class $ClassName --view skipped --limit 8 --wait-ms 10000 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "$Name external skipped preview failed"
        }

        $dumpArgs = @("dump", "-p", "$($proc.Id)", "-o", $outDir, "--class", $ClassName, "--group-by", "module", "--wait-ms", "10000")
        if (-not [string]::IsNullOrWhiteSpace($ModuleFilter)) {
            $dumpArgs += @("--module", $ModuleFilter)
        }
        & $mjdump @dumpArgs
        if ($LASTEXITCODE -ne 0) {
            throw "$Name practical dump failed"
        }
        $optsPath = Join-Path $env:TEMP "mjdump-$($proc.Id).opts"
        if (Test-Path -LiteralPath $optsPath) {
            throw "$Name inject opts file remained after first dump: $optsPath"
        }

        $dumped = Get-ChildItem -Path $outDir -Recurse -Filter "$ClassName.class" |
            Select-Object -First 1
        if (-not $dumped) {
            throw "$Name dumped class not found"
        }

        $indexPath = Join-Path $outDir "_index.json"
        if (-not (Test-Path -LiteralPath $indexPath)) {
            throw "$Name _index.json not found"
        }
        $indexJson = Get-Content -LiteralPath $indexPath -Raw
        if (-not $indexJson.Contains("`"name`": `"$ClassName`"")) {
            throw "$Name _index.json missing dumped class"
        }
        if (-not $indexJson.Contains("`"classMagicValid`": true")) {
            throw "$Name _index.json missing class magic validation"
        }
        if (-not $indexJson.Contains("`"pathSafe`": true")) {
            throw "$Name _index.json missing path safety validation"
        }
        $index = $indexJson | ConvertFrom-Json
        if (@($index.dumpedClasses).Count -ne 1) {
            throw "$Name _index.json dumpedClasses count mismatch"
        }
        if (-not $index.postValidation.ok) {
            throw "$Name _index.json post validation failed"
        }
        if ($index.filterHits -ne 1) {
            throw "$Name _index.json filterHits mismatch: $($index.filterHits)"
        }
        if ([string]::IsNullOrWhiteSpace($index.dumpedClasses[0].sourcePath)) {
            throw "$Name _index.json missing dumped class sourcePath"
        }
        if ([string]::IsNullOrWhiteSpace($index.dumpedClasses[0].hash)) {
            throw "$Name _index.json missing dumped class hash"
        }
        if (@($index.skippedClasses).Count -eq 0) {
            throw "$Name _index.json missing skippedClasses from preview"
        }

        $status = & $mjdump status -o $outDir
        if ($LASTEXITCODE -ne 0) {
            throw "$Name status failed"
        }
        Assert-StatusContains $status "status: completed"
        Assert-StatusContains $status "classWrites: 1"
        Assert-StatusContains $status "sweepMatched: 1"

        $packedJar = Join-Path $outDir "dumped.jar"
        & $mjdump pack -i $outDir -o $packedJar
        if ($LASTEXITCODE -ne 0) {
            throw "$Name pack readback verification failed"
        }
        if (-not (Test-Path -LiteralPath $packedJar)) {
            throw "$Name packed jar not found"
        }

        if ($mjt) {
            $stringsOutput = & $mjt strings -i $outDir
            if ($LASTEXITCODE -ne 0) {
                throw "$Name mjt strings failed on dump directory"
            }
            Assert-OutputContains $stringsOutput $ClassName "$Name mjt strings"

            $disasmOutput = & $mjt disasm --legacy -i $outDir -c $ClassName
            if ($LASTEXITCODE -ne 0) {
                throw "$Name mjt disasm failed on dump directory"
            }
            Assert-OutputContains $disasmOutput "Class: $ClassName" "$Name mjt disasm"

            $infoOutput = & $mjt info -i $packedJar
            if ($LASTEXITCODE -ne 0) {
                throw "$Name mjt info failed on packed jar"
            }
            Assert-OutputContains $infoOutput "Class files:" "$Name mjt info"
        } else {
            Write-Host "skipping mjt downstream checks for $Name"
        }
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

$stamp = "$(Get-Date -Format 'yyyyMMdd_HHmmss_ffff')_$PID"
$root = Join-Path $repoRoot "build\smoke\smoke_mvp_practical\$stamp"
New-Item -ItemType Directory -Force -Path $root | Out-Null

Invoke-PracticalDumpSmoke $root "corretto8" $Java8Home "SmokeMvpJdk8"
Invoke-PracticalDumpSmoke $root "corretto17" $Java17Home "SmokeMvpJdk17" "unnamed"
if (-not $SkipJdk21) {
    Invoke-PracticalDumpSmoke $root "corretto21" $Java21Home "SmokeMvpJdk21" "unnamed"
}

Write-Host "MVP practical dump smoke passed: $root"
