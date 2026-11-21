param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateSet('codex', 'user')][string]$Actor = 'user'
)

$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$repository = Split-Path -Parent $project
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'Visual Studio C++ tools unavailable.' }
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild unavailable: $msbuild" }

$resultsRoot = Join-Path $repository 'results'
New-Item -ItemType Directory -Path $resultsRoot -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDir = $null
for ($ordinal = 1; $ordinal -le 999; ++$ordinal) {
    $candidate = Join-Path $resultsRoot ('extension-{0}-{1}-{2:000}' -f $stamp, $Actor, $ordinal)
    if (-not (Test-Path -LiteralPath $candidate)) { $runDir = $candidate; break }
}
if (-not $runDir) { throw 'No unused run directory.' }
New-Item -ItemType Directory -Path $runDir -ErrorAction Stop | Out-Null
$snapshot = Join-Path $runDir 'source_snapshot'
New-Item -ItemType Directory -Path $snapshot -ErrorAction Stop | Out-Null
$sourceNames = @('dict.c','dict.h','tests.c','tests.h','main.c',
    'workload.c','workload.h','timer.c','timer.h','results.c','results.h',
    'extension_tests.c','extension_tests.h','runner.c','runner.h',
    'WordDictionaryPrototype.sln','WordDictionaryPrototype.vcxproj',
    'run_extension.ps1','run_probe.ps1','run_init_pilot.ps1','run_proto.ps1','README.md')
$sourceHashes = [ordered]@{}
foreach ($name in $sourceNames) {
    $source = Join-Path $project $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Missing source: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $snapshot $name) -ErrorAction Stop
    $sourceHashes[$name] = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
}
$fixtureSource = Join-Path $project 'fixtures'
if (Test-Path -LiteralPath $fixtureSource) {
    Copy-Item -LiteralPath $fixtureSource -Destination (Join-Path $snapshot 'fixtures') -Recurse -ErrorAction Stop
    Get-ChildItem -LiteralPath $fixtureSource -File | ForEach-Object {
        $sourceHashes[('fixtures/' + $_.Name)] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
}

$manifestPath = Join-Path $runDir 'manifest.json'
$manifest = [ordered]@{
    schema_version = 2
    stage = 'extension_accuracy'
    run_id = Split-Path $runDir -Leaf
    actor = $Actor
    started_at = (Get-Date).ToString('o')
    status = 'RUNNING'
    configuration = $Configuration
    platform = 'x64'
    c_standard = 'C17'
    optimization = if ($Configuration -eq 'Release') { '/O2' } else { '/Od' }
    source_snapshot = $snapshot
    source_sha256 = $sourceHashes
    build_log = (Join-Path $runDir 'build.log')
    build_exit_code = $null
    exe_sha256 = $null
    suites = @()
    completed_at = $null
}
function Save-Manifest {
    $json = $manifest | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($manifestPath, $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}
Save-Manifest
$solution = Join-Path $snapshot 'WordDictionaryPrototype.sln'
$buildLog = $manifest.build_log
('Command: "{0}" "{1}" /t:Rebuild /p:Configuration={2} /p:Platform=x64 /v:normal /nologo' -f
    $msbuild, $solution, $Configuration) | Set-Content -LiteralPath $buildLog -Encoding utf8
& $msbuild $solution /t:Rebuild "/p:Configuration=$Configuration" /p:Platform=x64 /v:normal /nologo 2>&1 |
    Out-File -LiteralPath $buildLog -Append -Encoding utf8
$manifest.build_exit_code = $LASTEXITCODE
$exe = Join-Path $snapshot "build\x64\$Configuration\WordDictionaryPrototype.exe"
if ($manifest.build_exit_code -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
    $manifest.status = 'FAILED_BUILD'
    $manifest.completed_at = (Get-Date).ToString('o')
    Save-Manifest
    Write-Host "result_dir=$runDir status=FAILED_BUILD"
    exit 3
}
$manifest.exe_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
$modes = if ($Configuration -eq 'Debug') { @('all','extension') } else { @('core','extension') }
$requiredCore = @('T02:CRUD','prototype:avl-del-v1:D01','T06:HASH','extension:0.2:T06-FULL')
$requiredExtension = @('T09:FULL_REFERENCE:A5:1000','T09:FULL_REFERENCE:A6_SEARCH:100000',
    'T09:FULL_REFERENCE:A6_INSERT:100000','T09:FULL_REFERENCE:A6_DELETE:100000',
    'T12:SCHEMA2_FIXTURE','extension:0.2:T05-D09')
$failed = $false
foreach ($mode in $modes) {
    $log = Join-Path $runDir "$mode.tsv"
    $console = Join-Path $runDir "$mode.console.log"
    & $exe --test $mode --log $log 2>&1 | Out-File -LiteralPath $console -Encoding utf8
    $exitCode = $LASTEXITCODE
    $line = Get-Content -LiteralPath $console | Where-Object { $_ -like 'TEST_SUMMARY *' } | Select-Object -Last 1
    $checks = 0; $failedChecks = -1
    if ($line -match 'checks=(\d+) failed=(\d+)') {
        $checks = [int]$Matches[1]; $failedChecks = [int]$Matches[2]
    }
    $required = if ($mode -eq 'extension') { $requiredExtension } else { $requiredCore }
    $missing = @($required | Where-Object {
        -not (Test-Path -LiteralPath $log) -or -not (Select-String -LiteralPath $log -Pattern ([regex]::Escape($_) + "`t") -Quiet)
    })
    $suite = [ordered]@{
        mode = $mode; log = $log; console = $console; exit_code = $exitCode
        checks = $checks; failed_checks = $failedChecks
        required_ids = $required; missing_ids = $missing
    }
    $manifest.suites += $suite
    if ($exitCode -ne 0 -or $checks -le 0 -or $failedChecks -ne 0 -or $missing.Count -ne 0) { $failed = $true }
}
$manifest.status = if ($failed) { 'FAILED_TEST' } else { 'COMPLETED' }
$manifest.completed_at = (Get-Date).ToString('o')
Save-Manifest
Write-Host ("run_id={0} status={1} result_dir={2}" -f $manifest.run_id, $manifest.status, $runDir)
if ($failed) { exit 3 }
