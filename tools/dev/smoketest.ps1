# Smoke test without a player: starts the game (one or two copies) and the local server, waits,
# then checks the logs. Catches a mod that crashes or fails to start; gameplay still needs a person.
# Exit code 0 when everything looks fine.
param(
    [string]$GameDir = "D:\Steam\steamapps\common\NieRAutomata",
    [int]$Instances = 1,
    [int]$Seconds = 60,
    [switch]$KeepRunning # Leave the games open after the check.
)

$ErrorActionPreference = "Stop"

if (Get-Process NieRAutomata -ErrorAction SilentlyContinue) {
    Write-Host "FAIL: the game is already running, close it first (the smoke test kills the copies it starts)."
    exit 2
}

$started = Get-Date
$problems = @()
$games = @()

# Local server, like "Host Game" starts it.
$serverDir = "$GameDir\automatamp_server"
$serverOut = "$env:TEMP\nier-coop-smoketest-server.txt"
$server = $null

if (Test-Path "$serverDir\server.exe") {
    $server = Start-Process "$serverDir\server.exe" -ArgumentList "-mode", "server" -WorkingDirectory $serverDir `
        -RedirectStandardOutput $serverOut -RedirectStandardError "$serverOut.err" -WindowStyle Hidden -PassThru
}

for ($i = 0; $i -lt $Instances; $i++) {
    $games += Start-Process "$GameDir\NieRAutomata.exe" -WorkingDirectory $GameDir -PassThru
    Start-Sleep -Seconds 5
}

$deadline = $started.AddSeconds($Seconds)

while ((Get-Date) -lt $deadline) {
    if ($games | Where-Object { $_.HasExited }) { break }
    Start-Sleep -Seconds 2
}

foreach ($game in $games) {
    if ($game.HasExited) {
        $problems += "game process $($game.Id) exited with code $($game.ExitCode)"
    }
}

if ($server -ne $null -and $server.HasExited) {
    $problems += "server exited with code $($server.ExitCode)"
}

$logs = Get-ChildItem "$GameDir\automatamp_log*.txt" | Where-Object { $_.LastWriteTime -gt $started }

if ($logs.Count -lt $Instances) {
    $problems += "expected $Instances fresh log(s), found $($logs.Count)"
}

$required = @("AutomataMP entry", "AutomataMP initialized", "[MidHooks] on_update_function", "[EntityList] EntityList:")

foreach ($log in $logs) {
    $text = Get-Content $log.FullName -Raw

    foreach ($line in $required) {
        if (-not $text.Contains($line)) {
            $problems += "$($log.Name): no line '$line'"
        }
    }

    # Errors worth a look; the DirectX 11/12 retries at startup are normal.
    $errors = Select-String -Path $log.FullName -Pattern "\[(error|critical)\]" |
        Where-Object { $_.Line -notmatch "Failed to initialize AutomataMP on DirectX 1[12]" }

    foreach ($e in $errors | Select-Object -First 10) {
        $problems += "$($log.Name): $($e.Line)"
    }
}

$dump = Get-Item "$GameDir\automatamp_crash.dmp" -ErrorAction SilentlyContinue

if ($dump -ne $null -and $dump.LastWriteTime -gt $started) {
    $problems += "new crash dump: $($dump.FullName)"
}

if (-not $KeepRunning) {
    $games | Where-Object { -not $_.HasExited } | ForEach-Object { Stop-Process -Id $_.Id -Force }
    if ($server -ne $null -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}

Write-Host "Logs: $($logs.Name -join ', ')"

if (Test-Path $serverOut) {
    Write-Host "Server output:"
    Get-Content $serverOut, "$serverOut.err" -ErrorAction SilentlyContinue | Select-Object -Last 15 | ForEach-Object { Write-Host "  $_" }
}

if ($problems.Count -gt 0) {
    Write-Host "FAIL:"
    $problems | ForEach-Object { Write-Host "  $_" }
    exit 1
}

Write-Host "OK: $Instances game(s) ran $Seconds s, the mod started, no errors, no crash dump."
exit 0
