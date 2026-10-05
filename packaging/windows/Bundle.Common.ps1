function Test-SmCodexDesktopRunning {
    $matches = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        ($_.Name -ieq 'ChatGPT.exe' -and [string]$_.ExecutablePath -match 'OpenAI\.Codex_') -or
        ($_.Name -ieq 'codex.exe' -and [string]$_.CommandLine -match '\bapp-server\b') -or
        ($_.Name -ieq 'semantic-memory-mcp.exe') -or
        ($_.Name -ieq 'semantic-memory-manager.exe')
    })
    return $matches
}

function Assert-SmCodexDesktopStopped {
    param([string]$UserHome,[switch]$AllowRunningCodexForIsolatedTest)
    if ($AllowRunningCodexForIsolatedTest) {
        $actual = [IO.Path]::GetFullPath($env:USERPROFILE).TrimEnd('\')
        $requested = [IO.Path]::GetFullPath($UserHome).TrimEnd('\')
        if ([string]::Equals($actual,$requested,[StringComparison]::OrdinalIgnoreCase)) {
            throw 'Isolated-test bypass is forbidden for the real user profile.'
        }
        return
    }
    $running = @(Test-SmCodexDesktopRunning)
    if ($running.Count -gt 0) {
        $ids = ($running | ForEach-Object { [string]$_.ProcessId }) -join ','
        throw "CODEX_DESKTOP_RUNNING: Fully exit Codex Desktop before install or repair. Detected PID(s): $ids"
    }
}

function Invoke-SmJsonScript {
    param([string]$Script,[string[]]$Arguments)
    $named = @{}
    for ($i = 0; $i -lt $Arguments.Count; $i++) {
        $key = $Arguments[$i].TrimStart('-')
        if (($i + 1) -lt $Arguments.Count -and -not $Arguments[$i + 1].StartsWith('-')) {
            $named[$key] = $Arguments[$i + 1]; $i++
        } else { $named[$key] = $true }
    }
    $global:LASTEXITCODE = 0
    $lines = @(& $Script @named 2>&1)
    $code = $LASTEXITCODE
    $text = ($lines | ForEach-Object { [string]$_ }) -join "`n"
    if ($code -ne 0) { throw "Subprocess failed (exit=$code): $text" }
    try { return ($text | ConvertFrom-Json -ErrorAction Stop) }
    catch { throw "Subprocess did not return valid JSON: $text" }
}

function Get-SmExpectation($state,$property) {
    if (-not [bool]$state.exists) { return 'ABSENT' }
    return [string]$state.$property
}

