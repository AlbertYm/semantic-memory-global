param(
    [Parameter(Position=0,Mandatory=$true)]
    [ValidateSet('mcp','recall','post-tool','stop')][string]$Action
)

$ErrorActionPreference = 'Stop'
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
& $hook $Action
exit $LASTEXITCODE
