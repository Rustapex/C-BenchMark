param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateSet('codex', 'user')][string]$Actor = 'user'
)

$ErrorActionPreference = 'Stop'
$projectDir = $PSScriptRoot
$repository = Split-Path $projectDir -Parent
$solution = Join-Path $projectDir 'WordDictionaryPrototype.sln'
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'Visual Studio C++ tools were not found.' }
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild not found: $msbuild" }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$results = Join-Path $repository 'results'
New-Item -ItemType Directory -Path $results -Force | Out-Null
$runDir = $null
for ($number = 1; $number -le 999; ++$number) {
    $candidate = Join-Path $results ('proto-{0}-{1}-{2:000}' -f $stamp, $Actor, $number)
    if (-not (Test-Path -LiteralPath $candidate)) {
        $runDir = $candidate
        break
    }
}
if (-not $runDir) { throw 'No unused run ID is available.' }
New-Item -ItemType Directory -Path $runDir | Out-Null
$snapshot = Join-Path $runDir 'source_snapshot'
New-Item -ItemType Directory -Path $snapshot | Out-Null
$sourceNames = @('dict.h', 'dict.c', 'tests.h', 'tests.c', 'main.c',
    'WordDictionaryPrototype.sln', 'WordDictionaryPrototype.vcxproj', 'run_proto.ps1', 'README.md')
$sourceHashes = [ordered]@{}
foreach ($name in $sourceNames) {
    $source = Join-Path $projectDir $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Missing source: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $snapshot $name)
    $sourceHashes[$name] = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
}

$mode = if ($Configuration -eq 'Debug') { 'all' } else { 'core' }
$exe = Join-Path $projectDir "build\x64\$Configuration\WordDictionaryPrototype.exe"
$manifestPath = Join-Path $runDir 'manifest.json'
$manifest = [ordered]@{
    schema_version = 1
    stage = 'prototype'
    run_id = Split-Path $runDir -Leaf
    runner_actor = $Actor
    started_at = (Get-Date).ToString('o')
    status = 'RUNNING'
    configuration = $Configuration
    platform = 'x64'
    c_standard = 'C17'
    test_mode = $mode
    fixture_version = 'prototype-v1'
    timer_policy_id = 'NOT_APPLICABLE_PROTOTYPE'
    source_sha256 = $sourceHashes
    exe_path = $exe
    exe_sha256 = $null
    build_exit_code = $null
    test_exit_code = $null
    checks = $null
    failed_checks = $null
    required_case_ids = @()
    missing_case_ids = @()
    completed_at = $null
}
function Save-Manifest {
    $json = $manifest | ConvertTo-Json -Depth 7
    [IO.File]::WriteAllText($manifestPath, $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}
Save-Manifest

$buildLog = Join-Path $runDir 'build.log'
('Command: "{0}" "{1}" /t:Rebuild /p:Configuration={2} /p:Platform=x64 /v:normal /nologo' -f
    $msbuild, $solution, $Configuration) | Set-Content -LiteralPath $buildLog -Encoding utf8
& $msbuild $solution /t:Rebuild "/p:Configuration=$Configuration" /p:Platform=x64 /v:normal /nologo 2>&1 |
    Out-File -LiteralPath $buildLog -Append -Encoding utf8
$manifest.build_exit_code = $LASTEXITCODE
if ($manifest.build_exit_code -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
    $manifest.status = 'FAILED_BUILD'
    $manifest.completed_at = (Get-Date).ToString('o')
    Save-Manifest
    Write-Error "Build failed. Run: $runDir"
    exit 3
}
$manifest.exe_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
$consoleLog = Join-Path $runDir 'console.log'
$accuracyLog = Join-Path $runDir 'accuracy.log'
& $exe --test $mode --log $accuracyLog 2>&1 |
    Out-File -LiteralPath $consoleLog -Encoding utf8
$manifest.test_exit_code = $LASTEXITCODE
$summaryLine = Get-Content -LiteralPath $consoleLog | Where-Object { $_ -like 'TEST_SUMMARY *' } | Select-Object -Last 1
if ($summaryLine -match 'checks=(\d+) failed=(\d+)') {
    $manifest.checks = [int]$Matches[1]
    $manifest.failed_checks = [int]$Matches[2]
}
$required = @('T02:CRUD', 'T02:INPUT', 'T02:MODEL', 'T03:SORTED_VISIT',
    'T04:LIST_LINKS', 'T05:INSERT_ROTATIONS', 'T05:AVL:OTHER_DELETE',
    'T06:HASH', 'T07:API', 'prototype:hash-v1:FULL', 'extension:0.2:T06-FULL')
$required += 1..9 | ForEach-Object { 'prototype:avl-del-v1:D{0:00}' -f $_ }
if ($Configuration -eq 'Debug') { $required += @('T08:ALLOCATION', 'CHECK-FAULT:prototype-v1') }
$manifest.required_case_ids = $required
if (Test-Path -LiteralPath $accuracyLog) {
    $accuracyText = Get-Content -LiteralPath $accuracyLog -Raw
    $manifest.missing_case_ids = @($required | Where-Object { -not $accuracyText.Contains("$_`t") })
} else {
    $manifest.missing_case_ids = $required
}
$manifest.status = if ($manifest.test_exit_code -eq 0 -and
    $manifest.checks -gt 0 -and $manifest.failed_checks -eq 0 -and
    $manifest.missing_case_ids.Count -eq 0) { 'COMPLETED' } else { 'FAILED_TEST' }
$manifest.completed_at = (Get-Date).ToString('o')
Save-Manifest
Write-Host ("run_id={0} status={1} checks={2} failed={3}" -f
    $manifest.run_id, $manifest.status, $manifest.checks, $manifest.failed_checks)
Write-Host "result_dir=$runDir"
if ($manifest.status -ne 'COMPLETED') { exit 3 }
exit 0
