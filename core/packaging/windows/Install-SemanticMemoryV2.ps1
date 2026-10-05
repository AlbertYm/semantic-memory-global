param(
    [ValidateSet('Install','Upgrade','Repair','Verify','Uninstall')][string]$Action = 'Install',
    [string]$InstallRoot,
    [string]$PayloadManifestPath,
    [ValidateSet('none','after_stage','before_switch','marker_write','launcher_copy')][string]$FaultInjection = 'none'
)

$ErrorActionPreference = 'Stop'
$moduleSource = Join-Path $PSScriptRoot 'SemanticMemory.Common.psm1'
Import-Module $moduleSource -Force -DisableNameChecking

if (-not $InstallRoot) { $InstallRoot = Get-SmDefaultInstallRoot }
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
$markerPath = Join-Path $InstallRoot '.semantic-memory-managed.json'
$stateDir = Join-Path $InstallRoot 'state'
$currentPath = Join-Path $stateDir 'current.json'
$binDir = Join-Path $InstallRoot 'bin'
$script:markerTemporaryPath = $null
$script:currentTemporaryPath = $null
$script:currentHistoryPath = $null
$script:currentHistoryDirectoryCreated = $false

function Assert-SmInstallPathHasNoReparsePoint {
    $candidate = $InstallRoot
    while (-not [string]::IsNullOrWhiteSpace($candidate)) {
        if (Test-Path -LiteralPath $candidate) {
            $item = Get-Item -Force -LiteralPath $candidate
            if ([bool]($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
                throw "INSTALL_ROOT_REPARSE_POINT: existing install path component is a reparse point: $candidate"
            }
        }
        $parent = Split-Path -Parent $candidate
        if ([string]::IsNullOrWhiteSpace($parent) -or
            [string]::Equals($parent, $candidate, [System.StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $candidate = $parent
    }
}

Assert-SmInstallPathHasNoReparsePoint

function Assert-SmManagedInstall {
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) {
        throw "Install root is not managed by Semantic Memory installer v2: $InstallRoot"
    }
    $marker = Read-SmJson -Path $markerPath
    if ($marker.schema -ne 'stage14-install-marker/v2') { throw 'Unsupported install marker schema.' }
    return $marker
}

function Assert-SmFreshInstallRoot {
    if (-not (Test-Path -LiteralPath $InstallRoot)) { return }
    if (-not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
        throw "INSTALL_ROOT_NOT_EMPTY_UNMANAGED: install root exists and is not a directory: $InstallRoot"
    }
    $rootItem = Get-Item -Force -LiteralPath $InstallRoot
    if ([bool]($rootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw "INSTALL_ROOT_REPARSE_POINT: fresh install root must not be a reparse point: $InstallRoot"
    }
    if (Get-ChildItem -Force -LiteralPath $InstallRoot | Select-Object -First 1) {
        throw "INSTALL_ROOT_NOT_EMPTY_UNMANAGED: Install accepts only a new or empty root: $InstallRoot"
    }
}

function Get-SmFileSnapshot {
    param([Parameter(Mandatory=$true)][string]$Path)
    $exists = Test-Path -LiteralPath $Path -PathType Leaf
    $bytes = $null
    if ($exists) { $bytes = [System.IO.File]::ReadAllBytes($Path) }
    return [pscustomobject]@{ path=$Path; existed=$exists; bytes=$bytes }
}

function Restore-SmFileSnapshot {
    param([Parameter(Mandatory=$true)]$Snapshot)
    $path = [string]$Snapshot.path
    if ([bool]$Snapshot.existed) {
        $parent = Split-Path -Parent $path
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $currentBytes = [System.IO.File]::ReadAllBytes($path)
            $snapshotBytes = [byte[]]$Snapshot.bytes
            $matches = $currentBytes.Length -eq $snapshotBytes.Length
            if ($matches) {
                for ($index = 0; $index -lt $currentBytes.Length; $index++) {
                    if ($currentBytes[$index] -ne $snapshotBytes[$index]) {
                        $matches = $false
                        break
                    }
                }
            }
            if ($matches) { return }
        }
        [System.IO.File]::WriteAllBytes($path, [byte[]]$Snapshot.bytes)
    } elseif (Test-Path -LiteralPath $path -PathType Leaf) {
        Remove-Item -LiteralPath $path -Force
    }
}

function Remove-SmEmptyDirectory {
    param([string]$Path)
    if ($Path -and (Test-Path -LiteralPath $Path -PathType Container) -and
        -not (Get-ChildItem -Force -LiteralPath $Path | Select-Object -First 1)) {
        Remove-Item -LiteralPath $Path -Force
    }
}

function Restore-SmInstallTransaction {
    param(
        [Parameter(Mandatory=$true)]$StableTransaction,
        [Parameter(Mandatory=$true)]$CurrentSnapshot,
        [Parameter(Mandatory=$true)]$MarkerSnapshot
    )
    Restore-SmStableNativeEntrypoints -Transaction $StableTransaction
    if ($script:currentHistoryPath -and (Test-Path -LiteralPath $script:currentHistoryPath -PathType Leaf)) {
        if (Test-Path -LiteralPath $currentPath -PathType Leaf) {
            Remove-Item -LiteralPath $currentPath -Force
        }
        Move-Item -LiteralPath $script:currentHistoryPath -Destination $currentPath
    } else {
        Restore-SmFileSnapshot -Snapshot $CurrentSnapshot
    }
    Restore-SmFileSnapshot -Snapshot $MarkerSnapshot
    foreach ($temporaryPath in @($script:markerTemporaryPath,$script:currentTemporaryPath)) {
        if ($temporaryPath -and (Test-Path -LiteralPath $temporaryPath -PathType Leaf)) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
    if ($script:currentHistoryDirectoryCreated -and $script:currentHistoryPath) {
        Remove-SmEmptyDirectory -Path (Split-Path -Parent $script:currentHistoryPath)
    }
    $transactionRoot = [string]$StableTransaction.transaction_root
    if (Test-Path -LiteralPath $transactionRoot -PathType Container) {
        Remove-Item -LiteralPath $transactionRoot -Recurse -Force
    }
    if ([bool]$StableTransaction.transaction_parent_created) {
        Remove-SmEmptyDirectory -Path (Split-Path -Parent $transactionRoot)
    }
    if ([bool]$StableTransaction.bin_directory_created) {
        Remove-SmEmptyDirectory -Path $binDir
    }
}

function New-SmHardLinkOrCopy {
    param(
        [Parameter(Mandatory=$true)][string]$Source,
        [Parameter(Mandatory=$true)][string]$Destination
    )
    try {
        New-Item -ItemType HardLink -Path $Destination -Target $Source -ErrorAction Stop | Out-Null
        return $true
    } catch {
        Copy-Item -LiteralPath $Source -Destination $Destination
        return $false
    }
}

function Install-SmStableNativeEntrypoints {
    param([Parameter(Mandatory=$true)]$Verification)
    $payloadRoot = Join-Path $InstallRoot ("app\versions\{0}" -f $Verification.version_id)
    $stableNames = [ordered]@{
        mcp = 'semantic-memory-mcp.exe'
        hook = 'semantic-memory-hook.exe'
        manager = 'semantic-memory-manager.exe'
    }
    $launcherName = 'semantic-memory-launcher.ps1'
    $launcherSource = Join-Path $PSScriptRoot $launcherName
    if (-not (Test-Path -LiteralPath $launcherSource -PathType Leaf)) {
        throw "RED_PAYLOAD_INTEGRITY_FAILURE: stable launcher package script is missing: $launcherSource"
    }
    $launcherSha256 = Get-SmSha256File -Path $launcherSource
    if ($FaultInjection -eq 'launcher_copy') { throw 'FAULT_INJECTION: launcher_copy' }
    $stableFileNames = @($stableNames.Values) + @($launcherName)
    $transactionId = "{0}-{1}" -f [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmssfff'),[guid]::NewGuid().ToString('N')
    $transactionParent = Join-Path $stateDir 'stable-transactions'
    $transactionParentCreated = -not (Test-Path -LiteralPath $transactionParent -PathType Container)
    $binDirectoryCreated = -not (Test-Path -LiteralPath $binDir -PathType Container)
    $transactionRoot = Join-Path $transactionParent $transactionId
    $newDir = Join-Path $transactionRoot 'new'
    $oldDir = Join-Path $transactionRoot 'old'
    New-Item -ItemType Directory -Force -Path $newDir,$oldDir,$binDir | Out-Null
    $allHardlinks = $true
    foreach ($role in $stableNames.Keys) {
        $entrypoint = $Verification.manifest.entrypoints.PSObject.Properties[$role]
        if (-not $entrypoint -or [string]::IsNullOrWhiteSpace([string]$entrypoint.Value)) {
            throw "RED_PAYLOAD_INTEGRITY_FAILURE: missing native entrypoint role: $role"
        }
        $source = Resolve-SmChildPath -Root $payloadRoot -RelativePath ([string]$entrypoint.Value)
        $prepared = Join-Path $newDir $stableNames[$role]
        if (-not (New-SmHardLinkOrCopy -Source $source -Destination $prepared)) { $allHardlinks = $false }
    }
    $preparedLauncher = Join-Path $newDir $launcherName
    Copy-Item -LiteralPath $launcherSource -Destination $preparedLauncher
    if ((Get-SmSha256File -Path $preparedLauncher) -ne $launcherSha256) {
        throw 'RED_PAYLOAD_INTEGRITY_FAILURE: stable launcher copy verification failed.'
    }
    $oldMoved = New-Object System.Collections.Generic.List[string]
    $newMoved = New-Object System.Collections.Generic.List[string]
    try {
        foreach ($name in $stableFileNames) {
            $destination = Join-Path $binDir $name
            if (Test-Path -LiteralPath $destination -PathType Leaf) {
                Move-Item -LiteralPath $destination -Destination (Join-Path $oldDir $name)
                $oldMoved.Add($name)
            }
        }
        foreach ($name in $stableFileNames) {
            Move-Item -LiteralPath (Join-Path $newDir $name) -Destination (Join-Path $binDir $name)
            $newMoved.Add($name)
        }
    } catch {
        foreach ($name in @($newMoved.ToArray()) | Select-Object -Last 100) {
            $installed = Join-Path $binDir $name
            if (Test-Path -LiteralPath $installed -PathType Leaf) {
                Move-Item -LiteralPath $installed -Destination (Join-Path $newDir $name)
            }
        }
        foreach ($name in $oldMoved) {
            $old = Join-Path $oldDir $name
            if (Test-Path -LiteralPath $old -PathType Leaf) {
                Move-Item -LiteralPath $old -Destination (Join-Path $binDir $name)
            }
        }
        throw
    }
    return [pscustomobject]@{
        transaction_id = $transactionId
        transaction_root = $transactionRoot
        prior_entrypoints_preserved = ($oldMoved.Count -gt 0)
        hardlink_all = $allHardlinks
        rollback_performed = $false
        transaction_parent_created = $transactionParentCreated
        bin_directory_created = $binDirectoryCreated
        stable_file_names = $stableFileNames
        launcher_sha256 = $launcherSha256
    }
}

function Restore-SmStableNativeEntrypoints {
    param([Parameter(Mandatory=$true)]$Transaction)
    $newDir = Join-Path ([string]$Transaction.transaction_root) 'new'
    $oldDir = Join-Path ([string]$Transaction.transaction_root) 'old'
    $stableFileNames = @($Transaction.stable_file_names)
    if ($stableFileNames.Count -eq 0) {
        $stableFileNames = @('semantic-memory-mcp.exe','semantic-memory-hook.exe','semantic-memory-manager.exe')
    }
    foreach ($name in $stableFileNames) {
        $active = Join-Path $binDir $name
        if (Test-Path -LiteralPath $active -PathType Leaf) {
            Move-Item -LiteralPath $active -Destination (Join-Path $newDir $name)
        }
    }
    foreach ($name in $stableFileNames) {
        $old = Join-Path $oldDir $name
        if (Test-Path -LiteralPath $old -PathType Leaf) {
            Move-Item -LiteralPath $old -Destination (Join-Path $binDir $name)
        }
    }
    $Transaction.rollback_performed = $true
}

function Write-SmCurrentPointer {
    param($Verification)
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
    if (Test-Path -LiteralPath $currentPath -PathType Leaf) {
        $historyDir = Join-Path $stateDir 'history'
        $script:currentHistoryDirectoryCreated = -not (Test-Path -LiteralPath $historyDir -PathType Container)
        New-Item -ItemType Directory -Force -Path $historyDir | Out-Null
        $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmssfff')
        $script:currentHistoryPath = Join-Path $historyDir ("current-before-$stamp.json")
        Move-Item -LiteralPath $currentPath -Destination $script:currentHistoryPath
    }
    $payloadRoot=Join-Path $InstallRoot ("app\versions\{0}"-f $Verification.version_id)
    $receiptPath=Join-Path $payloadRoot 'verification-receipt.json'
    $receipt=New-SmVerificationReceipt -Verification $Verification -PayloadRoot $payloadRoot
    Write-SmJson -Path $receiptPath -Value $receipt
    $pointer = [ordered]@{
        schema = 'stage14-current-pointer/v1'
        version_id = $Verification.version_id
        manifest_sha256 = $Verification.manifest_sha256
        receipt_sha256 = Get-SmSha256File -Path $receiptPath
        switched_at = [DateTime]::UtcNow.ToString('o')
    }
    $script:currentTemporaryPath = Join-Path $stateDir ("current.new-{0}.json" -f [guid]::NewGuid().ToString('N'))
    Write-SmJson -Path $script:currentTemporaryPath -Value $pointer
    Move-Item -LiteralPath $script:currentTemporaryPath -Destination $currentPath
}

function Stage-SmPayload {
    if (-not $PayloadManifestPath) { throw "$Action requires -PayloadManifestPath." }
    $manifestFull = [System.IO.Path]::GetFullPath($PayloadManifestPath)
    $payloadRoot = Split-Path -Parent $manifestFull
    $sourceVerification = Test-SmPayloadManifest -PayloadRoot $payloadRoot -ManifestPath $manifestFull
    if ($sourceVerification.status -ne 'PASS') { throw $sourceVerification.status }
    $versionId = $sourceVerification.version_id
    $versionsDir = Join-Path $InstallRoot 'app\versions'
    $destination = Join-Path $versionsDir $versionId
    if (Test-Path -LiteralPath $destination) {
        $existing = Test-SmPayloadManifest -PayloadRoot $destination -ManifestPath (Join-Path $destination 'payload-manifest.json')
        if ($existing.status -ne 'PASS' -or $existing.manifest_sha256 -ne $sourceVerification.manifest_sha256) {
            throw 'RED_PAYLOAD_INTEGRITY_FAILURE: immutable version directory differs from package.'
        }
        return $existing
    }
    New-Item -ItemType Directory -Force -Path $versionsDir | Out-Null
    $staging = Join-Path $versionsDir (".staging-{0}-{1}" -f $versionId,[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $staging | Out-Null
    $canonicalByContent = @{}
    foreach ($record in $sourceVerification.manifest.files) {
        $source = Resolve-SmChildPath -Root $payloadRoot -RelativePath ([string]$record.path)
        $target = Resolve-SmChildPath -Root $staging -RelativePath ([string]$record.path)
        $targetParent = Split-Path -Parent $target
        New-Item -ItemType Directory -Force -Path $targetParent | Out-Null
        $contentKey = "{0}:{1}" -f ([string]$record.sha256).ToLowerInvariant(),[long]$record.bytes
        if ($canonicalByContent.ContainsKey($contentKey)) {
            [void](New-SmHardLinkOrCopy -Source ([string]$canonicalByContent[$contentKey]) -Destination $target)
        } else {
            Copy-Item -LiteralPath $source -Destination $target
            $canonicalByContent[$contentKey] = $target
        }
    }
    Copy-Item -LiteralPath $manifestFull -Destination (Join-Path $staging 'payload-manifest.json')
    $stagedVerification = Test-SmPayloadManifest -PayloadRoot $staging -ManifestPath (Join-Path $staging 'payload-manifest.json')
    if ($stagedVerification.status -ne 'PASS') { throw $stagedVerification.status }
    Move-Item -LiteralPath $staging -Destination $destination
    return (Test-SmPayloadManifest -PayloadRoot $destination -ManifestPath (Join-Path $destination 'payload-manifest.json'))
}

function Write-SmInstallMarker {
    param($Verification)
    $entrypointHashes = [ordered]@{
        'semantic-memory-mcp.exe' = Get-SmSha256File -Path (Join-Path $binDir 'semantic-memory-mcp.exe')
        'semantic-memory-hook.exe' = Get-SmSha256File -Path (Join-Path $binDir 'semantic-memory-hook.exe')
        'semantic-memory-manager.exe' = Get-SmSha256File -Path (Join-Path $binDir 'semantic-memory-manager.exe')
        'semantic-memory-launcher.ps1' = Get-SmSha256File -Path (Join-Path $binDir 'semantic-memory-launcher.ps1')
    }
    $marker = [ordered]@{
        schema = 'stage14-install-marker/v2'
        product = 'semantic-memory'
        current_version_id = $Verification.version_id
        current_manifest_sha256 = $Verification.manifest_sha256
        stable_entrypoint_sha256 = $entrypointHashes
        stable_transaction_id = $(if ($stableInstallResult) { $stableInstallResult.transaction_id } else { $null })
        stable_hardlink_all = $(if ($stableInstallResult) { [bool]$stableInstallResult.hardlink_all } else { $false })
        data_retained_by_default = $true
        registry_written = $false
        path_modified = $false
        service_installed = $false
        autostart_installed = $false
        updated_at = [DateTime]::UtcNow.ToString('o')
    }
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
    $script:markerTemporaryPath = Join-Path $stateDir ("install-marker.new-{0}.json" -f [guid]::NewGuid().ToString('N'))
    Write-SmJson -Path $script:markerTemporaryPath -Value $marker
    if ($FaultInjection -eq 'marker_write') { throw 'FAULT_INJECTION: marker_write' }
    Move-Item -LiteralPath $script:markerTemporaryPath -Destination $markerPath -Force
}

$status = 'PASS'
$versionId = $null
$currentSwitched = $false
$stableInstallResult = $null

if ($Action -eq 'Install') {
    Assert-SmFreshInstallRoot
    $currentSnapshot = Get-SmFileSnapshot -Path $currentPath
    $markerSnapshot = Get-SmFileSnapshot -Path $markerPath
    $managedDirectories = @(
        (Join-Path $InstallRoot 'data'),
        (Join-Path $InstallRoot 'data\projects'),
        (Join-Path $InstallRoot 'data\artifacts'),
        (Join-Path $InstallRoot 'backups'),
        (Join-Path $InstallRoot 'logs'),
        $stateDir
    )
    New-Item -ItemType Directory -Force -Path $managedDirectories | Out-Null
    $verified = Stage-SmPayload
    $versionId = $verified.version_id
    if ($FaultInjection -eq 'after_stage') { throw 'FAULT_INJECTION: after_stage' }
    $stableInstallResult = Install-SmStableNativeEntrypoints -Verification $verified
    try {
        if ($FaultInjection -eq 'before_switch') { throw 'FAULT_INJECTION: before_switch' }
        Write-SmCurrentPointer -Verification $verified
        Write-SmInstallMarker -Verification $verified
    } catch {
        $originalFailure = $_
        try { Restore-SmInstallTransaction -StableTransaction $stableInstallResult -CurrentSnapshot $currentSnapshot -MarkerSnapshot $markerSnapshot }
        catch { throw "INSTALL_TRANSACTION_ROLLBACK_FAILED: $($_.Exception.Message); original: $($originalFailure.Exception.Message)" }
        throw $originalFailure
    }
    $currentSwitched = $true
}
elseif ($Action -eq 'Upgrade') {
    Assert-SmManagedInstall | Out-Null
    $before = Get-SmCurrentPayloadFast -InstallRoot $InstallRoot
    $currentSnapshot = Get-SmFileSnapshot -Path $currentPath
    $markerSnapshot = Get-SmFileSnapshot -Path $markerPath
    $verified = Stage-SmPayload
    $versionId = $verified.version_id
    if ($FaultInjection -eq 'after_stage') { throw 'FAULT_INJECTION: after_stage' }
    if ($before.pointer.version_id -eq $verified.version_id -and $before.verification.manifest_sha256 -eq $verified.manifest_sha256) {
        $status = 'REPLAYED'
    } else {
        $stableInstallResult = Install-SmStableNativeEntrypoints -Verification $verified
        try {
            if ($FaultInjection -eq 'before_switch') { throw 'FAULT_INJECTION: before_switch' }
            Write-SmCurrentPointer -Verification $verified
            Write-SmInstallMarker -Verification $verified
        } catch {
            $originalFailure = $_
            try { Restore-SmInstallTransaction -StableTransaction $stableInstallResult -CurrentSnapshot $currentSnapshot -MarkerSnapshot $markerSnapshot }
            catch { throw "INSTALL_TRANSACTION_ROLLBACK_FAILED: $($_.Exception.Message); original: $($originalFailure.Exception.Message)" }
            throw $originalFailure
        }
        $currentSwitched = $true
    }
}
elseif ($Action -eq 'Repair') {
    Assert-SmManagedInstall | Out-Null
    $current = Get-SmCurrentPayloadFast -InstallRoot $InstallRoot
    $currentSnapshot = Get-SmFileSnapshot -Path $currentPath
    $markerSnapshot = Get-SmFileSnapshot -Path $markerPath
    $versionId = $current.pointer.version_id
    $stableInstallResult = Install-SmStableNativeEntrypoints -Verification $current.verification
    try {
        Write-SmInstallMarker -Verification $current.verification
    } catch {
        $originalFailure = $_
        try { Restore-SmInstallTransaction -StableTransaction $stableInstallResult -CurrentSnapshot $currentSnapshot -MarkerSnapshot $markerSnapshot }
        catch { throw "INSTALL_TRANSACTION_ROLLBACK_FAILED: $($_.Exception.Message); original: $($originalFailure.Exception.Message)" }
        throw $originalFailure
    }
}
elseif ($Action -eq 'Verify') {
    $marker = Assert-SmManagedInstall
    $current = Get-SmCurrentPayloadFast -InstallRoot $InstallRoot
    $versionId = $current.pointer.version_id
    foreach ($name in @('semantic-memory-mcp.exe','semantic-memory-hook.exe','semantic-memory-manager.exe','semantic-memory-launcher.ps1')) {
        $path = Join-Path $binDir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing stable entrypoint: $name" }
        $expected = [string]$marker.stable_entrypoint_sha256.$name
        if (-not $expected -or (Get-SmSha256File -Path $path) -ne $expected) {
            throw "RED_PAYLOAD_INTEGRITY_FAILURE: stable entrypoint hash mismatch: $name"
        }
    }
}
elseif ($Action -eq 'Uninstall') {
    Assert-SmManagedInstall | Out-Null
    $current = Get-SmCurrentPayloadFast -InstallRoot $InstallRoot
    $versionId = $current.pointer.version_id
    $archive = Join-Path $InstallRoot ("uninstalled\{0}-{1}" -f [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmssfff'),[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $archive | Out-Null
    foreach ($name in @('app','bin','state')) {
        $path = Join-Path $InstallRoot $name
        if (Test-Path -LiteralPath $path) { Move-Item -LiteralPath $path -Destination (Join-Path $archive $name) }
    }
    Move-Item -LiteralPath $markerPath -Destination (Join-Path $archive '.semantic-memory-managed.json')
    Write-SmJson -Path (Join-Path $InstallRoot 'data\retained-after-uninstall.json') -Value ([ordered]@{
        schema='stage14-retained-data/v1'; retained=$true; uninstalled_version_id=$versionId; archive=$archive; created_at=[DateTime]::UtcNow.ToString('o')
    })
}

[ordered]@{
    schema = 'stage14-installer-result/v2'
    action = $Action
    status = $status
    install_root = $InstallRoot
    version_id = $versionId
    current_pointer_switched = $currentSwitched
    stable_hardlink_all = $(if ($stableInstallResult) { [bool]$stableInstallResult.hardlink_all } else { $null })
    prior_stable_entrypoints_preserved = $(if ($stableInstallResult) { [bool]$stableInstallResult.prior_entrypoints_preserved } else { $null })
    stable_rollback_performed = $(if ($stableInstallResult) { [bool]$stableInstallResult.rollback_performed } else { $null })
    data_dir_exists = (Test-Path -LiteralPath (Join-Path $InstallRoot 'data'))
    data_retained_by_default = $true
    registry_written = $false
    path_modified = $false
    service_installed = $false
    autostart_installed = $false
} | ConvertTo-Json -Depth 10
