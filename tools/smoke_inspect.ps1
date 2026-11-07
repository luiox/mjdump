# Migrated from the morpher mjt-dump line (tools/ live smoke suite),
# adapted to the mjdump repo layout: artifact dir comes from -ArtifactDir
# or MJDUMP_SMOKE_ARTIFACT_DIR (default build\windows\x64\<Mode>),
# products are mjdump.exe / mjserver.dll, scratch output goes under
# build\smoke\<script>\<stamp>.
param(
    [string]$Java17Home = "$env:USERPROFILE\.jdks\corretto-17.0.5",
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
if (-not (Test-Path -LiteralPath $mjdump)) {
    throw "mjdump.exe not found: $mjdump. Run: xmake f -m $Mode --with_tests=y -y; xmake build mjdump"
}

$java = Join-Path $Java17Home "bin\java.exe"
$javac = Join-Path $Java17Home "bin\javac.exe"
if (-not (Test-Path -LiteralPath $java)) {
    throw "java.exe not found: $java"
}
if (-not (Test-Path -LiteralPath $javac)) {
    throw "javac.exe not found: $javac"
}

$stamp = "$(Get-Date -Format 'yyyyMMdd_HHmmss_ffff')_$PID"
$root = Join-Path $repoRoot "build\smoke\smoke_inspect\$stamp"
$srcDir = Join-Path $root "src"
$outDir = Join-Path $root "inspect"
$externalDir = Join-Path $root "inspect_external"
New-Item -ItemType Directory -Force -Path $srcDir, $outDir, $externalDir | Out-Null

$className = "SmokeInspectTarget"
$sourcePath = Join-Path $srcDir "$className.java"
$source = @"
public class $className {
    public static void main(String[] args) throws Exception {
        long deadline = System.currentTimeMillis() + 30000L;
        while (System.currentTimeMillis() < deadline) {
            Thread.sleep(200L);
        }
    }
}
"@
Set-Content -LiteralPath $sourcePath -Value $source -Encoding ASCII
& $javac -d $srcDir $sourcePath
if ($LASTEXITCODE -ne 0) {
    throw "javac failed for $className"
}

$proc = Start-Process -FilePath $java -ArgumentList @("-cp", $srcDir, $className) `
    -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Milliseconds 1500
    if ($proc.HasExited) {
        throw "target JVM exited early"
    }
    $defaultLog = Join-Path $env:TEMP "mjdump-$($proc.Id).log"
    $defaultOpts = Join-Path $env:TEMP "mjdump-$($proc.Id).opts"
    Remove-Item -LiteralPath $defaultLog, $defaultOpts -Force -ErrorAction SilentlyContinue

    $externalSummary = & $mjdump inspect -p $proc.Id -o $externalDir --external --class $className --filter-preview --limit 16 --wait-ms 10000
    if ($LASTEXITCODE -ne 0) {
        throw "mjdump inspect --external failed"
    }
    $externalJoined = $externalSummary -join "`n"
    if (-not $externalJoined.Contains("matched: 1")) {
        throw "external inspect summary did not report one match: $externalJoined"
    }
    $externalPath = Join-Path $externalDir "_inspect.json"
    $external = (Get-Content -LiteralPath $externalPath -Raw) | ConvertFrom-Json
    if ($external.source -ne "external") {
        throw "external inspect source should be external: $($external.source)"
    }
    if ($external.view -ne "matched") {
        throw "external inspect view should be matched: $($external.view)"
    }
    $externalTarget = @($external.classes) | Where-Object { $_.name -eq $className } | Select-Object -First 1
    if (-not $externalTarget -or -not $externalTarget.matched) {
        throw "external inspect missing matched target row"
    }
    if ($externalTarget.loader -ne "unknown" -or $externalTarget.module -ne "unknown") {
        throw "external inspect should mark module/loader as unknown"
    }

    $summary = & $mjdump inspect -p $proc.Id -o $outDir --class $className --module unnamed --group-by module --filter-preview --limit 16 --wait-ms 10000
    if ($LASTEXITCODE -ne 0) {
        throw "mjdump inspect failed"
    }
    $joined = $summary -join "`n"
    if (-not $joined.Contains("matched: 1")) {
        throw "inspect summary did not report one match: $joined"
    }

    $inspectPath = Join-Path $outDir "_inspect.json"
    if (-not (Test-Path -LiteralPath $inspectPath)) {
        throw "_inspect.json not found"
    }
    $json = Get-Content -LiteralPath $inspectPath -Raw
    $inspect = $json | ConvertFrom-Json
    if ($inspect.view -ne "matched") {
        throw "_inspect.json view should be matched for --filter-preview: $($inspect.view)"
    }
    foreach ($row in @($inspect.classes)) {
        if (-not $row.matched) {
            throw "--filter-preview wrote a skipped class row: $($row.name)"
        }
    }
    $targetRow = @($inspect.classes) | Where-Object { $_.name -eq $className } | Select-Object -First 1
    if (-not $targetRow) {
        throw "_inspect.json missing target class row"
    }
    if ($targetRow.loaderIdentityHash -eq 0) {
        throw "_inspect.json missing loader identity hash for app class"
    }
    if ([string]::IsNullOrWhiteSpace($targetRow.loaderParent)) {
        throw "_inspect.json missing loader parent summary"
    }
    if ($targetRow.estimatedPath -ne "_by_module/unnamed/$className.class") {
        throw "_inspect.json estimatedPath did not honor --group-by module: $($targetRow.estimatedPath)"
    }
    if (-not $json.Contains("`"name`": `"$className`"")) {
        throw "_inspect.json missing target class"
    }
    if (-not $json.Contains("`"matched`": true")) {
        throw "_inspect.json missing matched preview"
    }
    if (-not $json.Contains("`"module`": `"unnamed`"")) {
        throw "_inspect.json missing unnamed module label"
    }
    if (-not $json.Contains("`"normalizedFilters`"")) {
        throw "_inspect.json missing normalized filters"
    }

    $aliasDir = Join-Path $root "classes_alias_json"
    New-Item -ItemType Directory -Force -Path $aliasDir | Out-Null
    $aliasOutput = & $mjdump classes -p $proc.Id -o $aliasDir --json --limit 1 --view all --wait-ms 10000
    if ($LASTEXITCODE -ne 0) {
        throw "mjdump classes --json failed"
    }
    $aliasText = $aliasOutput -join "`n"
    if ($aliasText.Contains("inspect:") -or $aliasText.Contains("matched:")) {
        throw "classes --json mixed human summary into JSON output"
    }
    $aliasJson = $aliasText | ConvertFrom-Json
    if ($aliasJson.mode -ne "inspect") {
        throw "classes alias JSON mode mismatch: $($aliasJson.mode)"
    }
    if ($aliasJson.view -ne "all") {
        throw "classes alias JSON view mismatch: $($aliasJson.view)"
    }
    if ($aliasJson.limit -ne 1 -or @($aliasJson.classes).Count -ne 1) {
        throw "classes alias JSON limit did not cap rows"
    }
    if (-not $aliasJson.truncated) {
        throw "classes alias JSON should be truncated with --limit 1"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $aliasDir "_inspect.json"))) {
        throw "classes alias did not write _inspect.json"
    }

    $quietDir = Join-Path $root "inspect_quiet"
    New-Item -ItemType Directory -Force -Path $quietDir | Out-Null
    $quietOutput = & $mjdump inspect -p $proc.Id -o $quietDir --class $className --quiet --wait-ms 10000
    if ($LASTEXITCODE -ne 0) {
        throw "mjdump inspect --quiet failed"
    }
    $quietText = $quietOutput -join "`n"
    if (-not [string]::IsNullOrWhiteSpace($quietText)) {
        throw "inspect --quiet printed output: $quietText"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $quietDir "_inspect.json"))) {
        throw "inspect --quiet did not write _inspect.json"
    }

    $externalQuietDir = Join-Path $root "inspect_external_quiet"
    New-Item -ItemType Directory -Force -Path $externalQuietDir | Out-Null
    $externalQuietOutput = & $mjdump inspect -p $proc.Id -o $externalQuietDir --external --class $className --quiet --wait-ms 10000
    if ($LASTEXITCODE -ne 0) {
        throw "mjdump inspect --external --quiet failed"
    }
    $externalQuietText = $externalQuietOutput -join "`n"
    if (-not [string]::IsNullOrWhiteSpace($externalQuietText)) {
        throw "inspect --external --quiet printed output: $externalQuietText"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $externalQuietDir "_inspect.json"))) {
        throw "inspect --external --quiet did not write _inspect.json"
    }

    if (Test-Path -LiteralPath $defaultLog) {
        throw "default inspect left diagnostic log: $defaultLog"
    }
    if (Test-Path -LiteralPath $defaultOpts) {
        throw "default inspect left option file: $defaultOpts"
    }

    if (-not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
        $proc.WaitForExit()
    }
    $proc = Start-Process -FilePath $java -ArgumentList @("-cp", $srcDir, $className) `
        -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 1500
    if ($proc.HasExited) {
        throw "second target JVM exited early"
    }

    $skippedDir = Join-Path $root "inspect_skipped"
    New-Item -ItemType Directory -Force -Path $skippedDir | Out-Null
    & $mjdump inspect -p $proc.Id -o $skippedDir --class $className --view skipped --limit 16 --wait-ms 10000 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "mjdump inspect --view skipped failed"
    }
    $skippedPath = Join-Path $skippedDir "_inspect.json"
    $skipped = (Get-Content -LiteralPath $skippedPath -Raw) | ConvertFrom-Json
    if ($skipped.view -ne "skipped") {
        throw "_inspect.json view should be skipped: $($skipped.view)"
    }
    foreach ($row in @($skipped.classes)) {
        if ($row.matched) {
            throw "--view skipped wrote a matched class row: $($row.name)"
        }
        if ($row.name -eq $className) {
            throw "--view skipped wrote target class"
        }
        if ([string]::IsNullOrWhiteSpace($row.skipReason)) {
            throw "--view skipped row missing skipReason: $($row.name)"
        }
    }

    if (-not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
        $proc.WaitForExit()
    }
    $proc = Start-Process -FilePath $java -ArgumentList @("-cp", $srcDir, $className) `
        -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 1500
    if ($proc.HasExited) {
        throw "diagnostic target JVM exited early"
    }
    $diagnosticDir = Join-Path $root "inspect_diagnostic"
    $diagnosticLog = Join-Path $env:TEMP "mjdump-$($proc.Id).log"
    $diagnosticOpts = Join-Path $env:TEMP "mjdump-$($proc.Id).opts"
    Remove-Item -LiteralPath $diagnosticLog, $diagnosticOpts -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $diagnosticDir | Out-Null
    & $mjdump inspect -p $proc.Id -o $diagnosticDir --class $className --diagnostic --filter-preview --limit 4 --wait-ms 10000 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "mjdump inspect --diagnostic failed"
    }
    if (-not (Test-Path -LiteralPath $diagnosticLog)) {
        throw "--diagnostic did not write injection log: $diagnosticLog"
    }
    if (Test-Path -LiteralPath $diagnosticOpts) {
        throw "--diagnostic left option file: $diagnosticOpts"
    }
    Remove-Item -LiteralPath $diagnosticLog -Force -ErrorAction SilentlyContinue

    & $mjdump inspect -p $proc.Id -o (Join-Path $root "bad_regex") --regex "[" --wait-ms 1000 2>$null
    if ($LASTEXITCODE -eq 0) {
        throw "inspect accepted invalid regex"
    }
    & $mjdump inspect -p $proc.Id -o (Join-Path $root "bad_view") --view nope --wait-ms 1000 2>$null
    if ($LASTEXITCODE -eq 0) {
        throw "inspect accepted invalid view"
    }
}
finally {
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
        $proc.WaitForExit()
    }
}

Write-Host "inspect smoke passed: $root"
