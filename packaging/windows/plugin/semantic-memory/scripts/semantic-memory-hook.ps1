param(
    [Parameter(Position=0,Mandatory=$true)]
    [ValidateSet('mcp','recall','post-tool','stop')][string]$Action
)

$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)
[Console]::InputEncoding = $utf8
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$root = $env:SEMANTIC_MEMORY_HOME
if (-not $root) {
    $local = $env:LOCALAPPDATA
    if (-not $local) { $local = [Environment]::GetFolderPath('LocalApplicationData') }
    $root = Join-Path $local 'SemanticMemory'
}
$hook = Join-Path $root 'bin\semantic-memory-hook.exe'
if (-not (Test-Path -LiteralPath $hook -PathType Leaf)) {
    throw "Semantic Memory stable hook entrypoint is not installed: $hook"
}
if ($Action -eq 'mcp') {
    throw 'The Personal Plugin hook wrapper does not launch MCP mode.'
}

if ($Action -eq 'stop') {
    # Stop must record the lifecycle result without returning a blocking decision
    # to Codex, because a blocking decision creates a continuation turn.
    $inputLine = [Console]::In.ReadLine()
    if ([string]::IsNullOrWhiteSpace($inputLine)) {
        exit 0
    }

    try {
        $stopPayload = $inputLine | ConvertFrom-Json -ErrorAction Stop
        if ($null -eq $stopPayload -or [string]$stopPayload.hook_event_name -ne 'Stop') {
            exit 0
        }

        $firstOutput = @($inputLine | & $hook stop 2>$null)
        $firstExitCode = $LASTEXITCODE
        if ($firstExitCode -ne 0 -or $firstOutput.Count -eq 0) {
            exit 0
        }

        $firstResult = ($firstOutput -join [Environment]::NewLine) | ConvertFrom-Json -ErrorAction Stop
        if ([string]$firstResult.decision -eq 'block') {
            $stopPayload.stop_hook_active = $true
            $boundedInput = $stopPayload | ConvertTo-Json -Compress -Depth 20
            $null = @($boundedInput | & $hook stop 2>$null)
        }
    } catch {
        # Hook failures are fail-open here; they must not create another UI turn.
    }

    exit 0
}

$outputLines = @(& $hook $Action)
$hookExitCode = $LASTEXITCODE
$outputLines | ForEach-Object { [Console]::Out.WriteLine($_) }

exit $hookExitCode
