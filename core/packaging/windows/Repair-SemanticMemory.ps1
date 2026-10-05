param(
    [string]$ReferenceConfigPath,
    [string]$CandidateConfigPath,
    [string]$ConfigPath,
    [string]$InstallRoot,
    [string]$ExpectedConfigSha256,
    [string]$BackupRoot,
    [string]$TransactionPath,
    [switch]$EnableProductionCanary,
    [string]$CanaryAuthManifestPath,
    [string]$CanaryAuthSha256,
    [ValidateSet('Preview','Apply','Verify','Recover')][string]$Mode = 'Preview',
    [ValidateSet('none','after_backup','after_temp','after_cas_delay','after_cas_and_restore_delay','after_swap_delay','after_replace','before_verify')]
    [string]$FaultInjection = 'none'
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'SemanticMemory.Common.psm1') -Force -DisableNameChecking

function Get-SafeRepairErrorCode {
    param([string]$Message)
    $prefix = $Message.Split(':')[0].Trim()
    if ($prefix -match '^(CONFIG|RED|FAULT_INJECTION)_[A-Z0-9_]+$') {
        return $prefix
    }
    return 'CONFIG_REPAIR_FAILED'
}

try {
    $legacyRequested = $PSBoundParameters.ContainsKey('ReferenceConfigPath') -or
        $PSBoundParameters.ContainsKey('CandidateConfigPath')
    if ($legacyRequested) {
        if (-not $ReferenceConfigPath -or -not $CandidateConfigPath -or $Mode -ne 'Preview') {
            throw 'CONFIG_LEGACY_PREVIEW_ARGUMENTS_INVALID'
        }
        $comparison = Compare-SmManagedConfig `
            -ReferenceConfigPath $ReferenceConfigPath `
            -CandidateConfigPath $CandidateConfigPath
        $operations = @()
        foreach ($field in $comparison.changed_managed_fields) {
            $operations += [ordered]@{ operation='restore_managed_field'; field=$field }
        }
        foreach ($field in $comparison.missing_managed_fields) {
            if (-not ($operations | Where-Object { $_.field -eq $field })) {
                $operations += [ordered]@{ operation='restore_managed_field'; field=$field }
            }
        }
        [ordered]@{
            schema = 'stage14-repair-preview/v1'
            mode = $Mode
            classification = $comparison.classification
            managed_fingerprint_before = $comparison.managed_fingerprint_before
            managed_fingerprint_after = $comparison.managed_fingerprint_after
            whole_config_sha256_before = $comparison.whole_config_sha256_before
            whole_config_sha256_after = $comparison.whole_config_sha256_after
            operations = $operations
            unmanaged_fields_preserved = $true
            raw_credential_values_recorded = $false
            apply_performed = $false
        } | ConvertTo-Json -Depth 20
        return
    }

    if (-not $ConfigPath -or -not $InstallRoot) {
        throw 'CONFIG_REPAIR_TARGET_ARGUMENTS_REQUIRED'
    }
    switch ($Mode) {
        'Preview' {
            $result = Get-SmManagedConfigRepairPreview `
                -ConfigPath $ConfigPath `
                -InstallRoot $InstallRoot `
                -EnableProductionCanary:$EnableProductionCanary `
                -CanaryAuthManifestPath $CanaryAuthManifestPath `
                -CanaryAuthSha256 $CanaryAuthSha256
        }
        'Apply' {
            $result = Invoke-SmManagedConfigRepair `
                -ConfigPath $ConfigPath `
                -InstallRoot $InstallRoot `
                -ExpectedConfigSha256 $ExpectedConfigSha256 `
                -BackupRoot $BackupRoot `
                -EnableProductionCanary:$EnableProductionCanary `
                -CanaryAuthManifestPath $CanaryAuthManifestPath `
                -CanaryAuthSha256 $CanaryAuthSha256 `
                -FaultInjection $FaultInjection
        }
        'Verify' {
            $result = Test-SmManagedConfigRepair `
                -ConfigPath $ConfigPath `
                -InstallRoot $InstallRoot `
                -EnableProductionCanary:$EnableProductionCanary `
                -CanaryAuthManifestPath $CanaryAuthManifestPath `
                -CanaryAuthSha256 $CanaryAuthSha256
        }
        'Recover' {
            if (-not $TransactionPath) { throw 'CONFIG_RECOVERY_TRANSACTION_PATH_REQUIRED' }
            $result = Recover-SmManagedConfigTransaction `
                -TransactionPath $TransactionPath `
                -ConfigPath $ConfigPath `
                -InstallRoot $InstallRoot `
                -BackupRoot $BackupRoot
        }
    }
    $result | ConvertTo-Json -Depth 20
    $successful = $true
    if ($Mode -eq 'Apply') {
        $successful = [string]$result.status -in @('APPLIED_VERIFIED','REPLAYED_ZERO_WRITE')
    } elseif ($Mode -eq 'Verify') {
        $successful = [string]$result.status -ceq 'PASS'
    } elseif ($Mode -eq 'Recover') {
        $successful = [string]$result.status -in @(
            'RECOVERED_COMMIT_VERIFIED',
            'RECOVERED_ROLLBACK_VERIFIED'
        )
    }
    if (-not $successful) { exit 1 }
} catch {
    [ordered]@{
        schema = 'stage14-config-repair-error/v2'
        mode = $Mode
        status = 'FAILED_CLOSED'
        error_code = Get-SafeRepairErrorCode -Message ([string]$_.Exception.Message)
        raw_credential_values_recorded = $false
        write_performed = $false
    } | ConvertTo-Json -Depth 10
    exit 1
}
