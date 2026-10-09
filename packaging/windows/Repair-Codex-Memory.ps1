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
if (-not $PythonExe) {
    $python = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($python) { $PythonExe = $python.Source }
    else { throw 'PYTHON_311_REQUIRED: install Python 3.11+ or pass -PythonExe. No repair changes made.' }
}
$probe = @(& $PythonExe -I -c 'import sys; print(sys.executable); sys.exit(0 if sys.version_info >= (3,11) else 1)' 2>$null)
if ($LASTEXITCODE -ne 0 -or $probe.Count -ne 1) { throw 'PYTHON_311_REQUIRED: no repair changes made.' }
$PythonExe = ([string]$probe[0]).Trim()
$helper = Join-Path $PSScriptRoot 'memory-adapter\repair_codex_memory.py'
$arguments = @('-I',$helper,$Mode.ToLowerInvariant(), '--user-home',$UserHome,'--install-root',$InstallRoot,'--codex-home',$CodexHome)
if ($MmcapiDatabase) { $arguments += @('--mmcapi-db',$MmcapiDatabase) }
if ($TransactionPath) { $arguments += @('--transaction',$TransactionPath) }
& $PythonExe @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
