# Migrated from the morpher mjt-dump line (tools/ live smoke suite),
# adapted to the mjdump repo layout: artifact dir comes from -ArtifactDir
# or MJDUMP_SMOKE_ARTIFACT_DIR (default build\windows\x64\<Mode>),
# products are mjdump.exe / mjserver.dll, scratch output goes under
# build\smoke\<script>\<stamp>.
param(
    [string]$Java17Home = "$env:USERPROFILE\.jdks\corretto-17.0.5",
    [ValidateSet("debug", "release")]
    [string]$Mode = "release",
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

$java = Join-Path $Java17Home "bin\java.exe"
$javac = Join-Path $Java17Home "bin\javac.exe"
$jar = Join-Path $Java17Home "bin\jar.exe"
$javap = Join-Path $Java17Home "bin\javap.exe"
foreach ($tool in @($java, $javac, $jar, $javap)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Java tool not found: $tool"
    }
}

function New-Dir {
    param([string]$Path)
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    return $Path
}

function Write-AsciiFile {
    param([string]$Path, [string]$Value)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    Set-Content -LiteralPath $Path -Value $Value -Encoding ASCII
}

function Compile-Sources {
    param([string]$SourceRoot, [string]$ClassesRoot)
    New-Dir $ClassesRoot | Out-Null
    $sources = @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -Filter "*.java" |
        ForEach-Object { $_.FullName })
    & $javac -d $ClassesRoot @sources
    if ($LASTEXITCODE -ne 0) {
        throw "javac failed for $SourceRoot"
    }
}

function Start-App {
    param([string[]]$Arguments)
    $proc = Start-Process -FilePath $java -ArgumentList $Arguments -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 1800
    if ($proc.HasExited) {
        throw "target JVM exited early: $($Arguments -join ' ')"
    }
    return $proc
}

function Assert-Contains {
    param([string[]]$Lines, [string]$Needle)
    $joined = $Lines -join "`n"
    if (-not $joined.Contains($Needle)) {
        throw "missing '$Needle'"
    }
}

function Assert-OutputContains {
    param([string[]]$Output, [string]$Needle, [string]$Label)
    $joined = $Output -join "`n"
    if (-not $joined.Contains($Needle)) {
        throw "$Label missing '$Needle'"
    }
}

function Invoke-DumpGate {
    param(
        [string]$ScenarioRoot,
        [string]$ScenarioName,
        [System.Diagnostics.Process]$Process,
        [string]$TargetClass
    )

    $inspectDir = New-Dir (Join-Path $ScenarioRoot "inspect")
    $summary = & $mjdump inspect -p $Process.Id -o $inspectDir --external --class $TargetClass --filter-preview --limit 32 --wait-ms 10000
    if ($LASTEXITCODE -ne 0) {
        throw "$ScenarioName inspect failed"
    }
    Assert-Contains $summary "matched: 1"
    $inspect = Get-Content -LiteralPath (Join-Path $inspectDir "_inspect.json") -Raw | ConvertFrom-Json
    $matched = @($inspect.classes) | Where-Object { $_.name -eq ($TargetClass -replace '\.', '/') -and $_.matched } |
        Select-Object -First 1
    if (-not $matched) {
        throw "$ScenarioName inspect did not list matched $TargetClass"
    }

    $dumpDir = New-Dir (Join-Path $ScenarioRoot "dump")
    & $mjdump dump -p $Process.Id -o $dumpDir --class $TargetClass --module unnamed --group-by package --wait-ms 10000 --quiet
    if ($LASTEXITCODE -ne 0) {
        throw "$ScenarioName dump failed"
    }

    $classFile = Join-Path $dumpDir ("_by_package\" + ($TargetClass -replace '\.', '\') + ".class")
    if (-not (Test-Path -LiteralPath $classFile)) {
        throw "$ScenarioName dumped class not found: $classFile"
    }

    $status = & $mjdump status -o $dumpDir
    if ($LASTEXITCODE -ne 0) {
        throw "$ScenarioName status failed"
    }
    Assert-Contains $status "status: completed"
    Assert-Contains $status "classWrites: 1"
    Assert-Contains $status "sweepMatched: 1"

    $index = Get-Content -LiteralPath (Join-Path $dumpDir "_index.json") -Raw | ConvertFrom-Json
    if ($index.filterHits -ne 1 -or @($index.dumpedClasses).Count -ne 1) {
        throw "$ScenarioName index summary mismatch"
    }
    if (-not $index.dumpedClasses[0].classMagicValid) {
        throw "$ScenarioName index class magic invalid"
    }
    if ([string]::IsNullOrWhiteSpace($index.dumpedClasses[0].hash)) {
        throw "$ScenarioName index missing hash"
    }

    $jarPath = Join-Path $dumpDir "dumped.jar"
    & $mjdump pack -i $dumpDir -o $jarPath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "$ScenarioName pack failed"
    }
    & $javap -classpath $jarPath $TargetClass | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "$ScenarioName javap could not read packed dumped class"
    }

    $simpleName = ($TargetClass -split '\.')[-1]
    if ($mjt) {
        $stringsOutput = & $mjt strings -i $dumpDir
        if ($LASTEXITCODE -ne 0) {
            throw "$ScenarioName mjt strings failed on dump directory"
        }
        Assert-OutputContains $stringsOutput $simpleName "$ScenarioName mjt strings"

        $internalName = $TargetClass -replace '\.', '/'
        $disasmOutput = & $mjt disasm --legacy -i $dumpDir -c $internalName
        if ($LASTEXITCODE -ne 0) {
            throw "$ScenarioName mjt disasm failed on dump directory"
        }
        Assert-OutputContains $disasmOutput "Class: $internalName" "$ScenarioName mjt disasm"

        $infoOutput = & $mjt info -i $jarPath
        if ($LASTEXITCODE -ne 0) {
            throw "$ScenarioName mjt info failed on packed jar"
        }
        Assert-OutputContains $infoOutput "Class files:" "$ScenarioName mjt info"
    } else {
        Write-Host "skipping mjt downstream checks for $ScenarioName"
    }
}

function Invoke-FatJarScenario {
    param([string]$Root)
    $work = New-Dir (Join-Path $Root "fat_jar")
    $src = New-Dir (Join-Path $work "src")
    $classes = Join-Path $work "classes"

    Write-AsciiFile (Join-Path $src "app\real\fat\FatJarMain.java") @"
package app.real.fat;
public class FatJarMain {
    public static void main(String[] args) throws Exception {
        System.out.println(new FatController().handle());
        long deadline = System.currentTimeMillis() + 30000L;
        while (System.currentTimeMillis() < deadline) {
            Thread.sleep(200L);
        }
    }
}
"@
    Write-AsciiFile (Join-Path $src "app\real\fat\FatController.java") @"
package app.real.fat;
public class FatController {
    public String handle() {
        return new FatService().message();
    }
}
"@
    Write-AsciiFile (Join-Path $src "app\real\fat\FatService.java") @"
package app.real.fat;
public class FatService {
    public String message() {
        return "fat-service";
    }
}
"@
    Compile-Sources $src $classes

    $appJar = Join-Path $work "fat-app.jar"
    & $jar --create --file $appJar --main-class app.real.fat.FatJarMain -C $classes .
    if ($LASTEXITCODE -ne 0) {
        throw "jar failed for fat app"
    }

    $proc = Start-App @("-jar", $appJar)
    try {
        Invoke-DumpGate $work "fat_jar" $proc "app.real.fat.FatController"
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

function Invoke-DynamicLoaderScenario {
    param([string]$Root)
    $work = New-Dir (Join-Path $Root "dynamic_loader")
    $mainSrc = New-Dir (Join-Path $work "main_src")
    $pluginSrc = New-Dir (Join-Path $work "plugin_src")
    $mainClasses = Join-Path $work "main_classes"
    $pluginClasses = Join-Path $work "plugin_classes"

    Write-AsciiFile (Join-Path $mainSrc "app\real\dynamic\DynamicMain.java") @"
package app.real.dynamic;
import java.io.File;
import java.net.URL;
import java.net.URLClassLoader;
public class DynamicMain {
    private static Object plugin;
    public static void main(String[] args) throws Exception {
        URL url = new File(args[0]).toURI().toURL();
        URLClassLoader loader = new URLClassLoader(new URL[] { url }, DynamicMain.class.getClassLoader());
        Class<?> type = Class.forName("app.real.dynamic.plugin.DynamicPlugin", true, loader);
        plugin = type.getDeclaredConstructor().newInstance();
        System.out.println(type.getMethod("value").invoke(plugin));
        long deadline = System.currentTimeMillis() + 30000L;
        while (System.currentTimeMillis() < deadline) {
            Thread.sleep(200L);
        }
    }
}
"@
    Write-AsciiFile (Join-Path $pluginSrc "app\real\dynamic\plugin\DynamicPlugin.java") @"
package app.real.dynamic.plugin;
public class DynamicPlugin {
    public String value() {
        return "dynamic-plugin";
    }
}
"@
    Compile-Sources $mainSrc $mainClasses
    Compile-Sources $pluginSrc $pluginClasses
    $pluginJar = Join-Path $work "dynamic-plugin.jar"
    & $jar --create --file $pluginJar -C $pluginClasses .
    if ($LASTEXITCODE -ne 0) {
        throw "jar failed for dynamic plugin"
    }

    $proc = Start-App @("-cp", $mainClasses, "app.real.dynamic.DynamicMain", $pluginJar)
    try {
        Invoke-DumpGate $work "dynamic_loader" $proc "app.real.dynamic.plugin.DynamicPlugin"
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

function Invoke-EncryptedDefineScenario {
    param([string]$Root)
    $work = New-Dir (Join-Path $Root "encrypted_define")
    $payloadSrc = New-Dir (Join-Path $work "payload_src")
    $payloadClasses = Join-Path $work "payload_classes"
    $mainSrc = New-Dir (Join-Path $work "main_src")
    $mainClasses = Join-Path $work "main_classes"

    Write-AsciiFile (Join-Path $payloadSrc "app\real\secret\SecretPayload.java") @"
package app.real.secret;
public class SecretPayload {
    public String value() {
        return "secret-payload";
    }
}
"@
    Compile-Sources $payloadSrc $payloadClasses
    $payloadClass = Join-Path $payloadClasses "app\real\secret\SecretPayload.class"
    [byte[]]$payloadBytes = [System.IO.File]::ReadAllBytes($payloadClass)
    $encoded = ($payloadBytes | ForEach-Object { [string]([int]($_ -bxor 0x5a)) }) -join ","

    Write-AsciiFile (Join-Path $mainSrc "app\real\secret\EncryptedMain.java") @"
package app.real.secret;
public class EncryptedMain {
    private static Class<?> payloadClass;
    private static Object payloadInstance;
    private static final int[] ENCRYPTED = new int[] { $encoded };
    static class Loader extends ClassLoader {
        Class<?> loadPayload() {
            byte[] bytes = new byte[ENCRYPTED.length];
            for (int i = 0; i < ENCRYPTED.length; ++i) {
                bytes[i] = (byte)(ENCRYPTED[i] ^ 0x5a);
            }
            return defineClass(null, bytes, 0, bytes.length);
        }
    }
    public static void main(String[] args) throws Exception {
        payloadClass = new Loader().loadPayload();
        payloadInstance = payloadClass.getDeclaredConstructor().newInstance();
        System.out.println(payloadClass.getMethod("value").invoke(payloadInstance));
        long deadline = System.currentTimeMillis() + 30000L;
        while (System.currentTimeMillis() < deadline) {
            Thread.sleep(200L);
        }
    }
}
"@
    Compile-Sources $mainSrc $mainClasses

    $proc = Start-App @("-cp", $mainClasses, "app.real.secret.EncryptedMain")
    try {
        Invoke-DumpGate $work "encrypted_define" $proc "app.real.secret.SecretPayload"
    }
    finally {
        if ($proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    }
}

$stamp = "$(Get-Date -Format 'yyyyMMdd_HHmmss_ffff')_$PID"
$root = New-Dir (Join-Path $repoRoot "build\smoke\smoke_real_apps\$stamp")

Invoke-FatJarScenario $root
Invoke-DynamicLoaderScenario $root
Invoke-EncryptedDefineScenario $root

Write-Host "real app dump smoke passed: $root"
