[CmdletBinding()]
param(
    [string]$UserHome = $env:USERPROFILE,
    [string]$InstallRoot,
    [string]$CodexHome,
    [switch]$AllowRunningCodexForIsolatedTest
)

$ErrorActionPreference = 'Stop'
$version = [string](Get-Content -LiteralPath (Join-Path $PSScriptRoot 'RELEASE.json') -Encoding UTF8 -Raw | ConvertFrom-Json).version
. (Join-Path $PSScriptRoot 'Bundle.Common.ps1')
if (-not $UserHome) { throw 'Cannot determine the current user home.' }
if (-not $InstallRoot) { $InstallRoot = Join-Path $env:LOCALAPPDATA 'SemanticMemory' }
if (-not $CodexHome) { $CodexHome = Join-Path $UserHome '.codex' }
if ($InstallRoot -match '[^\x00-\x7F]') { throw 'NON_ASCII_RUNTIME_ROOT_UNSUPPORTED: this native core requires an ASCII InstallRoot. Use -InstallRoot with a writable ASCII path; no files changed.' }
$configPath = Join-Path $CodexHome 'config.toml'
$payloadManifest = Join-Path $PSScriptRoot 'payload\payload-manifest.json'
$pluginInstaller = Join-Path $PSScriptRoot 'Install-SemanticMemoryPlugin.ps1'
$runtimeInstaller = Join-Path $PSScriptRoot 'Install-SemanticMemoryV2.ps1'
$configRepair = Join-Path $PSScriptRoot 'Repair-SemanticMemory.ps1'
$stateRoot = Join-Path $InstallRoot ('backups\bundle-installer\' + [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmssfff') + '-' + [guid]::NewGuid().ToString('N'))
$pluginStateRoot = Join-Path $UserHome 'AppData\Local\SemanticMemory\backups\codex-plugin'
$statePath = Join-Path $InstallRoot 'install-bundle-state.json'
$managedMarkerPath = Join-Path $InstallRoot '.semantic-memory-managed.json'

function Invoke-JsonScript {
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
    if ($code -ne 0) { throw "Installer script failed: $Script (exit=$code): $text" }
    try { return ($text | ConvertFrom-Json -ErrorAction Stop) }
    catch { throw "Subprocess did not return valid JSON: $text" }
}

function Get-Expectation($state,$property) {
    if (-not [bool]$state.exists) { return 'ABSENT' }
    return [string]$state.$property
}

if (-not [Environment]::Is64BitOperatingSystem) { throw 'This package supports Windows x64 only.' }
if (-not (Test-Path -LiteralPath $payloadManifest -PathType Leaf)) { throw 'Package is incomplete: payload manifest is missing.' }
Assert-SmCodexDesktopStopped -UserHome $UserHome -AllowRunningCodexForIsolatedTest:$AllowRunningCodexForIsolatedTest
$previousBundleState = $null
if (Test-Path -LiteralPath $statePath -PathType Leaf) {
    $previousBundleState = Get-Content -LiteralPath $statePath -Encoding UTF8 -Raw | ConvertFrom-Json
    if ([string]$previousBundleState.schema -ne 'semantic-memory-transfer-install/v1') { throw 'Unsupported previous bundle state.' }
}
$previousPayloadManifest = $null
$previousPointerPath = Join-Path $InstallRoot 'state\current.json'
if (Test-Path -LiteralPath $previousPointerPath -PathType Leaf) {
    $priorPointer = Get-Content -LiteralPath $previousPointerPath -Encoding UTF8 -Raw | ConvertFrom-Json
    if ([string]$priorPointer.version_id -notmatch '^[A-Za-z0-9._+-]+$') { throw 'Invalid previous runtime version.' }
    $previousPayloadManifest = Join-Path $InstallRoot ('app\versions\' + [string]$priorPointer.version_id + '\payload-manifest.json')
}
$runtimeMode = 'Install'
$runtimeExistedBefore = $false
if (Test-Path -LiteralPath $InstallRoot) {
    if (Get-ChildItem -LiteralPath $InstallRoot -Force -ErrorAction SilentlyContinue | Select-Object -First 1) {
        if (-not (Test-Path -LiteralPath $managedMarkerPath -PathType Leaf)) {
            throw "The runtime target is not empty and has no managed marker. Refusing to overwrite unknown data: $InstallRoot"
        }
        try { $managedMarker = Get-Content -LiteralPath $managedMarkerPath -Raw -Encoding UTF8 | ConvertFrom-Json }
        catch { throw "The runtime target has an unreadable managed marker. Refusing to continue: $managedMarkerPath" }
        if ([string]$managedMarker.schema -ne 'stage14-install-marker/v2' -or
            [string]$managedMarker.product -ne 'semantic-memory') {
            throw "The runtime target has an unsupported managed marker. Refusing to continue: $managedMarkerPath"
        }
        $runtimeExistedBefore = $true
        $packageManifestSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $payloadManifest).Hash.ToLowerInvariant()
        $runtimeMode = $(if ([string]$managedMarker.current_manifest_sha256 -eq $packageManifestSha) { 'Verify' } else { 'Upgrade' })
    }
}

$pluginTransaction = $null
$runtimeInstalled = $false
$configExisted = Test-Path -LiteralPath $configPath -PathType Leaf
$configBackup = $null
$configInstalledSha = $null
try {
    New-Item -ItemType Directory -Force -Path $CodexHome | Out-Null
    if (-not $configExisted) {
        [IO.File]::WriteAllText($configPath, '', [Text.UTF8Encoding]::new($false))
    }
    $runtimeArguments = @('-Action',$runtimeMode,'-InstallRoot',$InstallRoot)
    if ($runtimeMode -in @('Install','Upgrade')) { $runtimeArguments += @('-PayloadManifestPath',$payloadManifest) }
    $runtime = Invoke-JsonScript $runtimeInstaller $runtimeArguments
    if ([string]$runtime.status -ne 'PASS') { throw 'Runtime installer did not return PASS.' }
    $runtimeInstalled = -not $runtimeExistedBefore

    New-Item -ItemType Directory -Force -Path $stateRoot | Out-Null
    if ($previousBundleState) { Copy-Item -LiteralPath $statePath -Destination (Join-Path $stateRoot 'install-bundle-state.before.json') }
    if ($configExisted) {
        $configBackup = Join-Path $stateRoot 'config.toml.before-install'
        [IO.File]::WriteAllBytes($configBackup, [IO.File]::ReadAllBytes($configPath))
    }

    $preview = Invoke-JsonScript $pluginInstaller @(
        '-Action','Preview','-PackageRoot',$PSScriptRoot,'-UserHome',$UserHome,
        '-CodexHome',$CodexHome,'-StateRoot',$pluginStateRoot
    )
    $plugin = Invoke-JsonScript $pluginInstaller @(
        '-Action','Apply','-PackageRoot',$PSScriptRoot,'-UserHome',$UserHome,
        '-CodexHome',$CodexHome,'-StateRoot',$pluginStateRoot,
        '-ExpectedMarketplaceSha256',(Get-Expectation $preview.states_before.marketplace 'sha256'),
        '-ExpectedSourceTreeSha256',(Get-Expectation $preview.states_before.source 'tree_sha256'),
        '-ExpectedCacheTreeSha256',(Get-Expectation $preview.states_before.cache 'tree_sha256'),
        '-ConfirmUserMutation'
    )
    $pluginTransaction = [string]$plugin.transaction_path

    $configBeforeSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash.ToLowerInvariant()
    $config = Invoke-JsonScript $configRepair @(
        '-Mode','Apply','-ConfigPath',$configPath,'-InstallRoot',$InstallRoot,
        '-ExpectedConfigSha256',$configBeforeSha,'-BackupRoot',(Join-Path $InstallRoot 'backups\codex-config')
    )
    if ([string]$config.status -notin @('APPLIED_VERIFIED','REPLAYED_ZERO_WRITE')) {
        throw 'Codex MCP configuration was not verified.'
    }
    $configInstalledSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash.ToLowerInvariant()

    $state = [ordered]@{
        schema = 'semantic-memory-transfer-install/v1'
        version = $version
        installed_at = [DateTimeOffset]::Now.ToString('o')
        user_home = [IO.Path]::GetFullPath($UserHome)
        codex_home = [IO.Path]::GetFullPath($CodexHome)
        install_root = [IO.Path]::GetFullPath($InstallRoot)
        runtime_mode = $runtimeMode
        runtime_existed_before = $runtimeExistedBefore
        previous_payload_manifest = $previousPayloadManifest
        previous_bundle_state_path = $(if ($previousBundleState) { Join-Path $stateRoot 'install-bundle-state.before.json' } else { $null })
        config_path = [IO.Path]::GetFullPath($configPath)
        config_existed_before = $configExisted
        config_backup_path = $configBackup
        config_installed_sha256 = $configInstalledSha
        plugin_state_root = [IO.Path]::GetFullPath($pluginStateRoot)
        plugin_transaction_path = $pluginTransaction
    }
    [IO.File]::WriteAllText($statePath, (($state | ConvertTo-Json -Depth 8) + "`n"), [Text.UTF8Encoding]::new($false))

    & powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Verify-Bundle.ps1') -UserHome $UserHome -InstallRoot $InstallRoot -CodexHome $CodexHome
    if ($LASTEXITCODE -ne 0) { throw 'Final verification failed.' }
    Write-Host "`nInstall and automatic verification completed: Semantic Memory $version" -ForegroundColor Green
    Write-Host 'Fully exit and reopen Codex Desktop, then create a new task for manual acceptance.'
} catch {
    $failure = $_.Exception.Message
    if ($pluginTransaction -and (Test-Path -LiteralPath $pluginTransaction)) {
        & powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $pluginInstaller -Action Rollback -PackageRoot $PSScriptRoot -UserHome $UserHome -CodexHome $CodexHome -StateRoot $pluginStateRoot -TransactionPath $pluginTransaction -ConfirmUserMutation | Out-Null
    }
    if ($configExisted -and $configBackup -and (Test-Path -LiteralPath $configBackup)) {
        [IO.File]::WriteAllBytes($configPath, [IO.File]::ReadAllBytes($configBackup))
    } elseif (-not $configExisted -and (Test-Path -LiteralPath $configPath)) {
        Remove-Item -LiteralPath $configPath -Force
    }
    if ($runtimeInstalled) {
        & powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $runtimeInstaller -Action Uninstall -InstallRoot $InstallRoot | Out-Null
    }
    if ($runtimeExistedBefore -and $runtimeMode -eq 'Upgrade' -and $previousPayloadManifest) {
        & powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $runtimeInstaller -Action Upgrade -InstallRoot $InstallRoot -PayloadManifestPath $previousPayloadManifest | Out-Null
        if ($LASTEXITCODE -ne 0) { $failure += '; prior runtime recovery failed; inspect retained payload and backups' }
    }
    if ($previousBundleState -and (Test-Path -LiteralPath (Join-Path $stateRoot 'install-bundle-state.before.json'))) {
        Copy-Item -LiteralPath (Join-Path $stateRoot 'install-bundle-state.before.json') -Destination $statePath -Force
    }
    throw "Install failed; rollback was attempted: $failure"
}
