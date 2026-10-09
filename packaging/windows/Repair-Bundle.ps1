[CmdletBinding()]
param(
    [string]$UserHome = $env:USERPROFILE,
    [string]$InstallRoot,
    [string]$CodexHome,
    [switch]$AllowRunningCodexForIsolatedTest
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Bundle.Common.ps1')
if (-not $InstallRoot) { $InstallRoot = Join-Path $env:LOCALAPPDATA 'SemanticMemory' }
if (-not $CodexHome) { $CodexHome = Join-Path $UserHome '.codex' }
Assert-SmCodexDesktopStopped -UserHome $UserHome -AllowRunningCodexForIsolatedTest:$AllowRunningCodexForIsolatedTest
$statePath = Join-Path $InstallRoot 'install-bundle-state.json'
if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { throw 'Bundle state is missing. Run Install Semantic Memory.cmd for a first install.' }
$state = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
if ([string]$state.schema -ne 'semantic-memory-transfer-install/v1') { throw 'Unsupported bundle state schema.' }
$repairArguments = @('-Mode','Apply','-UserHome',$UserHome,'-InstallRoot',$InstallRoot,'-CodexHome',$CodexHome)
if ($AllowRunningCodexForIsolatedTest) { $repairArguments += '-AllowRunningCodexForIsolatedTest' }
$repair = Invoke-SmJsonScript (Join-Path $PSScriptRoot 'Repair-Codex-Memory.ps1') $repairArguments
if ([string]$repair.status -notin @('APPLIED_VERIFIED','REPLAYED_ZERO_WRITE')) { throw 'Persistent MCP config repair did not verify.' }
if ([string]$repair.status -eq 'APPLIED_VERIFIED') {
    $state.config_installed_sha256 = (Get-SmBundleSha256File (Join-Path $repair.transaction 'config.before.toml'))
    $state | Add-Member -NotePropertyName persistent_repair_transaction -NotePropertyValue ([string]$repair.transaction) -Force
    $state | Add-Member -NotePropertyName last_repair_at -NotePropertyValue ([DateTimeOffset]::Now.ToString('o')) -Force
    [IO.File]::WriteAllText($statePath,(($state|ConvertTo-Json -Depth 10)+"`n"),[Text.UTF8Encoding]::new($false))
}
& powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Verify-Bundle.ps1') -UserHome $UserHome -InstallRoot $InstallRoot -CodexHome $CodexHome
if ($LASTEXITCODE -ne 0) { throw 'Repair verification failed.' }
Write-Host 'Persistent MCP configuration repair and automatic verification completed.' -ForegroundColor Green
Write-Host 'Open Codex Desktop and verify the specialized tools in a new task.'
