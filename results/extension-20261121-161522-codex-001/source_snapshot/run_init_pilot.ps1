param(
    [ValidateSet(1000,10000,100000)][int]$N,
    [string]$WordsDirectory = '',
    [string]$OutputDirectory = '',
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'
if ($TimeoutSeconds -lt 1) { throw 'TimeoutSeconds must be positive.' }
if (-not $WordsDirectory) { throw 'Pass -WordsDirectory containing the original words.txt.' }
$exe = Join-Path $PSScriptRoot 'build\x64\Release\WordDictionaryPrototype.exe'
$words = Join-Path $WordsDirectory 'words.txt'
if (-not (Test-Path -LiteralPath $exe) -or -not (Test-Path -LiteralPath $words)) { throw 'Release build or words.txt missing.' }
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) ('results\init-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$path = Join-Path $OutputDirectory "init-$N.json"
if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite initialization record: $path" }

$info = [System.Diagnostics.ProcessStartInfo]::new()
$info.FileName = $exe
$info.Arguments = "--init words.txt $N"
$info.WorkingDirectory = $WordsDirectory
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $info
$watch = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $startedAt = (Get-Date).ToString('o')
    if (-not $process.Start()) { throw 'Initialization process did not start.' }
    $completed = $process.WaitForExit($TimeoutSeconds * 1000)
    $watch.Stop()
    $finishedAt = (Get-Date).ToString('o')
    if (-not $completed) {
        $process.Kill()
        $process.WaitForExit()
    }
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $status = if (-not $completed) { 'TIMEOUT' }
              elseif ($process.ExitCode -eq 0 -and $stdout -match "INIT_COMPLETE n=$N inserted=$N") { 'COMPLETE' }
              else { 'FAILED' }
    $record = [ordered]@{
        n = $N
        status = $status
        external_monotonic_cap_seconds = $TimeoutSeconds
        external_elapsed_seconds = [math]::Round($watch.Elapsed.TotalSeconds, 6)
        process_exit_code = if ($completed) { $process.ExitCode } else { $null }
        executable_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
        words_sha256 = (Get-FileHash -LiteralPath $words -Algorithm SHA256).Hash
        started_at = $startedAt
        finished_at = $finishedAt
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
    Write-Output "INIT_RECORD=$path"
    Write-Output "INIT_STATUS=$status"
    if ($stdout -match 'INIT_COMPLETE[^\r\n]*') { Write-Output $Matches[0] }
    if ($status -ne 'COMPLETE') { exit 7 }
} finally {
    $process.Dispose()
}
