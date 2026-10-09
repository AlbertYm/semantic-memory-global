[CmdletBinding()]
param([string]$InstallRoot,[switch]$AllowRunningCodexForIsolatedTest)
$ErrorActionPreference = 'Stop'
if (-not $InstallRoot) { $InstallRoot = Join-Path $env:LOCALAPPDATA 'SemanticMemory' }
if (Test-Path -LiteralPath (Join-Path $InstallRoot 'codex-memory-repair-state.json') -PathType Leaf) {
    throw 'PERSISTENT_REPAIR_ACTIVE: Roll back the persistent Codex memory repair before native uninstall. No uninstall changes made.'
}
$statePath = Join-Path $InstallRoot 'install-bundle-state.json'
if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { throw 'Bundle state was not found. Refusing to guess the uninstall scope.' }
$state = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
. (Join-Path $PSScriptRoot 'Bundle.Common.ps1')
Assert-SmCodexDesktopStopped -UserHome $state.user_home -AllowRunningCodexForIsolatedTest:$AllowRunningCodexForIsolatedTest
$configPath = [string]$state.config_path
if (Test-Path -LiteralPath $configPath -PathType Leaf) {
    $preflightSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash.ToLowerInvariant()
    if ($preflightSha -ne [string]$state.config_installed_sha256) { throw 'config.toml changed after install. No uninstall changes made.' }
}
$pluginInstaller = Join-Path $PSScriptRoot 'Install-SemanticMemoryPlugin.ps1'
if ($state.plugin_transaction_path -and (Test-Path -LiteralPath $state.plugin_transaction_path)) {
    & powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $pluginInstaller -Action Rollback -PackageRoot $PSScriptRoot -UserHome $state.user_home -CodexHome $state.codex_home -StateRoot $state.plugin_state_root -TransactionPath $state.plugin_transaction_path -ConfirmUserMutation
    if ($LASTEXITCODE -ne 0) { throw 'Plugin rollback failed. Uninstall stopped.' }
}
$configPath = [string]$state.config_path
if (Test-Path -LiteralPath $configPath -PathType Leaf) {
    $currentSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash.ToLowerInvariant()
    if ($currentSha -ne [string]$state.config_installed_sha256) {
        throw 'config.toml changed after install. Uninstall stopped to preserve newer configuration; see README.'
    }
    if ([bool]$state.config_existed_before) {
        [IO.File]::WriteAllBytes($configPath, [IO.File]::ReadAllBytes([string]$state.config_backup_path))
    } else {
        Remove-Item -LiteralPath $configPath -Force
    }
}
if (-not [bool]$state.runtime_existed_before) {
    & powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Install-SemanticMemoryV2.ps1') -Action Uninstall -InstallRoot $InstallRoot
    if ($LASTEXITCODE -ne 0) { throw 'Runtime uninstall failed.' }
    Write-Host 'Uninstall completed. Memory data remains under SemanticMemory\data.' -ForegroundColor Green
} else {
    if ($state.previous_payload_manifest -and (Test-Path -LiteralPath $state.previous_payload_manifest)) {
        & powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Install-SemanticMemoryV2.ps1') -Action Upgrade -InstallRoot $InstallRoot -PayloadManifestPath $state.previous_payload_manifest
        if ($LASTEXITCODE -ne 0) { throw 'Prior runtime recovery failed.' }
    }
    if ($state.previous_bundle_state_path -and (Test-Path -LiteralPath $state.previous_bundle_state_path)) {
        Copy-Item -LiteralPath $state.previous_bundle_state_path -Destination $statePath -Force
    } else { Remove-Item -LiteralPath $statePath -Force }
    Write-Host 'Bundle changes were rolled back. The pre-existing managed runtime and data were preserved.' -ForegroundColor Green
}
