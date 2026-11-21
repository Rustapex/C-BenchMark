param(
    [string]$Configuration = 'Release',
    [string]$OutputDirectory = '',
    [int]$TimeoutSeconds = 10,
    [string]$ProbeExecutable = '',
    [string]$ProbeArguments = '--probe'
)

$ErrorActionPreference = 'Stop'
if ($Configuration -notin @('Release', 'Debug') -or $TimeoutSeconds -lt 1) { throw 'Invalid probe configuration.' }
$root = Split-Path -Parent $PSScriptRoot
$exe = if ($ProbeExecutable) { $ProbeExecutable } else { Join-Path $PSScriptRoot "build\x64\$Configuration\WordDictionaryPrototype.exe" }
if (-not (Test-Path -LiteralPath $exe)) { throw "Build first: $exe" }
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $root ('results\probe-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$path = Join-Path $OutputDirectory 'probe.json'
if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite probe record: $path" }

$info = [System.Diagnostics.ProcessStartInfo]::new()
$info.FileName = $exe
$info.Arguments = $ProbeArguments
$info.WorkingDirectory = $PSScriptRoot
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $info
$watch = [System.Diagnostics.Stopwatch]::StartNew()
try {
    if (-not $process.Start()) { throw 'Probe process did not start.' }
    $completed = $process.WaitForExit($TimeoutSeconds * 1000)
    $watch.Stop()
    if (-not $completed) {
        $process.Kill()
        $process.WaitForExit()
    }
    $stdout = $process.StandardOutput.ReadToEnd().Trim()
    $stderr = $process.StandardError.ReadToEnd().Trim()
    $status = if ($stdout -match 'status=([A-Z_]+)') { $Matches[1] }
              elseif (-not $completed) { 'OBSERVATION_INTERRUPTED' }
              else { 'TIMER_ERROR' }
    $overheadComplete = $stdout -match 'PROBE_OVERHEAD\s+empty_pair_min_ticks='
    $runStatus = if ($completed -and $process.ExitCode -eq 0 -and $status -eq 'OK' -and $overheadComplete) { 'COMPLETED' }
                 elseif (-not $completed) { 'INTERRUPTED' }
                 else { 'FAILED' }
    $record = [ordered]@{
        timer_policy_id = 'CLOCK-STD-C-01'
        probe_source = if ($ProbeExecutable -or $ProbeArguments -ne '--probe') { 'FIXTURE' } else { 'OFFICIAL' }
        probe_status = $status
        run_status = $runStatus
        overhead_completed = [bool]$overheadComplete
        external_monotonic_watchdog_seconds = $TimeoutSeconds
        external_elapsed_seconds = [math]::Round($watch.Elapsed.TotalSeconds, 6)
        process_exit_code = if ($completed) { $process.ExitCode } else { $null }
        timed_out = -not $completed
        observed_at = (Get-Date).ToString('o')
        stdout = $stdout
        stderr = $stderr
    }
    $json = $record | ConvertTo-Json -Depth 4
    $stream = [System.IO.FileStream]::new($path, [System.IO.FileMode]::CreateNew)
    try {
        $writer = [System.IO.StreamWriter]::new($stream, [System.Text.UTF8Encoding]::new($false))
        try { $writer.WriteLine($json) } finally { $writer.Dispose() }
    } finally { $stream.Dispose() }
    Write-Output "PROBE_RECORD=$path"
    Write-Output "PROBE_STATUS=$status"
    Write-Output "PROBE_RUN_STATUS=$runStatus"
    Write-Output $stdout
    if ($runStatus -ne 'COMPLETED') { exit 7 }
} finally {
    $process.Dispose()
}
