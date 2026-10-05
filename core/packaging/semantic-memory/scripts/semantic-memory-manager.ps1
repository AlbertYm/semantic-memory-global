param([switch]$VerifyOnly)

$ErrorActionPreference = 'Stop'
$root = $env:SEMANTIC_MEMORY_HOME
if (-not $root) {
    $local = $env:LOCALAPPDATA
    if (-not $local) { $local = [Environment]::GetFolderPath('LocalApplicationData') }
    $root = Join-Path $local 'SemanticMemory'
}
$launcher = Join-Path $root 'bin\semantic-memory-launcher.ps1'
if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
    throw "Semantic Memory stable launcher is not installed: $launcher"
}
& $launcher -Mode manager -VerifyOnly:$VerifyOnly
exit $LASTEXITCODE
