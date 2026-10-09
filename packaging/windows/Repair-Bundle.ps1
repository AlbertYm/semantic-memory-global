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
if (Test-Path -LiteralPath (Join-Path $InstallRoot 'codex-memory-repair-state.json') -PathType Leaf) {
    & (Join-Path $PSScriptRoot 'Repair-Codex-Memory.ps1') -Mode Apply -UserHome $UserHome -InstallRoot $InstallRoot -CodexHome $CodexHome -AllowRunningCodexForIsolatedTest:$AllowRunningCodexForIsolatedTest
    if ($LASTEXITCODE -ne 0) { throw 'Persistent MCP repair did not verify.' }
    & (Join-Path $PSScriptRoot 'Verify-Bundle.ps1') -UserHome $UserHome -InstallRoot $InstallRoot -CodexHome $CodexHome
    if ($LASTEXITCODE -ne 0) { throw 'Persistent repair verification failed.' }
    return
}
$statePath = Join-Path $InstallRoot 'install-bundle-state.json'
if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { throw 'Bundle state is missing. Run Install Semantic Memory.cmd for a first install.' }
$state = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
if ([string]$state.schema -ne 'semantic-memory-transfer-install/v1') { throw 'Unsupported bundle state schema.' }
$configPath = Join-Path $CodexHome 'config.toml'
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
    New-Item -ItemType Directory -Force -Path $CodexHome | Out-Null
    [IO.File]::WriteAllText($configPath,'',[Text.UTF8Encoding]::new($false))
}
$configSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash.ToLowerInvariant()
$repair = Invoke-SmJsonScript (Join-Path $PSScriptRoot 'Repair-SemanticMemory.ps1') @(
    '-Mode','Apply','-ConfigPath',$configPath,'-InstallRoot',$InstallRoot,
    '-ExpectedConfigSha256',$configSha,'-BackupRoot',(Join-Path $InstallRoot 'backups\codex-config')
)
if ([string]$repair.status -notin @('APPLIED_VERIFIED','REPLAYED_ZERO_WRITE')) { throw 'Managed MCP config repair did not verify.' }
$state.config_installed_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash.ToLowerInvariant()
$state | Add-Member -NotePropertyName last_repair_at -NotePropertyValue ([DateTimeOffset]::Now.ToString('o')) -Force
$state | Add-Member -NotePropertyName last_repair_status -NotePropertyValue ([string]$repair.status) -Force
[IO.File]::WriteAllText($statePath,(($state|ConvertTo-Json -Depth 10)+"`n"),[Text.UTF8Encoding]::new($false))
& powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Verify-Bundle.ps1') -UserHome $UserHome -InstallRoot $InstallRoot -CodexHome $CodexHome
if ($LASTEXITCODE -ne 0) { throw 'Repair verification failed.' }
Write-Host 'MCP configuration repair and automatic verification completed.' -ForegroundColor Green
Write-Host 'Open Codex Desktop, then run Verify After Restart.cmd from a new task or after fully closing Codex again.'

