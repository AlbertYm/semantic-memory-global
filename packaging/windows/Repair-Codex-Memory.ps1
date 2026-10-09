[CmdletBinding()]
param(
    [ValidateSet('Apply','Verify','Rollback')][string]$Mode = 'Apply',
    [string]$UserHome = $env:USERPROFILE,
    [string]$InstallRoot,
    [string]$CodexHome,
    [string]$MmcapiDatabase,
    [string]$PythonExe,
    [string]$TransactionPath,
    [switch]$AllowRunningCodexForIsolatedTest
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Bundle.Common.ps1')
if (-not $InstallRoot) {
    $InstallRoot = $env:SEMANTIC_MEMORY_HOME
    if (-not $InstallRoot) { $InstallRoot = Join-Path $env:LOCALAPPDATA 'SemanticMemory' }
}
if (-not $CodexHome) {
    $CodexHome = $env:CODEX_HOME
    if (-not $CodexHome) { $CodexHome = Join-Path $UserHome '.codex' }
}
if ($Mode -ne 'Verify') {
    Assert-SmCodexDesktopStopped -UserHome $UserHome -AllowRunningCodexForIsolatedTest:$AllowRunningCodexForIsolatedTest
}
if (-not $TransactionPath -and (Test-Path -LiteralPath (Join-Path $InstallRoot 'codex-memory-repair-state.json'))) {
    $pointer = Get-Content -LiteralPath (Join-Path $InstallRoot 'codex-memory-repair-state.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    $TransactionPath = [string]$pointer.transaction
}
if ($TransactionPath) {
    $transaction = Get-Content -LiteralPath (Join-Path $TransactionPath 'transaction.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    if (-not $PythonExe) { $PythonExe = [string]$transaction.registration.command }
    if (-not $MmcapiDatabase) { $MmcapiDatabase = [string]$transaction.mmcapi_db }
}
$PythonExe = Resolve-SmPython -PythonExe $PythonExe
$helper = Join-Path $PSScriptRoot 'memory-adapter\repair_codex_memory.py'
$arguments = @('-I',$helper,$Mode.ToLowerInvariant(), '--user-home',$UserHome,'--install-root',$InstallRoot,'--codex-home',$CodexHome)
if ($MmcapiDatabase) { $arguments += @('--mmcapi-db',$MmcapiDatabase) }
if ($TransactionPath) { $arguments += @('--transaction',$TransactionPath) }
& $PythonExe @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
