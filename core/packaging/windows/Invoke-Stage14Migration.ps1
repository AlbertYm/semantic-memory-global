param(
    [ValidateSet('Plan','Apply','Verify','VerifyManaged','ForwardRecover')][string]$Action = 'Plan',
    [string]$SourceMemory,
    [string]$SourceGraph,
    [string]$SourceConfig,
    [string]$TargetRoot,
    [string]$ProjectPath,
    [string]$IdempotencyKey,
    [string]$MigrationExecutable,
    [string[]]$MigrationArgs = @(),
    [string]$ReportPath,
    [string]$PriorReportPath,
    [string]$ForwardJournalPath,
    [switch]$AllowExistingIsolatedTarget,
    [switch]$AllowExistingManagedTarget,
    [switch]$ProductionAuthorized,
    [ValidateSet('none','after_backup','before_core','after_core')][string]$FaultInjection = 'none',
    [Parameter(Mandatory=$true)][string]$DisposableRoot,
    [Parameter(Mandatory=$true)][string]$AuthorizationManifestPath,
    [Parameter(Mandatory=$true)][string]$AuthorizationManifestSha256,
    [Parameter(Mandatory=$true)][string]$AuthorizationNonce
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'SemanticMemory.Common.psm1') -Force -DisableNameChecking

function Get-SmCanonicalDirectoryPrefix {
    param([Parameter(Mandatory=$true)][string]$Path)
    return ([System.IO.Path]::GetFullPath($Path).TrimEnd('\') + '\')
}

function Assert-SmPathComponentsHaveNoReparsePoint {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Scope
    )
    $candidate = [System.IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrWhiteSpace($candidate)) {
        $item = Get-Item -Force -LiteralPath $candidate -ErrorAction SilentlyContinue
        if ($null -ne $item -and
            [bool]($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
            throw "PRODUCTION_TARGET_REPARSE_POINT: $Scope contains a reparse point: $candidate"
        }
        $parent = Split-Path -Parent $candidate
        if ([string]::IsNullOrWhiteSpace($parent) -or
            [string]::Equals($parent,$candidate,[System.StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $candidate = $parent
    }
}

function Assert-SmProductionTargetPathsHaveNoReparsePoint {
    param([Parameter(Mandatory=$true)][string]$TargetRoot)
    $installRoot = Split-Path -Parent $TargetRoot
    $projectsPath = Join-Path $TargetRoot 'projects'
    $fixedPaths = @(
        [ordered]@{ path=$installRoot; scope='SemanticMemory install root' },
        [ordered]@{ path=$TargetRoot; scope='managed data root' },
        [ordered]@{ path=(Join-Path $installRoot '.semantic-memory-managed.json'); scope='install marker' },
        [ordered]@{ path=(Join-Path $installRoot 'state\current.json'); scope='current pointer' },
        [ordered]@{ path=$projectsPath; scope='projects root' },
        [ordered]@{ path=(Join-Path $TargetRoot 'artifacts'); scope='artifacts root' },
        [ordered]@{ path=(Join-Path $TargetRoot '_config.db'); scope='managed config database' },
        [ordered]@{ path=(Join-Path $TargetRoot '__global__-memory.db'); scope='managed memory database' },
        [ordered]@{ path=(Join-Path $TargetRoot '__global__-graph.db'); scope='managed global graph database' }
    )
    foreach ($entry in $fixedPaths) {
        Assert-SmPathComponentsHaveNoReparsePoint -Path $entry.path -Scope $entry.scope
    }
    if (Test-Path -LiteralPath $projectsPath -PathType Container) {
        foreach ($projectEntry in @(Get-ChildItem -Force -LiteralPath $projectsPath)) {
            Assert-SmPathComponentsHaveNoReparsePoint -Path $projectEntry.FullName -Scope 'managed project shard'
            if ($projectEntry.PSIsContainer) {
                Assert-SmPathComponentsHaveNoReparsePoint -Path (Join-Path $projectEntry.FullName 'graph.db') -Scope 'managed project graph database'
            }
        }
    }
}

function Assert-SmContainedPath {
    param([Parameter(Mandatory=$true)][string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($script:DisposablePrefix,[System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the authorized disposable root: $full"
    }
    return $full
}

function Assert-SmHealthyReportChecks {
    param($Checks,[string]$Scope)
    if($Checks.quick_check -ne 'ok' -or [int]$Checks.foreign_key_violations -ne 0){throw "Migration $Scope health failed."}
    foreach($name in @('schema_sha256','canonical_logical_sha256')){if(([string]$Checks.$name)-notmatch '^[0-9a-f]{64}$'){throw "Migration $Scope $name is invalid."}}
    if($null -eq $Checks.row_counts){throw "Migration $Scope row_counts missing."}
}

function Test-SmInstallerManagedRootBinding {
    param([Parameter(Mandatory=$true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return $false }
    $dataItem = Get-Item -Force -LiteralPath $Path
    if ([bool]($dataItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) { return $false }

    $installRoot = Split-Path -Parent $Path
    $markerPath = Join-Path $installRoot '.semantic-memory-managed.json'
    $pointerPath = Join-Path $installRoot 'state\current.json'
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $pointerPath -PathType Leaf)) { return $false }
    try {
        $installMarker = Read-SmJson -Path $markerPath
        $currentPointer = Read-SmJson -Path $pointerPath
    } catch {
        return $false
    }
    if ($installMarker.schema -ne 'stage14-install-marker/v2' -or
        $installMarker.product -ne 'semantic-memory' -or
        $currentPointer.schema -ne 'stage14-current-pointer/v1' -or
        $installMarker.current_version_id -ne $currentPointer.version_id -or
        $installMarker.current_manifest_sha256 -ne $currentPointer.manifest_sha256) { return $false }
    return $true
}

function Test-SmExactEmptyInstallerManagedDataRoot {
    param([Parameter(Mandatory=$true)][string]$Path)
    if (-not (Test-SmInstallerManagedRootBinding -Path $Path)) { return $false }
    $children = @(Get-ChildItem -Force -LiteralPath $Path)
    if ($children.Count -ne 2) { return $false }
    $expected = @('artifacts','projects')
    $actual = @($children | ForEach-Object { $_.Name } | Sort-Object)
    if ([string]::Join('|',$actual) -ne [string]::Join('|',$expected)) { return $false }
    foreach ($child in $children) {
        if (-not $child.PSIsContainer -or
            [bool]($child.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -or
            (Get-ChildItem -Force -LiteralPath $child.FullName | Select-Object -First 1)) { return $false }
    }
    return $true
}

$DisposableRoot=[IO.Path]::GetFullPath($DisposableRoot)
$script:DisposablePrefix=Get-SmCanonicalDirectoryPrefix -Path $DisposableRoot
$manifestFull=Assert-SmContainedPath -Path $AuthorizationManifestPath
if((Get-SmSha256File $manifestFull)-ne $AuthorizationManifestSha256.ToLowerInvariant()){throw 'Authorization manifest SHA256 mismatch.'}
$authorization=Read-SmJson $manifestFull
$expectedAuthorizationSchema=if($ProductionAuthorized){'stage14-windows-production-migration-authorization/v2'}else{'stage14-windows-migration-authorization/v2'}
if($authorization.schema-ne$expectedAuthorizationSchema-or$authorization.nonce-ne$AuthorizationNonce){throw 'Migration authorization schema or nonce mismatch.'}
$markerPath=Join-Path $DisposableRoot '.stage14-disposable-marker.json';$marker=Read-SmJson $markerPath
if($marker.schema-ne'stage14-disposable-marker/v1'-or$marker.nonce-ne$AuthorizationNonce-or$marker.authorization_manifest_sha256-ne$AuthorizationManifestSha256.ToLowerInvariant()){throw 'Disposable marker binding mismatch.'}
if([DateTimeOffset]::Parse($authorization.expires_at)-le[DateTimeOffset]::UtcNow){throw 'Migration authorization expired.'}
if($ProductionAuthorized-and$AllowExistingIsolatedTarget){throw 'Production migration cannot use the isolated-target replay switch.'}
if(-not$ProductionAuthorized-and$AllowExistingManagedTarget){throw 'Isolated migration cannot use the managed-target replay switch.'}
if($ProductionAuthorized-and$Action-eq'ForwardRecover'){throw 'Production ForwardRecover is not enabled by this wrapper.'}

$localAppDataRoot=$null;$managedDataRoot=$null;$currentPointerPath=$null;$currentPointerExistsBefore=$false;$currentPointerShaBefore=$null
if($ProductionAuthorized){
    if([IO.Path]::GetFullPath([string]$authorization.disposable_root)-ne$DisposableRoot){throw 'DisposableRoot is not exactly bound by the production authorization.'}
    $localAppDataRoot=if($env:LOCALAPPDATA){[IO.Path]::GetFullPath($env:LOCALAPPDATA)}else{[IO.Path]::GetFullPath([Environment]::GetFolderPath('LocalApplicationData'))}
    $managedDataRoot=[IO.Path]::GetFullPath((Join-Path $localAppDataRoot 'SemanticMemory\data')).TrimEnd('\')
    $currentPointerPath=Join-Path $localAppDataRoot 'SemanticMemory\state\current.json'
    $currentPointerExistsBefore=Test-Path -LiteralPath $currentPointerPath -PathType Leaf
    if($currentPointerExistsBefore){$currentPointerShaBefore=Get-SmSha256File -Path $currentPointerPath}
}

function Assert-SmMigrationReport {
    param([Parameter(Mandatory=$true)][string]$Path)
    $report = Read-SmJson -Path $Path
    if ($report.schema -ne 'semantic-memory-global-migration/v1') { throw 'Unexpected migration report schema.' }
    if ($report.status -notin @('planned','applied','replayed','IDEMPOTENCY_CONFLICT','failed')) { throw 'Unexpected migration report status.' }
    foreach ($field in @('project_uuid','source','backup','target','source_to_target_projection','source_to_target_mapping','legacy_alias_counts','payload_sha256','current_pointer_switched','preserved_data_equivalent')) {
        if (-not ($report.PSObject.Properties.Name -contains $field)) { throw "Migration report missing field: $field" }
    }
    if ($report.current_pointer_switched -ne $false) { throw 'Migration engine must not switch current pointer.' }
    foreach ($scope in @('source','target')) {
        foreach ($field in @('quick_check','foreign_key_violations','schema_sha256','row_counts','canonical_logical_sha256')) {
            if (-not ($report.$scope.PSObject.Properties.Name -contains $field)) { throw "Migration report missing $scope.$field" }
        }
    }
    return $report
}

function Assert-SmManagedTargetVerifyReport {
    param([Parameter(Mandatory=$true)][string]$Path)
    $report = Read-SmJson -Path $Path
    if ($report.schema -ne 'semantic-memory-global-managed-target-verify/v1') { throw 'Unexpected managed-target verify report schema.' }
    if ($report.status -notin @('verified','source_drift','target_drift','target_not_quiescent','target_missing','target_unhealthy','ledger_missing','ledger_not_applied','ledger_unreadable','source_missing','source_unhealthy','failed')) { throw 'Unexpected managed-target verify status.' }
    foreach ($field in @('project_uuid','source_exists','target_exists','sidecars_absent','source','target','global_graph','ledger','comparisons','payload_sha256','database_write_performed','current_pointer_switched')) {
        if (-not ($report.PSObject.Properties.Name -contains $field)) { throw "Managed-target verify report missing field: $field" }
    }
    if ($report.database_write_performed -ne $false -or $report.current_pointer_switched -ne $false) { throw 'Managed-target verify performed a forbidden state change.' }
    foreach ($scope in @('source','target','global_graph')) {
        foreach ($field in @('quick_check','foreign_key_violations','schema_sha256','row_counts','canonical_logical_sha256')) {
            if (-not ($report.$scope.PSObject.Properties.Name -contains $field)) { throw "Managed-target verify report missing $scope.$field" }
        }
    }
    foreach ($field in @('found','row_count','state','payload_sha256','source_logical_sha256','target_logical_sha256')) {
        if (-not ($report.ledger.PSObject.Properties.Name -contains $field)) { throw "Managed-target verify report missing ledger.$field" }
    }
    foreach ($field in @('source_payload_match','source_logical_match','target_logical_match')) {
        if (-not ($report.comparisons.PSObject.Properties.Name -contains $field)) { throw "Managed-target verify report missing comparisons.$field" }
    }
    return $report
}

if ($Action -eq 'Verify') {
    if (-not $ReportPath) { throw 'Verify requires -ReportPath.' }
    $verifyReportFull=if($ProductionAuthorized){[IO.Path]::GetFullPath($ReportPath)}else{Assert-SmContainedPath -Path $ReportPath}
    if($ProductionAuthorized-and$verifyReportFull-ne[IO.Path]::GetFullPath([string]$authorization.report_path)){throw 'ReportPath is not exactly bound by the production authorization.'}
    $report = Assert-SmMigrationReport -Path $verifyReportFull
    $report | ConvertTo-Json -Depth 20
    exit 0
}

if ($Action -eq 'ForwardRecover') {
    if (-not $TargetRoot -or -not $PriorReportPath -or -not $ForwardJournalPath -or -not $ReportPath) {
        throw 'ForwardRecover requires TargetRoot, PriorReportPath, ForwardJournalPath, and a new ReportPath.'
    }
    $target = Assert-SmContainedPath -Path $TargetRoot
    $priorReportFull = Assert-SmContainedPath -Path $PriorReportPath
    $forwardJournalFull = Assert-SmContainedPath -Path $ForwardJournalPath
    $reportFull = Assert-SmContainedPath -Path $ReportPath
    if (-not (Test-Path -LiteralPath $target -PathType Container)) { throw 'Forward recovery target must already exist.' }
    if (Test-Path -LiteralPath $reportFull) { throw 'Forward recovery report path must be new.' }
    $prior = Assert-SmMigrationReport -Path $priorReportFull
    if (-not (Test-Path -LiteralPath $forwardJournalFull -PathType Leaf)) { throw 'Missing forward recovery journal.' }
    $lines=@([IO.File]::ReadAllLines($forwardJournalFull,[Text.UTF8Encoding]::new($false)));if($lines.Count-eq 0){throw 'Forward journal is empty.'}
    $seen=@{};foreach($line in $lines){$entry=$line|ConvertFrom-Json;if(-not $entry.task_id-or([string]$entry.payload_sha256)-notmatch'^[0-9a-f]{64}$'-or$seen.ContainsKey([string]$entry.task_id)){throw 'Forward journal schema, hash, or task continuity failed.'};$seen[[string]$entry.task_id]=$true}
    $payload = [ordered]@{
        schema = 'stage14-forward-recovery-manifest/v1'
        status = 'READY_FOR_CORE_FORWARD_REPLAY'
        target_root = $target
        project_uuid = $prior.project_uuid
        prior_report_sha256 = Get-SmSha256File -Path $priorReportFull
        forward_journal_sha256 = Get-SmSha256File -Path $forwardJournalFull
        strategy = 'fresh-backup-plus-forward-replay'
        production_restore_performed = $false
        current_pointer_switched = $false
        created_at = [DateTime]::UtcNow.ToString('o')
    }
    Write-SmJson -Path $reportFull -Value $payload
    $payload | ConvertTo-Json -Depth 10
    exit 0
}

foreach ($required in @('SourceMemory','SourceGraph','SourceConfig','TargetRoot','ProjectPath','IdempotencyKey','MigrationExecutable','ReportPath')) {
    if (-not (Get-Variable -Name $required -ValueOnly)) { throw "$Action requires -$required." }
}
$sourceMemoryFull=if($ProductionAuthorized){[IO.Path]::GetFullPath($SourceMemory)}else{Assert-SmContainedPath -Path $SourceMemory}
$sourceGraphFull=if($ProductionAuthorized){[IO.Path]::GetFullPath($SourceGraph)}else{Assert-SmContainedPath -Path $SourceGraph}
$sourceConfigFull=if($ProductionAuthorized){[IO.Path]::GetFullPath($SourceConfig)}else{Assert-SmContainedPath -Path $SourceConfig}
$targetFull=if($ProductionAuthorized){[IO.Path]::GetFullPath($TargetRoot)}else{Assert-SmContainedPath -Path $TargetRoot}
$reportFull=if($ProductionAuthorized){[IO.Path]::GetFullPath($ReportPath)}else{Assert-SmContainedPath -Path $ReportPath}
$projectPathFull=[IO.Path]::GetFullPath($ProjectPath)
if($projectPathFull-ne[IO.Path]::GetFullPath([string]$authorization.project_path)){throw 'ProjectPath is not exactly bound by the main-thread manifest.'}
if($ProductionAuthorized){
    if($sourceMemoryFull-ne[IO.Path]::GetFullPath([string]$authorization.source_memory_path)-or$sourceGraphFull-ne[IO.Path]::GetFullPath([string]$authorization.source_graph_path)-or$sourceConfigFull-ne[IO.Path]::GetFullPath([string]$authorization.source_config_path)){throw 'Production source paths are not exactly bound by the authorization.'}
    if($targetFull-ne[IO.Path]::GetFullPath([string]$authorization.target_root)){throw 'TargetRoot is not exactly bound by the production authorization.'}
    if($reportFull-ne[IO.Path]::GetFullPath([string]$authorization.report_path)){throw 'ReportPath is not exactly bound by the production authorization.'}
    if(-not$targetFull.Equals($managedDataRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Production target must be the exact authorized Semantic Memory data root.'}
    Assert-SmProductionTargetPathsHaveNoReparsePoint -TargetRoot $targetFull
}
if (-not (Test-Path -LiteralPath $sourceMemoryFull -PathType Leaf) -or -not (Test-Path -LiteralPath $sourceGraphFull -PathType Leaf) -or -not (Test-Path -LiteralPath $sourceConfigFull -PathType Leaf)) {
    throw 'Source databases are missing.'
}
if((Get-SmSha256File $sourceMemoryFull)-ne$authorization.source_memory_sha256-or(Get-SmSha256File $sourceGraphFull)-ne$authorization.source_graph_sha256-or(Get-SmSha256File $sourceConfigFull)-ne$authorization.source_config_sha256){throw 'Authorized source backup hash mismatch.'}
$latestWrite=[DateTimeOffset](Get-Item $sourceMemoryFull).LastWriteTimeUtc
foreach($sourcePath in @($sourceGraphFull,$sourceConfigFull)){if(([DateTimeOffset](Get-Item $sourcePath).LastWriteTimeUtc)-gt$latestWrite){$latestWrite=[DateTimeOffset](Get-Item $sourcePath).LastWriteTimeUtc}}
if(([DateTimeOffset]::UtcNow-$latestWrite).TotalMinutes-gt[double]$authorization.max_backup_age_minutes){throw 'Authorized source backup is stale.'}
$migrationExeFull=[IO.Path]::GetFullPath($MigrationExecutable)
if($ProductionAuthorized){Assert-SmPathComponentsHaveNoReparsePoint -Path $migrationExeFull -Scope 'authorized migration executable'}
if((Get-SmSha256File $migrationExeFull)-ne$authorization.migration_executable_sha256){throw 'Migration executable SHA256 mismatch.'}
if($ProductionAuthorized-and$migrationExeFull-ne[IO.Path]::GetFullPath([string]$authorization.migration_executable_path)){throw 'Migration executable path is not exactly bound by the production authorization.'}
$required=[long](Get-Item $sourceMemoryFull).Length*3+[long](Get-Item $sourceGraphFull).Length*3+[long](Get-Item $sourceConfigFull).Length*3+[long]$authorization.minimum_free_bytes
$backupDrive=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($DisposableRoot));if($backupDrive.AvailableFreeSpace-lt$required){throw 'Insufficient disk space for migration backup evidence.'}
if($ProductionAuthorized){$targetDrive=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($targetFull));if($targetDrive.AvailableFreeSpace-lt$required){throw 'Insufficient disk space for production migration target.'}}
if (Test-Path -LiteralPath $reportFull) { throw 'Migration report path must be new.' }
if($Action-eq'Apply'-and(Test-Path -LiteralPath $targetFull)){
    if($ProductionAuthorized){
        if(-not$AllowExistingManagedTarget){throw 'Production apply target must be new unless managed exact replay is explicit.'}
        $managedFilesPresent=$true
        foreach($managedFile in @('_config.db','__global__-memory.db','__global__-graph.db')){if(-not(Test-Path -LiteralPath (Join-Path $targetFull $managedFile) -PathType Leaf)){$managedFilesPresent=$false}}
        $managedReplayTarget=(Test-SmInstallerManagedRootBinding -Path $targetFull) -and $managedFilesPresent -and (Test-Path -LiteralPath (Join-Path $targetFull 'projects') -PathType Container)
        $emptyInstallerTarget=Test-SmExactEmptyInstallerManagedDataRoot -Path $targetFull
        if (-not $managedReplayTarget -and -not $emptyInstallerTarget) { throw 'Existing production target is neither an exact empty installer-managed data root nor a managed migration replay target.' }
    }elseif(-not$AllowExistingIsolatedTarget){throw 'Apply target must be a new isolated root unless exact replay is explicit.'}
}

$sourceMemoryShaBefore = Get-SmSha256File -Path $sourceMemoryFull
$sourceGraphShaBefore = Get-SmSha256File -Path $sourceGraphFull
$sourceConfigShaBefore = Get-SmSha256File -Path $sourceConfigFull
$coreSourceMemory = $sourceMemoryFull
$coreSourceGraph = $sourceGraphFull
$coreSourceConfig = $sourceConfigFull
$backupSourceKind = 'hash-bound isolated source fixtures'
if ($ProductionAuthorized) {
    $backupSourceKind = 'hash-bound fresh SQLite online backups supplied by the authorization manifest'
}
$backupEvidence = [ordered]@{
    schema = 'stage14-authorized-source-backups/v2'
    source = $backupSourceKind
    memory = [ordered]@{ path=$sourceMemoryFull; sha256=$sourceMemoryShaBefore }
    graph = [ordered]@{ path=$sourceGraphFull; sha256=$sourceGraphShaBefore }
    config = [ordered]@{ path=$sourceConfigFull; sha256=$sourceConfigShaBefore }
}

if ($Action -eq 'Apply') {
    if ($FaultInjection -eq 'after_backup') { throw 'FAULT_INJECTION: after_backup' }
}

if ($FaultInjection -eq 'before_core') { throw 'FAULT_INJECTION: before_core' }
$mode = if($Action -eq 'VerifyManaged'){'verify'}else{$Action.ToLowerInvariant()}
$coreArguments = @($MigrationArgs) + @(
    'global-migrate',
    '--source-memory', $coreSourceMemory,
    '--source-graph', $coreSourceGraph,
    '--source-config', $coreSourceConfig,
    '--target-root', $targetFull,
    '--project-path', $projectPathFull,
    '--idempotency-key', $IdempotencyKey,
    '--mode', $mode,
    '--report', $reportFull
)
if($ProductionAuthorized){
    Assert-SmProductionTargetPathsHaveNoReparsePoint -TargetRoot $targetFull
    Assert-SmPathComponentsHaveNoReparsePoint -Path $migrationExeFull -Scope 'authorized migration executable'
}
$coreOutput = & $migrationExeFull @coreArguments
$coreExit = $LASTEXITCODE
if ($coreExit -ne 0 -and -not (Test-Path -LiteralPath $reportFull)) { throw "Core migration failed with exit code $coreExit and no report." }
if ($FaultInjection -eq 'after_core') { throw 'FAULT_INJECTION: after_core' }
$report = if($Action -eq 'VerifyManaged'){
    Assert-SmManagedTargetVerifyReport -Path $reportFull
}else{
    Assert-SmMigrationReport -Path $reportFull
}

if ((Get-SmSha256File -Path $sourceMemoryFull) -ne $sourceMemoryShaBefore -or (Get-SmSha256File -Path $sourceGraphFull) -ne $sourceGraphShaBefore -or (Get-SmSha256File -Path $sourceConfigFull) -ne $sourceConfigShaBefore) {
    throw 'Migration modified a source database.'
}
$currentPointerExistsAfter=$false;$currentPointerShaAfter=$null
if($ProductionAuthorized){$currentPointerExistsAfter=Test-Path -LiteralPath $currentPointerPath -PathType Leaf;if($currentPointerExistsAfter){$currentPointerShaAfter=Get-SmSha256File -Path $currentPointerPath};if($currentPointerExistsAfter-ne$currentPointerExistsBefore-or$currentPointerShaAfter-ne$currentPointerShaBefore){throw 'Production migration changed the current pointer.'}}
if ($report.status -eq 'IDEMPOTENCY_CONFLICT') { throw 'IDEMPOTENCY_CONFLICT' }
if ($report.status -eq 'failed' -or $coreExit -ne 0) { throw "Core migration report status: $($report.status)" }
if ($Action -eq 'VerifyManaged' -and $report.status -ne 'verified') { throw "Managed-target verify status: $($report.status)" }
if ($Action -eq 'Plan' -and $report.status -ne 'planned') { throw 'Plan did not return planned status.' }
if ($Action -eq 'Apply' -and $report.status -notin @('applied','replayed')) { throw 'Apply did not return applied/replayed status.' }
Assert-SmHealthyReportChecks -Checks $report.source -Scope source
if($Action-eq'Apply'){
    Assert-SmHealthyReportChecks -Checks $report.backup -Scope backup
    Assert-SmHealthyReportChecks -Checks $report.target -Scope target
    if($null-eq$report.legacy_alias_counts){throw 'Migration alias counts missing.'}
    if($report.preserved_data_equivalent-ne$true){throw 'Migration report did not prove preserved data equivalence.'}
    foreach($projection in @('memory','config','project_graph','equivalent')){if($report.source_to_target_projection.$projection-ne$true){throw "Migration report projection is not equivalent: $projection"}}
    $expectedGraphMapping=('projects/{0}/graph.db'-f$report.project_uuid)
    if($report.source_to_target_mapping.memory-ne'__global__-memory.db'-or$report.source_to_target_mapping.config-ne'_config.db'-or$report.source_to_target_mapping.global_graph-ne'__global__-graph.db'-or$report.source_to_target_mapping.graph-ne$expectedGraphMapping){throw 'Migration report does not match the frozen Stage 14 data layout.'}
    $backupEvidence.core_source_checks=$report.source
}

[ordered]@{
    schema = 'stage14-windows-migration-orchestration/v1'
    action = $Action
    status = 'PASS'
    core_status = $report.status
    core_report_sha256 = Get-SmSha256File -Path $reportFull
    source_read_only = $true
    backup = $backupEvidence
    current_pointer_switched = $false
    current_pointer_unchanged = $(if($ProductionAuthorized){$true}else{$null})
    production_restore_performed = $false
} | ConvertTo-Json -Depth 20
