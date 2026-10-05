$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$installer = Join-Path $root 'packaging\windows\Install-SemanticMemory.ps1'
$text = Get-Content -Raw -LiteralPath $installer
if ($text -notmatch 'data_retained_by_default') { throw 'missing data retention marker' }
if ($text -notmatch 'ConfirmDisposablePurge') { throw 'missing purge confirmation guard' }
if ($text -match 'New-Service|sc\.exe|Set-ItemProperty|New-ItemProperty|setx') {
    throw 'installer contains forbidden system mutation primitive'
}
$uninstallIndex = $text.IndexOf("if (`$Action -eq 'Uninstall')")
$purgeGuardIndex = $text.IndexOf('Assert-DisposablePurgeRoot -Root $InstallRoot', $uninstallIndex)
$removeMarkerIndex = $text.IndexOf('Remove-Item -LiteralPath $Marker', $uninstallIndex)
if ($uninstallIndex -lt 0 -or $purgeGuardIndex -lt 0 -or $removeMarkerIndex -lt 0 -or $purgeGuardIndex -gt $removeMarkerIndex) {
    throw 'purge confirmation must run before uninstall mutates managed files'
}
$packager = Get-Content -Raw -LiteralPath (Join-Path $root 'packaging\package_release.ps1')
if ($packager -notmatch 'Start-MpScan[^\r\n]*-ErrorAction\s+Stop') {
    throw 'Defender scan must treat non-terminating errors as failed evidence'
}
if ($packager -notmatch 'scan_failed_no_detection_claim') {
    throw 'failed Defender scans must not claim no detection'
}
$scripts = @(
    'packaging\windows\Install-SemanticMemory.ps1',
    'packaging\windows\Invoke-PackageAcceptance.ps1',
    'packaging\windows\Register-SemanticMemoryProject.ps1',
    'packaging\windows\Invoke-CodeSign.ps1',
    'packaging\signing\Invoke-VerifySignature.ps1',
    'packaging\package_release.ps1',
    'packaging\clean-machine\Invoke-CleanMachineAcceptance.ps1'
)
foreach ($relative in $scripts) {
    $path = Join-Path $root $relative
    $errors = $null
    [System.Management.Automation.PSParser]::Tokenize((Get-Content -Raw -LiteralPath $path), [ref]$errors) | Out-Null
    if ($errors.Count -ne 0) {
        throw "parse error in $relative"
    }
}
Write-Output 'PASS_STAGE13_INSTALLER_STATIC_TEST'
