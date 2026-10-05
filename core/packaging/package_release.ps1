param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [Parameter(Mandatory=$true)][string]$CandidateExe,
    [Parameter(Mandatory=$true)][string]$ExpectedCandidateSha256,
    [Parameter(Mandatory=$true)][string]$ProductionBuildSummaryPath,
    [Parameter(Mandatory=$true)][string]$ExpectedProductionBuildSummarySha256,
    [Parameter(Mandatory=$true)][string]$SourceManifestPath,
    [Parameter(Mandatory=$true)][string]$ExpectedSourceManifestSha256,
    [Parameter(Mandatory=$true)][string]$ReleaseRoot,
    [string]$RunDir,
    [string]$BuildId = 'stage14-rc'
)

$ErrorActionPreference = 'Stop'
$env:PYTHONDONTWRITEBYTECODE = '1'
$CommonModulePath = Join-Path $PSScriptRoot 'windows\SemanticMemory.Common.psm1'
Import-Module -Force -DisableNameChecking -Name $CommonModulePath

function Get-FileSha256 {
    param([string]$Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Read-VerifiedFileSnapshot {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$ExpectedSha256,
        [Parameter(Mandatory=$true)][string]$ErrorCode
    )
    $stream = [System.IO.FileStream]::new(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read,
        65536,
        [System.IO.FileOptions]::SequentialScan
    )
    try {
        $memory = [System.IO.MemoryStream]::new()
        try {
            $stream.CopyTo($memory)
            $bytes = $memory.ToArray()
        } finally {
            $memory.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $actualSha256 = ([System.BitConverter]::ToString(
            $sha.ComputeHash($bytes)
        )).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
    if ($actualSha256 -cne $ExpectedSha256) {
        throw $ErrorCode
    }
    return [pscustomobject]@{
        bytes = [byte[]]$bytes
        length = [int64]$bytes.Length
        sha256 = $actualSha256
    }
}

function Assert-NoReparsePath {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [string]$ErrorCode = 'SOURCE_REPARSE_PATH_FORBIDDEN'
    )
    $probe = [System.IO.Path]::GetFullPath($Path)
    while ($probe) {
        if (Test-Path -LiteralPath $probe) {
            $item = Get-Item -Force -LiteralPath $probe
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "${ErrorCode}: $probe"
            }
        }
        $parent = Split-Path -Parent $probe
        if (-not $parent -or [string]::Equals(
            $parent,
            $probe,
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
            break
        }
        $probe = $parent
    }
}

function Assert-NoReparseTree {
    param([Parameter(Mandatory=$true)][string]$Root)
    Assert-NoReparsePath -Path $Root
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Missing source root: $Root"
    }
    foreach ($item in Get-ChildItem -Force -Recurse -LiteralPath $Root) {
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "SOURCE_REPARSE_TREE_FORBIDDEN: $($item.FullName)"
        }
    }
}

function Test-PathWithin {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Root
    )
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    if ([string]::Equals(
        $full,
        $rootFull,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        return $true
    }
    return $full.StartsWith(
        $rootFull + '\',
        [System.StringComparison]::OrdinalIgnoreCase
    )
}

function Test-PathsOverlap {
    param(
        [Parameter(Mandatory=$true)][string]$Left,
        [Parameter(Mandatory=$true)][string]$Right
    )
    return (Test-PathWithin -Path $Left -Root $Right) -or
        (Test-PathWithin -Path $Right -Root $Left)
}

function Write-JsonCreateNew {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)]$Value
    )
    $full = [System.IO.Path]::GetFullPath($Path)
    $parent = Split-Path -Parent $full
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw 'PACKAGE_RELEASE_RESULT_PARENT_MISSING'
    }
    Assert-NoReparsePath -Path $parent
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes(
        (($Value | ConvertTo-Json -Depth 12) + "`n")
    )
    $stream = [System.IO.File]::Open(
        $full,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None
    )
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

function Read-StageVersion {
    param([string]$Root)
    $versionPath = Join-Path $Root 'VERSION'
    $bytes = [System.IO.File]::ReadAllBytes($versionPath)
    $expectedRc = [System.Text.Encoding]::ASCII.GetBytes("v1.1.0-rc.1`n")
    $expectedFinal = [System.Text.Encoding]::ASCII.GetBytes("v1.1.0`n")
    $isRc = ($bytes.Length -eq $expectedRc.Length)
    if ($isRc) {
        for ($i = 0; $i -lt $bytes.Length; $i++) {
            if ($bytes[$i] -ne $expectedRc[$i]) { $isRc = $false; break }
        }
    }
    $isFinal = ($bytes.Length -eq $expectedFinal.Length)
    if ($isFinal) {
        for ($i = 0; $i -lt $bytes.Length; $i++) {
            if ($bytes[$i] -ne $expectedFinal[$i]) { $isFinal = $false; break }
        }
    }
    if (-not ($isRc -or $isFinal)) {
        throw 'VERSION must be exactly v1.1.0-rc.1 LF or v1.1.0 LF.'
    }
    return [System.Text.Encoding]::ASCII.GetString($bytes).Trim()
}

function New-TextFile {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if (-not $parent -or -not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "Destination parent is missing: $parent"
    }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None
    )
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

function Copy-Required {
    param([string]$From, [string]$To)
    if (-not (Test-Path -LiteralPath $From)) { throw "Missing required file: $From" }
    $parent = Split-Path -Parent $To
    if (-not $parent -or -not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "Destination parent is missing: $parent"
    }
    [System.IO.File]::Copy($From, $To, $false)
}

function Open-VerifiedCandidateStream {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$ExpectedSha256
    )
    $stream = $null
    $sha = $null
    try {
        $stream = [System.IO.FileStream]::new(
            $Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::Read,
            1048576,
            [System.IO.FileOptions]::SequentialScan
        )
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $actualSha256 = ([System.BitConverter]::ToString(
            $sha.ComputeHash($stream)
        )).Replace('-', '').ToLowerInvariant()
        if ($actualSha256 -cne $ExpectedSha256) {
            throw 'CANDIDATE_SHA256_MISMATCH'
        }
        $stream.Position = 0
        return $stream
    } catch {
        if ($null -ne $stream) { $stream.Dispose() }
        throw
    } finally {
        if ($null -ne $sha) { $sha.Dispose() }
    }
}

function Copy-VerifiedStream {
    param(
        [Parameter(Mandatory=$true)]$Stream,
        [Parameter(Mandatory=$true)][string]$To
    )
    if ($null -eq $Stream -or -not $Stream.CanSeek) {
        throw 'CANDIDATE_STREAM_NOT_SEEKABLE'
    }
    $parent = Split-Path -Parent $To
    if (-not $parent -or -not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "Destination parent is missing: $parent"
    }
    $destination = [System.IO.FileStream]::new(
        $To,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None,
        1048576,
        [System.IO.FileOptions]::SequentialScan
    )
    try {
        $Stream.Position = 0
        $Stream.CopyTo($destination)
        $destination.Flush($true)
    } finally {
        $destination.Dispose()
    }
}

function Copy-RequiredTree {
    param(
        [string]$From,
        [string]$To,
        [Parameter(Mandatory=$true)]$LeaseSink
    )
    if (-not (Test-Path -LiteralPath $From -PathType Container)) { throw "Missing required directory: $From" }
    if (Test-Path -LiteralPath $To) { throw "Destination tree already exists: $To" }
    $LeaseSink.Add((Enter-SmStableDirectoryLease `
        -Path $To `
        -RequireNewLeaf `
        -ErrorCode 'PACKAGE_RELEASE_TREE_LEASE_FAILED'))
    $sourceFull = [System.IO.Path]::GetFullPath($From).TrimEnd('\')
    $directories = @(
        Get-ChildItem -Force -Recurse -Directory -LiteralPath $sourceFull |
            Sort-Object { $_.FullName.Length },FullName
    )
    foreach ($directory in $directories) {
        $relative = $directory.FullName.Substring($sourceFull.Length + 1)
        $destination = Join-Path $To $relative
        $LeaseSink.Add((Enter-SmStableDirectoryLease `
            -Path $destination `
            -RequireNewLeaf `
            -ErrorCode 'PACKAGE_RELEASE_TREE_LEASE_FAILED'))
    }
    foreach ($file in Get-ChildItem -Force -Recurse -File -LiteralPath $sourceFull) {
        $relative = $file.FullName.Substring($sourceFull.Length + 1)
        Copy-Required -From $file.FullName -To (Join-Path $To $relative)
    }
}

function Get-RelativeFileRecord {
    param([string]$Path, [string]$Root)
    $full = [System.IO.Path]::GetFullPath($Path)
    $base = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    if (-not $full.StartsWith($base, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "File is outside manifest root: $full"
    }
    return [ordered]@{
        path = $full.Substring($base.Length).Replace('\','/')
        bytes = (Get-Item -LiteralPath $full).Length
        sha256 = Get-FileSha256 -Path $full
    }
}

function Write-Checksums {
    param([string]$Root, [string]$Output)
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($file in Get-ChildItem -Recurse -File -LiteralPath $Root | Sort-Object FullName) {
        if ($file.Name -eq 'checksums.sha256' -or $file.Name -eq 'release-manifest.json') { continue }
        $relative = $file.FullName.Substring($Root.Length).TrimStart('\') -replace '\\','/'
        $lines.Add((Get-FileSha256 -Path $file.FullName) + '  ' + $relative)
    }
    [System.IO.File]::WriteAllText(
        $Output,
        (($lines -join "`n") + "`n"),
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Write-DefenderEvidence {
    param([string]$Root, [string]$Output)
    $status = 'not_available'
    $detectionResult = 'scan_not_available'
    $errorText = $null
    $cmd = Get-Command Start-MpScan -ErrorAction SilentlyContinue
    if ($cmd) {
        try {
            Start-MpScan -ErrorAction Stop -ScanType CustomScan -ScanPath $Root | Out-Null
            $status = 'scan_completed'
            $detectionResult = 'no_detection_reported_by_packager'
        } catch {
            $status = 'scan_failed'
            $detectionResult = 'scan_failed_no_detection_claim'
            $errorText = $_.Exception.Message
        }
    }
    $result = [ordered]@{
        schema = 'stage13-defender-evidence/v1'
        scan_status = $status
        scan_target = 'release-root'
        detection_result = $detectionResult
        error = $errorText
    }
    $result | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 -LiteralPath $Output
}

$SourceRoot = [System.IO.Path]::GetFullPath($SourceRoot)
$CandidateExe = [System.IO.Path]::GetFullPath($CandidateExe)
$ProductionBuildSummaryPath = [System.IO.Path]::GetFullPath($ProductionBuildSummaryPath)
$SourceManifestPath = [System.IO.Path]::GetFullPath($SourceManifestPath)
$ReleaseRoot = [System.IO.Path]::GetFullPath($ReleaseRoot)
$RunDirFull = $null
$RunResultPath = $null
if (-not [string]::IsNullOrWhiteSpace($RunDir)) {
    $RunDirFull = [System.IO.Path]::GetFullPath($RunDir)
    $RunResultPath = [System.IO.Path]::GetFullPath(
        (Join-Path $RunDirFull 'package-release-result.json')
    )
    if ((Test-PathsOverlap -Left $RunDirFull -Right $ReleaseRoot) -or
        (Test-PathsOverlap -Left $RunDirFull -Right $SourceRoot) -or
        [string]::Equals(
            $RunResultPath,
            $CandidateExe,
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
        throw 'PACKAGE_RELEASE_RUN_DIR_PATH_CONFLICT'
    }
}

$directoryLeases = New-Object System.Collections.Generic.List[object]
$runDirAnchorLease = $null
$candidateStream = $null
$candidateStreamLength = $null
$managerPortableRoot = Join-Path $ReleaseRoot 'manager-portable'
$offlineInstallerRoot = Join-Path $ReleaseRoot 'offline-installer'
$offlinePayloadRoot = Join-Path $offlineInstallerRoot 'payload'
$offlinePluginRoot = Join-Path $offlineInstallerRoot 'plugin\semantic-memory'

try {
    $directoryLeases.Add((Enter-SmStableDirectoryAnchorLease `
        -Path $ReleaseRoot `
        -ErrorCode 'PACKAGE_RELEASE_ROOT_ANCHOR_LEASE_FAILED' `
        -InvalidTargetErrorCode 'PACKAGE_RELEASE_REPARSE_PATH_FORBIDDEN'))
    if ($RunDirFull) {
        $runDirAnchorLease = Enter-SmStableDirectoryAnchorLease `
            -Path $RunDirFull `
            -ErrorCode 'PACKAGE_RELEASE_RUN_DIR_ANCHOR_LEASE_FAILED' `
            -InvalidTargetErrorCode 'PACKAGE_RELEASE_REPARSE_PATH_FORBIDDEN'
        $directoryLeases.Add($runDirAnchorLease)
    }
    Assert-NoReparsePath `
        -Path $ReleaseRoot `
        -ErrorCode 'PACKAGE_RELEASE_REPARSE_PATH_FORBIDDEN'
    if ($RunDirFull) {
        Assert-NoReparsePath -Path $RunDirFull
        Assert-NoReparsePath -Path $RunResultPath
        if (Test-Path -LiteralPath $RunResultPath) {
            throw 'PACKAGE_RELEASE_RESULT_ALREADY_EXISTS'
        }
    }

Assert-NoReparseTree -Root $SourceRoot
foreach ($expected in @(
    $ExpectedCandidateSha256,
    $ExpectedProductionBuildSummarySha256,
    $ExpectedSourceManifestSha256
)) {
    if ($expected -cnotmatch '^[0-9a-f]{64}$') {
        throw 'PROVENANCE_SHA256_MUST_BE_LOWERCASE_64_HEX'
    }
}
foreach ($inputPath in @($CandidateExe,$ProductionBuildSummaryPath,$SourceManifestPath)) {
    Assert-NoReparsePath -Path $inputPath
    if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
        throw "PROVENANCE_INPUT_MISSING: $inputPath"
    }
}
if ((Get-FileSha256 -Path $CandidateExe) -cne $ExpectedCandidateSha256) {
    throw 'CANDIDATE_SHA256_MISMATCH'
}
$candidateStream = Open-VerifiedCandidateStream `
    -Path $CandidateExe `
    -ExpectedSha256 $ExpectedCandidateSha256
$candidateStreamLength = [int64]$candidateStream.Length
$ProductionSummarySnapshot = Read-VerifiedFileSnapshot `
    -Path $ProductionBuildSummaryPath `
    -ExpectedSha256 $ExpectedProductionBuildSummarySha256 `
    -ErrorCode 'PRODUCTION_BUILD_SUMMARY_SHA256_MISMATCH'
$SourceManifestSnapshot = Read-VerifiedFileSnapshot `
    -Path $SourceManifestPath `
    -ExpectedSha256 $ExpectedSourceManifestSha256 `
    -ErrorCode 'SOURCE_MANIFEST_SHA256_MISMATCH'
$ProductionSummaryText = [System.Text.UTF8Encoding]::new(
    $false,
    $true
).GetString($ProductionSummarySnapshot.bytes)
if ($ProductionSummaryText.Length -gt 0 -and
    $ProductionSummaryText[0] -eq [char]0xFEFF) {
    $ProductionSummaryText = $ProductionSummaryText.Substring(1)
}
$ProductionSummary = $ProductionSummaryText | ConvertFrom-Json
$SummaryChecks = @($ProductionSummary.checks.PSObject.Properties)
$SummarySchemaValid = [string]$ProductionSummary.schema -cin @(
    'stage14-production-build/v2',
    'stage14-rev9-production-build/v1'
)
$SummaryStatusValid = [string]$ProductionSummary.status -cin @(
    'PASS_REV8_UI_PRODUCTION_BUILD',
    'PASS_STAGE14_FINAL_UI_PRODUCTION_BUILD',
    'PASS_REV9_PRODUCTION_BUILD'
)
$SummaryCandidateWitness = if ($null -ne $ProductionSummary.candidate.witness) {
    $ProductionSummary.candidate.witness
} else {
    $ProductionSummary.candidate
}
$SummarySourceManifestWitness = $ProductionSummary.source_manifest
$SummaryCandidatePath = [System.IO.Path]::GetFullPath(
    [string]$ProductionSummary.candidate.path
)
$SummarySourceManifestPath = [System.IO.Path]::GetFullPath(
    [string]$ProductionSummary.source_manifest.path
)
if (
    -not $SummarySchemaValid -or
    -not $SummaryStatusValid -or
    $SummaryChecks.Count -eq 0 -or
    @($SummaryChecks | Where-Object {
        $_.Value -isnot [bool] -or -not [bool]$_.Value
    }).Count -ne 0 -or
    -not [string]::Equals(
        $SummaryCandidatePath,
        $CandidateExe,
        [System.StringComparison]::OrdinalIgnoreCase
    ) -or
    [int64]$SummaryCandidateWitness.bytes -ne $candidateStreamLength -or
    [string]$SummaryCandidateWitness.sha256 -cne $ExpectedCandidateSha256 -or
    -not [string]::Equals(
        $SummarySourceManifestPath,
        $SourceManifestPath,
        [System.StringComparison]::OrdinalIgnoreCase
    ) -or
    [int64]$SummarySourceManifestWitness.bytes -ne
        $SourceManifestSnapshot.length -or
    [string]$ProductionSummary.source_manifest.sha256 -cne
        $ExpectedSourceManifestSha256
) {
    throw 'PRODUCTION_BUILD_PROVENANCE_BINDING_MISMATCH'
}
$version = Read-StageVersion -Root $SourceRoot
$publicVersion = $version.TrimStart('v')

    $directoryLeases.Add((Enter-SmStableDirectoryLease `
        -Path $ReleaseRoot `
        -RequireNewLeaf `
        -ErrorCode 'PACKAGE_RELEASE_ROOT_LEASE_FAILED'))
    $directoryLeases.Add((Enter-SmStableDirectoryLease `
        -Path $managerPortableRoot `
        -RequireNewLeaf `
        -ErrorCode 'PACKAGE_RELEASE_TREE_LEASE_FAILED'))
    $directoryLeases.Add((Enter-SmStableDirectoryLease `
        -Path $offlineInstallerRoot `
        -RequireNewLeaf `
        -ErrorCode 'PACKAGE_RELEASE_TREE_LEASE_FAILED'))
    $directoryLeases.Add((Enter-SmStableDirectoryLease `
        -Path $offlinePayloadRoot `
        -RequireNewLeaf `
        -ErrorCode 'PACKAGE_RELEASE_TREE_LEASE_FAILED'))

    if ($BuildId -notmatch '^[A-Za-z0-9._+-]+$') { throw 'BuildId is invalid for Stage14 version_id.' }
    $versionId = "$version+$BuildId"

    & python (Join-Path $SourceRoot 'packaging\release_manifest.py') --source-root $SourceRoot --check-source | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'source server.json does not match VERSION.' }

    Copy-VerifiedStream -Stream $candidateStream -To (Join-Path $ReleaseRoot 'semantic-memory-mcp.exe')
    Copy-VerifiedStream -Stream $candidateStream -To (Join-Path $managerPortableRoot 'semantic-memory-manager.exe')
    Copy-VerifiedStream -Stream $candidateStream -To (Join-Path $offlinePayloadRoot 'semantic-memory-mcp.exe')
    Copy-VerifiedStream -Stream $candidateStream -To (Join-Path $offlinePayloadRoot 'semantic-memory-hook.exe')
    Copy-VerifiedStream -Stream $candidateStream -To (Join-Path $offlinePayloadRoot 'semantic-memory-manager.exe')
    Copy-Required -From (Join-Path $SourceRoot 'server.json') -To (Join-Path $ReleaseRoot 'server.json')
    Copy-Required -From (Join-Path $SourceRoot 'server.json') -To (Join-Path $offlinePayloadRoot 'server.json')
    Copy-Required -From (Join-Path $SourceRoot 'LICENSE') -To (Join-Path $ReleaseRoot 'LICENSE')
    Copy-Required -From (Join-Path $SourceRoot 'THIRD_PARTY.md') -To (Join-Path $ReleaseRoot 'THIRD_PARTY.md')

$payloadManifestPath = Join-Path $offlinePayloadRoot 'payload-manifest.json'
& python (Join-Path $SourceRoot 'packaging\release_manifest.py') --source-root $SourceRoot --stage14-payload-root $offlinePayloadRoot --stage14-version-id $versionId --stage14-payload-output $payloadManifestPath
if ($LASTEXITCODE -ne 0) { throw 'Stage14 payload manifest generation failed.' }

New-TextFile -Path (Join-Path $ReleaseRoot 'manager-portable\Open Memory Manager.cmd') -Text "@echo off`r`nsetlocal`r`n`"%~dp0semantic-memory-manager.exe`" manager`r`n"
foreach ($name in @('Install-SemanticMemoryV2.ps1','Install-SemanticMemoryPlugin.ps1','SemanticMemory.Common.psm1','semantic-memory-launcher.ps1','Invoke-Stage14Migration.ps1','Repair-SemanticMemory.ps1','Invoke-PackageAcceptance.ps1','README.zh-CN.md','Install Semantic Memory.cmd')) {
    Copy-Required -From (Join-Path $SourceRoot ("packaging\windows\{0}" -f $name)) -To (Join-Path $offlineInstallerRoot $name)
}
    Copy-RequiredTree `
        -From (Join-Path $SourceRoot 'packaging\semantic-memory') `
        -To $offlinePluginRoot `
        -LeaseSink $directoryLeases

$payloadManifest = Get-Content -Raw -Encoding UTF8 -LiteralPath $payloadManifestPath | ConvertFrom-Json
$scriptRecords = @()
foreach ($name in @('Install-SemanticMemoryV2.ps1','Install-SemanticMemoryPlugin.ps1','SemanticMemory.Common.psm1','semantic-memory-launcher.ps1','Invoke-Stage14Migration.ps1','Repair-SemanticMemory.ps1','Invoke-PackageAcceptance.ps1','README.zh-CN.md','Install Semantic Memory.cmd')) {
    $scriptRecords += Get-RelativeFileRecord -Path (Join-Path $offlineInstallerRoot $name) -Root $offlineInstallerRoot
}
$pluginRecords = @()
foreach ($file in Get-ChildItem -Force -Recurse -File -LiteralPath $offlinePluginRoot | Sort-Object FullName) {
    $pluginRecords += Get-RelativeFileRecord -Path $file.FullName -Root $offlineInstallerRoot
}

$installerManifest = [ordered]@{
    schema = 'stage14-offline-installer-manifest/v1'
    version = $version
    version_id = $versionId
    public_version = $publicVersion
    runtime = 'PowerShell 5.1'
    admin_required = $false
    registry_written = $false
    path_modified = $false
    service_installed = $false
    autostart_installed = $false
    data_retained_by_default = $true
    toolchain = [ordered]@{
        schema = 'stage14-packaging-toolchain/v1'
        target_os = 'windows'
        target_arch = 'x86_64'
        packaging_runtime = 'PowerShell 5.1'
        manifest_runtime = 'Python 3'
        archive_format = 'zip'
        package_script_sha256 = Get-FileSha256 -Path (
            Join-Path $SourceRoot 'packaging\package_release.ps1'
        )
        manifest_generator_sha256 = Get-FileSha256 -Path (
            Join-Path $SourceRoot 'packaging\release_manifest.py'
        )
    }
    payload = [ordered]@{
        manifest = 'payload/payload-manifest.json'
        manifest_sha256 = Get-FileSha256 -Path $payloadManifestPath
        entrypoints = $payloadManifest.entrypoints
        files = $payloadManifest.files
    }
    scripts = $scriptRecords
    personal_plugin = [ordered]@{
        root = 'plugin/semantic-memory'
        manifest = 'plugin/semantic-memory/.codex-plugin/plugin.json'
        files = $pluginRecords
        transaction = [ordered]@{
            script = 'Install-SemanticMemoryPlugin.ps1'
            schema = 'stage14-personal-plugin-transaction/v1'
            actions = @('Preview','Apply','Verify','Rollback','Recover')
            cas_preconditions = @(
                'ExpectedSourceTreeSha256',
                'ExpectedCacheTreeSha256',
                'ExpectedMarketplaceSha256'
            )
            absent_sentinel = 'ABSENT'
            cli_commands = @(
                @('plugin','marketplace','add','<personal-marketplace-path>'),
                @('plugin','add','semantic-memory@personal')
            )
            duplicate_mcp_registration_forbidden = $true
        }
    }
}
$installerManifest | ConvertTo-Json -Depth 14 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $offlineInstallerRoot 'installer-manifest.json')

$portableManifest = [ordered]@{
    schema = 'stage13-manager-portable-manifest/v1'
    version = $version
    entrypoint = 'Open Memory Manager.cmd'
    command = 'semantic-memory-manager.exe manager'
    runtime_dependencies = @()
    network_binding = '127.0.0.1:ephemeral'
    external_assets = $false
    files = @(
        [ordered]@{
            path = 'semantic-memory-manager.exe'
            bytes = (Get-Item -LiteralPath (Join-Path $ReleaseRoot 'manager-portable\semantic-memory-manager.exe')).Length
            sha256 = Get-FileSha256 -Path (Join-Path $ReleaseRoot 'manager-portable\semantic-memory-manager.exe')
        },
        [ordered]@{
            path = 'Open Memory Manager.cmd'
            bytes = (Get-Item -LiteralPath (Join-Path $ReleaseRoot 'manager-portable\Open Memory Manager.cmd')).Length
            sha256 = Get-FileSha256 -Path (Join-Path $ReleaseRoot 'manager-portable\Open Memory Manager.cmd')
        }
    )
}
$portableManifest | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $ReleaseRoot 'manager-portable\portable-manifest.json')

& python (Join-Path $SourceRoot 'packaging\licenses\build_notices.py') --source-root $SourceRoot --output (Join-Path $ReleaseRoot 'NOTICES.txt')
if ($LASTEXITCODE -ne 0) { throw 'notices generation failed.' }

$portableZip = Join-Path $ReleaseRoot ("semantic-memory-manager-portable-$version.zip")
$installerZip = Join-Path $ReleaseRoot ("semantic-memory-mcp-offline-installer-$version.zip")
Compress-Archive -Path (Join-Path $ReleaseRoot 'manager-portable\*') -DestinationPath $portableZip -Force
Compress-Archive -Path (Join-Path $ReleaseRoot 'offline-installer\*') -DestinationPath $installerZip -Force

powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $SourceRoot 'packaging\windows\Invoke-CodeSign.ps1') -FilePath (Join-Path $ReleaseRoot 'semantic-memory-mcp.exe') -Output (Join-Path $ReleaseRoot 'signing-status.json') | Out-Null
Write-DefenderEvidence -Root $ReleaseRoot -Output (Join-Path $ReleaseRoot 'defender-evidence.json')
& python (Join-Path $SourceRoot 'packaging\build_sbom.py') --source-root $SourceRoot --release-root $ReleaseRoot --output (Join-Path $ReleaseRoot 'sbom.cdx.json')
if ($LASTEXITCODE -ne 0) { throw 'SBOM generation failed.' }
Write-Checksums -Root $ReleaseRoot -Output (Join-Path $ReleaseRoot 'checksums.sha256')
& python (Join-Path $SourceRoot 'packaging\release_manifest.py') `
    --source-root $SourceRoot `
    --release-root $ReleaseRoot `
    --candidate-exe $CandidateExe `
    --build-id $BuildId `
    --source-manifest-path $SourceManifestPath `
    --source-manifest-sha256 $ExpectedSourceManifestSha256 `
    --production-build-summary-path $ProductionBuildSummaryPath `
    --production-build-summary-sha256 $ExpectedProductionBuildSummarySha256 `
    --output (Join-Path $ReleaseRoot 'release-manifest.json')
if ($LASTEXITCODE -ne 0) { throw 'release manifest generation failed.' }

$result = [ordered]@{
    schema = 'stage14-package-release-result/v1'
    status = 'PASS_STAGE14_PACKAGE_RELEASE'
    version = $version
    version_id = $versionId
    release_root = $ReleaseRoot
    release_exe_sha256 = Get-FileSha256 -Path (Join-Path $ReleaseRoot 'semantic-memory-mcp.exe')
    production_build_summary_sha256 = $ExpectedProductionBuildSummarySha256
    source_manifest_sha256 = $ExpectedSourceManifestSha256
    portable_zip = Split-Path -Leaf $portableZip
    installer_zip = Split-Path -Leaf $installerZip
    payload_manifest_sha256 = Get-FileSha256 -Path $payloadManifestPath
    personal_plugin_file_count = $pluginRecords.Count
    personal_plugin_transaction = 'Install-SemanticMemoryPlugin.ps1'
    public_release_ready = $false
    real_second_machine_accepted = $false
}

    if ($RunDirFull) {
        if ([string]::Equals(
            [string]$runDirAnchorLease.path,
            $RunDirFull.TrimEnd('\'),
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
            $directoryLeases.Add((Enter-SmStableDirectoryLease `
                -Path $RunDirFull `
                -RequireExisting `
                -ErrorCode 'PACKAGE_RELEASE_RUN_DIR_LEASE_FAILED'))
        } else {
            $directoryLeases.Add((Enter-SmStableDirectoryLease `
                -Path $RunDirFull `
                -RequireNewLeaf `
                -ErrorCode 'PACKAGE_RELEASE_RUN_DIR_LEASE_FAILED'))
        }
        if (Test-Path -LiteralPath $RunResultPath) {
            throw 'PACKAGE_RELEASE_RESULT_ALREADY_EXISTS'
        }
        Write-JsonCreateNew -Path $RunResultPath -Value $result
    }
    $result | ConvertTo-Json -Depth 8
} finally {
    if ($null -ne $candidateStream) {
        $candidateStream.Dispose()
    }
    for ($index = $directoryLeases.Count - 1; $index -ge 0; $index--) {
        Exit-SmStableDirectoryLease -Lease $directoryLeases[$index]
    }
}
