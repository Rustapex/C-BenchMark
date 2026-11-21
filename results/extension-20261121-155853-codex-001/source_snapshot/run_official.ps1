param(
    [Parameter(Mandatory)][string]$WordsDirectory,
    [Parameter(Mandatory)][string]$ProbeRecord,
    [Parameter(Mandatory)][string]$ExtensionRun,
    [ValidateSet(1000,10000,100000)][int]$N = 1000,
    [ValidateSet('A5','A6_SEARCH','A6_INSERT','A6_DELETE')][string]$Experiment = 'A5',
    [ValidateSet('DLL','AVL','HASH')][string]$Structure = 'AVL',
    [ValidateSet('codex','user')][string]$Actor = 'user'
)

$ErrorActionPreference = 'Stop'
$probe = Get-Content -LiteralPath $ProbeRecord -Raw | ConvertFrom-Json
if ($probe.timer_policy_id -ne 'CLOCK-STD-C-01' -or $probe.probe_source -ne 'RELEASE_SNAPSHOT' -or
    $probe.probe_status -ne 'OK' -or
    $probe.run_status -ne 'COMPLETED' -or $probe.process_exit_code -ne 0 -or
    -not $probe.overhead_completed -or $probe.timed_out) { throw 'Probe is not a completed CLOCK-STD-C-01 observation.' }
if ($probe.stdout -notmatch 'q_ms=([0-9]+(?:\.[0-9]+)?)') { throw 'Probe q_ms missing.' }
$qMs = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
if ($qMs -le 0) { throw 'Probe q_ms must be positive.' }
$extensionManifestPath = Join-Path $ExtensionRun 'manifest.json'
$extension = Get-Content -LiteralPath $extensionManifestPath -Raw | ConvertFrom-Json
if ($extension.stage -ne 'extension_accuracy' -or $extension.configuration -ne 'Release' -or
    $extension.status -ne 'COMPLETED') { throw 'Completed Release extension snapshot is required.' }
$exe = Join-Path $ExtensionRun 'source_snapshot\build\x64\Release\WordDictionaryPrototype.exe'
if (-not (Test-Path -LiteralPath $exe) -or
    (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $extension.exe_sha256) {
    throw 'Snapshot executable is absent or has changed.'
}
if ($probe.exe_sha256 -ne $extension.exe_sha256) { throw 'Probe and official runner executables differ.' }
$words = Join-Path $WordsDirectory 'words.txt'
if (-not (Test-Path -LiteralPath $words)) { throw "Missing original input: $words" }
$inputHashBefore = (Get-FileHash -LiteralPath $words -Algorithm SHA256).Hash
$resultsRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'results'
New-Item -ItemType Directory -Path $resultsRoot -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDir = $null
for ($ordinal = 1; $ordinal -le 999; ++$ordinal) {
    $candidate = Join-Path $resultsRoot ('official-{0}-{1}-{2:000}' -f $stamp, $Actor, $ordinal)
    if (-not (Test-Path -LiteralPath $candidate)) { $runDir = $candidate; break }
}
if (-not $runDir) { throw 'No unused official run directory.' }
New-Item -ItemType Directory -Path $runDir -ErrorAction Stop | Out-Null
$runId = Split-Path $runDir -Leaf
$buildId = ('release-' + $extension.exe_sha256.Substring(0,12).ToLowerInvariant())
$frozenAt = (Get-Date).ToString('o')
$console = Join-Path $runDir 'runner.console.log'
& $exe --official $words $N $Experiment $Structure $runDir $runId $buildId $qMs $frozenAt 2>&1 |
    Out-File -LiteralPath $console -Encoding utf8
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) { throw "Runner failed with exit $exitCode. Evidence: $runDir" }
$rawPath = Join-Path $runDir 'raw.csv'
$summaryPath = Join-Path $runDir 'summary.csv'
$manifestPath = Join-Path $runDir 'manifest.json'
$raw = @(Import-Csv -LiteralPath $rawPath)
$summary = @(Import-Csv -LiteralPath $summaryPath)
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($raw.Count -ne 5 -or $summary.Count -ne 1 -or $manifest.status -ne 'PENDING_PROVENANCE') {
    throw "Incomplete producer files: $runDir"
}
$condition = "$Experiment-$Structure-n$N"
$times = @()
for ($i = 0; $i -lt 5; ++$i) {
    $row = $raw[$i]
    if ($row.run_id -ne $runId -or $row.condition_id -ne $condition -or
        $row.build_id -ne $buildId -or $row.repeat -ne [string]($i + 1) -or
        $row.validation_status -ne 'PASS' -or $row.time_status -ne 'VALID' -or
        $row.actual_hits -ne $row.expected_hits) { throw "Invalid raw row $i in $runDir" }
    $elapsed = [double]::Parse($row.elapsed_ms, [Globalization.CultureInfo]::InvariantCulture)
    $expectedResolution = if ($elapsed -le 10 * $qMs) { 'BELOW_RESOLUTION' } else { 'OK' }
    if ($row.resolution_status -ne $expectedResolution) { throw "Wrong resolution status in row $i" }
    $times += $elapsed
}
$times = @($times | Sort-Object)
$median = $times[2]
$summaryMedian = [double]::Parse($summary[0].median_ms, [Globalization.CultureInfo]::InvariantCulture)
$summaryMin = [double]::Parse($summary[0].min_ms, [Globalization.CultureInfo]::InvariantCulture)
$summaryMax = [double]::Parse($summary[0].max_ms, [Globalization.CultureInfo]::InvariantCulture)
$expectedComparison = if (@($raw | Where-Object resolution_status -eq 'BELOW_RESOLUTION').Count -gt 0) {
    'BELOW_RESOLUTION'
} else { 'COMPARABLE' }
if ($summary[0].run_id -ne $runId -or $summary[0].condition_id -ne $condition -or
    $summary[0].valid_repeats -ne '5' -or $summaryMedian -ne $median -or
    $summaryMin -ne $times[0] -or $summaryMax -ne $times[4] -or
    $summary[0].comparison_status -ne $expectedComparison) {
    throw "Summary does not independently match raw: $runDir"
}
$inputHashAfter = (Get-FileHash -LiteralPath $words -Algorithm SHA256).Hash
if ($inputHashBefore -ne $inputHashAfter) { throw 'Input changed during official run.' }
$manifest.probe_status = 'OK'
$manifest.policy_frozen_at = $frozenAt
$manifest.policy_decision_reason = 'Completed probe with overhead and exit 0; q applied before official run.'
$manifest | Add-Member -NotePropertyName probe_record_sha256 -NotePropertyValue ((Get-FileHash -LiteralPath $ProbeRecord -Algorithm SHA256).Hash)
$manifest | Add-Member -NotePropertyName extension_manifest_sha256 -NotePropertyValue ((Get-FileHash -LiteralPath $extensionManifestPath -Algorithm SHA256).Hash)
$manifest | Add-Member -NotePropertyName exe_sha256 -NotePropertyValue $extension.exe_sha256
$manifest | Add-Member -NotePropertyName input_sha256 -NotePropertyValue $inputHashAfter
$manifest | Add-Member -NotePropertyName compiler_options -NotePropertyValue 'C17 x64 Release /O2'
$manifest | Add-Member -NotePropertyName independent_median_ms -NotePropertyValue $median
$manifest | Add-Member -NotePropertyName actor -NotePropertyValue $Actor
$manifest.status = 'COMPLETED'
$json = $manifest | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText($manifestPath, $json + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))
Write-Host "run_id=$runId status=COMPLETED median_ms=$median result_dir=$runDir"
