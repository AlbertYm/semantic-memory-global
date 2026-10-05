param(
    [Parameter(Mandatory=$true)][string]$ReleaseRoot,
    [Parameter(Mandatory=$true)][string]$WorkRoot,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
$CommonModulePath = Join-Path $PSScriptRoot 'SemanticMemory.Common.psm1'
Import-Module -Force -DisableNameChecking -Name $CommonModulePath

function Get-FileSha256 {
    param([string]$Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Get-BytesSha256 {
    param([Parameter(Mandatory=$true)][byte[]]$Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace(
            '-',
            ''
        ).ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Test-NonnegativeJsonInteger {
    param($Value)
    if ($null -eq $Value) { return $false }
    $typeName = $Value.GetType().Name
    return $typeName -cin @('Int32','Int64') -and [int64]$Value -ge 0
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

function Assert-NoReparsePath {
    param([Parameter(Mandatory=$true)][string]$Path)
    $probe = [System.IO.Path]::GetFullPath($Path)
    while ($probe) {
        if (Test-Path -LiteralPath $probe) {
            $item = Get-Item -Force -LiteralPath $probe
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'PACKAGE_ACCEPTANCE_REPARSE_PATH_FORBIDDEN'
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

function Enter-StableReleaseTreeLeases {
    param(
        [Parameter(Mandatory=$true)][string]$Root,
        [Parameter(Mandatory=$true)]$LeaseSink,
        [Parameter(Mandatory=$true)]$DirectorySink
    )
    try {
        $LeaseSink.Add((Enter-SmStableDirectoryLease `
            -Path $Root `
            -RequireExisting `
            -ErrorCode 'PACKAGE_ACCEPTANCE_RELEASE_LEASE_FAILED'))
    } catch {
        if ($_.Exception.Message -match 'STABLE_DIRECTORY_LEASE_TARGET_INVALID') {
            throw 'PACKAGE_ACCEPTANCE_REPARSE_PATH_FORBIDDEN'
        }
        throw
    }
    $rootLease = $LeaseSink[$LeaseSink.Count - 1]
    $DirectorySink.Add([string]$rootLease.path)
    $pending = [System.Collections.Generic.Queue[string]]::new()
    $pending.Enqueue([string]$rootLease.path)
    while ($pending.Count -gt 0) {
        $current = $pending.Dequeue()
        foreach ($item in Get-ChildItem -Force -LiteralPath $current) {
            if ($item.PSIsContainer) {
                try {
                    $discoveredIdentity = Get-SmDirectoryIdentitySnapshot `
                        -Path $item.FullName `
                        -ErrorCode 'PACKAGE_ACCEPTANCE_RELEASE_LEASE_FAILED'
                    $LeaseSink.Add((Enter-SmStableDirectoryLease `
                        -Path $item.FullName `
                        -RequireExisting `
                        -ErrorCode 'PACKAGE_ACCEPTANCE_RELEASE_LEASE_FAILED'))
                } catch {
                    if ($_.Exception.Message -match 'STABLE_DIRECTORY_LEASE_TARGET_INVALID') {
                        throw 'PACKAGE_ACCEPTANCE_REPARSE_PATH_FORBIDDEN'
                    }
                    throw
                }
                $childLease = $LeaseSink[$LeaseSink.Count - 1]
                if ([string]$childLease.identity -cne [string]$discoveredIdentity) {
                    throw 'PACKAGE_ACCEPTANCE_RELEASE_IDENTITY_DRIFT'
                }
                $DirectorySink.Add([string]$childLease.path)
                $pending.Enqueue([string]$childLease.path)
            } elseif (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'PACKAGE_ACCEPTANCE_REPARSE_PATH_FORBIDDEN'
            }
        }
    }
}

function Get-LeasedTreeFiles {
    param(
        [Parameter(Mandatory=$true)][string]$Root,
        [Parameter(Mandatory=$true)]$LeasedDirectories
    )
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    $knownDirectories = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase
    )
    foreach ($rawDirectory in @($LeasedDirectories)) {
        $directory = [System.IO.Path]::GetFullPath([string]$rawDirectory).TrimEnd('\')
        if (Test-PathWithin -Path $directory -Root $rootFull) {
            [void]$knownDirectories.Add($directory)
        }
    }
    if (-not $knownDirectories.Contains($rootFull)) {
        throw 'PACKAGE_ACCEPTANCE_RELEASE_TREE_LEASE_MISSING'
    }

    $files = New-Object System.Collections.Generic.List[object]
    foreach ($directory in @($knownDirectories | Sort-Object)) {
        foreach ($item in Get-ChildItem -Force -LiteralPath $directory) {
            $itemFull = [System.IO.Path]::GetFullPath($item.FullName).TrimEnd('\')
            if ($item.PSIsContainer) {
                if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                    throw 'PACKAGE_ACCEPTANCE_REPARSE_PATH_FORBIDDEN'
                }
                if (-not $knownDirectories.Contains($itemFull)) {
                    throw 'PACKAGE_ACCEPTANCE_RELEASE_TREE_DRIFT'
                }
                continue
            }
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'PACKAGE_ACCEPTANCE_REPARSE_PATH_FORBIDDEN'
            }
            $files.Add($item)
        }
    }
    return [object[]]$files.ToArray()
}

function Write-PackageAcceptanceResultCreateNew {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)]$Value
    )
    $full = [System.IO.Path]::GetFullPath($Path)
    $parent = Split-Path -Parent $full
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw 'PACKAGE_ACCEPTANCE_OUTPUT_PARENT_MISSING'
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

function Test-ManifestRecords {
    param(
        [Parameter(Mandatory=$true)][string]$Root,
        [Parameter(Mandatory=$true)]$Records
    )
    $seen = @{}
    foreach ($record in @($Records)) {
        $propertyNames = @($record.PSObject.Properties.Name | Sort-Object)
        if ([string]::Join("`n", $propertyNames) -cne
            [string]::Join("`n", @('bytes','path','sha256'))) {
            return $false
        }
        $relative = [string]$record.path
        if (-not $relative -or [System.IO.Path]::IsPathRooted($relative) -or $relative.Contains('..') -or $relative.Contains('\')) {
            return $false
        }
        if (-not (Test-NonnegativeJsonInteger -Value $record.bytes) -or
            [string]$record.sha256 -cnotmatch '^[0-9a-f]{64}$') {
            return $false
        }
        if ($seen.ContainsKey($relative)) { return $false }
        $seen[$relative] = $true
        $path = [System.IO.Path]::GetFullPath((Join-Path $Root $relative))
        $rootPrefix = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
        if (-not $path.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) { return $false }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
        if ((Get-Item -LiteralPath $path).Length -ne [long]$record.bytes) { return $false }
        if ((Get-FileSha256 -Path $path) -ne ([string]$record.sha256).ToLowerInvariant()) { return $false }
    }
    return $true
}

function Get-RelativeFilePaths {
    param(
        [Parameter(Mandatory=$true)][string]$Root,
        $LeasedDirectories = $null
    )
    $files = if ($null -eq $LeasedDirectories) {
        @(Get-ChildItem -Force -Recurse -File -LiteralPath $Root)
    } else {
        @(Get-LeasedTreeFiles -Root $Root -LeasedDirectories $LeasedDirectories)
    }
    return @(
        $files |
            ForEach-Object {
                $_.FullName.Substring(
                    [System.IO.Path]::GetFullPath($Root).TrimEnd('\').Length
                ).TrimStart('\').Replace('\','/')
            } |
            Sort-Object
    )
}

function Test-ExactStringSequence {
    param($Left,$Right)
    return [string]::Join("`n", @($Left)) -ceq [string]::Join("`n", @($Right))
}

function Test-SingleFileRecord {
    param(
        [Parameter(Mandatory=$true)][string]$Root,
        [Parameter(Mandatory=$true)]$Record,
        [Parameter(Mandatory=$true)][string]$ExpectedPath
    )
    return (
        [string]$Record.path -ceq $ExpectedPath -and
        (Test-ManifestRecords -Root $Root -Records @($Record))
    )
}

function Test-ZipMatchesTree {
    param(
        [Parameter(Mandatory=$true)][string]$Archive,
        [Parameter(Mandatory=$true)][string]$SourceRoot,
        [Parameter(Mandatory=$true)][string]$ExtractRoot,
        $SourceLeasedDirectories = $null
    )
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::ExtractToDirectory($Archive, $ExtractRoot)
    } catch {
        return $false
    }
    $sourcePaths = Get-RelativeFilePaths `
        -Root $SourceRoot `
        -LeasedDirectories $SourceLeasedDirectories
    $extractPaths = Get-RelativeFilePaths -Root $ExtractRoot
    if (-not (Test-ExactStringSequence -Left $sourcePaths -Right $extractPaths)) {
        return $false
    }
    foreach ($relative in $sourcePaths) {
        $source = Join-Path $SourceRoot $relative
        $extracted = Join-Path $ExtractRoot $relative
        if ((Get-Item -LiteralPath $source).Length -ne
            (Get-Item -LiteralPath $extracted).Length -or
            (Get-FileSha256 -Path $source) -cne
            (Get-FileSha256 -Path $extracted)) {
            return $false
        }
    }
    return $true
}

function Invoke-Installer {
    param([string]$Action, [string]$PackageRoot, [string]$InstallRoot)
    $script = Join-Path $PackageRoot 'Install-SemanticMemoryV2.ps1'
    $args = @('-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',$script,'-Action',$Action,'-InstallRoot',$InstallRoot)
    if ($Action -eq 'Install') {
        $args += @('-PayloadManifestPath',(Join-Path $PackageRoot 'payload\payload-manifest.json'))
    }
    $output = & powershell.exe @args 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "installer action failed: $Action"
    }
    return ($output | Out-String)
}

function Invoke-ConfigRepair {
    param(
        [Parameter(Mandatory=$true)][string]$Mode,
        [Parameter(Mandatory=$true)][string]$PackageRoot,
        [Parameter(Mandatory=$true)][string]$ConfigPath,
        [Parameter(Mandatory=$true)][string]$InstallRoot,
        [string]$ExpectedConfigSha256,
        [string]$BackupRoot,
        [switch]$EnableProductionCanary,
        [string]$CanaryAuthManifestPath,
        [string]$CanaryAuthSha256
    )
    $script = Join-Path $PackageRoot 'Repair-SemanticMemory.ps1'
    $args = @(
        '-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',
        '-File',$script,
        '-Mode',$Mode,
        '-ConfigPath',$ConfigPath,
        '-InstallRoot',$InstallRoot
    )
    if ($ExpectedConfigSha256) {
        $args += @('-ExpectedConfigSha256',$ExpectedConfigSha256)
    }
    if ($BackupRoot) {
        $args += @('-BackupRoot',$BackupRoot)
    }
    if ($EnableProductionCanary) {
        $args += @(
            '-EnableProductionCanary',
            '-CanaryAuthManifestPath',$CanaryAuthManifestPath,
            '-CanaryAuthSha256',$CanaryAuthSha256
        )
    }
    $output = & powershell.exe @args 2>&1
    $exitCode = $LASTEXITCODE
    $text = $output | Out-String
    if ($exitCode -ne 0) {
        $safeCode = 'UNKNOWN'
        try {
            $parsedFailure = $text | ConvertFrom-Json
            if ([string]$parsedFailure.error_code -match '^[A-Z0-9_]+$') {
                $safeCode = [string]$parsedFailure.error_code
            } elseif ([string]$parsedFailure.status -match '^[A-Z0-9_]+$') {
                $safeCode = [string]$parsedFailure.status
            }
        } catch {}
        throw "config repair action failed: $Mode ($safeCode)"
    }
    return [pscustomobject]@{
        text = $text
        value = ($text | ConvertFrom-Json)
    }
}

function Invoke-PluginTransaction {
    param(
        [Parameter(Mandatory=$true)][string]$Action,
        [Parameter(Mandatory=$true)][string]$PackageRoot,
        [Parameter(Mandatory=$true)][string]$UserHome,
        [Parameter(Mandatory=$true)][string]$CodexHome,
        [Parameter(Mandatory=$true)][string]$StateRoot,
        [object[]]$ExtraArguments = @(),
        [int]$ExpectedExitCode = 0
    )
    $script = Join-Path $PackageRoot 'Install-SemanticMemoryPlugin.ps1'
    $args = @(
        '-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',
        '-File',$script,
        '-Action',$Action,
        '-PackageRoot',$PackageRoot,
        '-UserHome',$UserHome,
        '-CodexHome',$CodexHome,
        '-StateRoot',$StateRoot
    ) + @($ExtraArguments)
    $output = & powershell.exe @args 2>&1
    $exitCode = $LASTEXITCODE
    $text = $output | Out-String
    if ($exitCode -ne $ExpectedExitCode) {
        throw "plugin transaction action failed: $Action"
    }
    return [pscustomobject]@{
        text = $text
        value = ($text | ConvertFrom-Json)
    }
}

$ReleaseRoot = [System.IO.Path]::GetFullPath($ReleaseRoot)
$WorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
$OutputFull = $null
if (Test-PathWithin -Path $WorkRoot -Root $ReleaseRoot) {
    throw 'PACKAGE_ACCEPTANCE_WORK_ROOT_INSIDE_RELEASE_ROOT'
}
if (-not [string]::IsNullOrWhiteSpace($Output)) {
    $OutputFull = [System.IO.Path]::GetFullPath($Output)
    if ((Test-PathsOverlap -Left $OutputFull -Right $ReleaseRoot) -or
        (Test-PathsOverlap -Left $OutputFull -Right $WorkRoot)) {
        throw 'PACKAGE_ACCEPTANCE_OUTPUT_PATH_CONFLICT'
    }
}

$directoryLeases = New-Object System.Collections.Generic.List[object]
$releaseTreeDirectories = [System.Collections.Generic.List[string]]::new()
try {
    Enter-StableReleaseTreeLeases `
        -Root $ReleaseRoot `
        -LeaseSink $directoryLeases `
        -DirectorySink $releaseTreeDirectories

    $workRootAnchorLease = Enter-SmStableDirectoryAnchorLease `
        -Path $WorkRoot `
        -ErrorCode 'PACKAGE_ACCEPTANCE_WORK_ROOT_ANCHOR_LEASE_FAILED' `
        -InvalidTargetErrorCode 'PACKAGE_ACCEPTANCE_REPARSE_PATH_FORBIDDEN'
    $directoryLeases.Add($workRootAnchorLease)
    [void]@(Get-LeasedTreeFiles `
        -Root $ReleaseRoot `
        -LeasedDirectories $releaseTreeDirectories)
    if ($OutputFull) {
        $outputParent = Split-Path -Parent $OutputFull
        $outputParentAnchorLease = Enter-SmStableDirectoryAnchorLease `
            -Path $outputParent `
            -ErrorCode 'PACKAGE_ACCEPTANCE_OUTPUT_ANCHOR_LEASE_FAILED' `
            -InvalidTargetErrorCode 'PACKAGE_ACCEPTANCE_REPARSE_PATH_FORBIDDEN'
        $directoryLeases.Add($outputParentAnchorLease)
        if (-not [string]::Equals(
            [string]$outputParentAnchorLease.path,
            $outputParent.TrimEnd('\'),
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
            $directoryLeases.Add((Enter-SmStableDirectoryLease `
                -Path $outputParent `
                -RequireNewLeaf `
                -ErrorCode 'PACKAGE_ACCEPTANCE_OUTPUT_LEASE_FAILED'))
        }
        Assert-NoReparsePath -Path $OutputFull
        if (Test-Path -LiteralPath $OutputFull) {
            throw 'PACKAGE_ACCEPTANCE_OUTPUT_ALREADY_EXISTS'
        }
    }
    [void]@(Get-LeasedTreeFiles -Root $ReleaseRoot -LeasedDirectories $releaseTreeDirectories); Assert-NoReparsePath -Path $WorkRoot
    if (Test-Path -LiteralPath $WorkRoot) { throw "WorkRoot already exists: $WorkRoot" }
    $directoryLeases.Add((Enter-SmStableDirectoryLease `
        -Path $WorkRoot `
        -RequireNewLeaf `
        -ErrorCode 'PACKAGE_ACCEPTANCE_WORK_ROOT_LEASE_FAILED'))

$installerRoot = Join-Path $ReleaseRoot 'offline-installer'
$installRoot = Join-Path $WorkRoot 'install-root'
$installerManifestPath = Join-Path $installerRoot 'installer-manifest.json'
$payloadManifestPath = Join-Path $installerRoot 'payload\payload-manifest.json'
$pluginRoot = Join-Path $installerRoot 'plugin\semantic-memory'
$releaseManifestPath = Join-Path $ReleaseRoot 'release-manifest.json'
$checksumsPath = Join-Path $ReleaseRoot 'checksums.sha256'
$installerManifest = Get-Content -Raw -Encoding UTF8 -LiteralPath $installerManifestPath | ConvertFrom-Json
$payloadManifest = Get-Content -Raw -Encoding UTF8 -LiteralPath $payloadManifestPath | ConvertFrom-Json
$releaseManifest = Get-Content -Raw -Encoding UTF8 -LiteralPath $releaseManifestPath | ConvertFrom-Json
$pluginManifest = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $pluginRoot '.codex-plugin\plugin.json') | ConvertFrom-Json
$checks = [ordered]@{}
$checks.release_exe = Test-Path -LiteralPath (Join-Path $ReleaseRoot 'semantic-memory-mcp.exe')
$checks.manager_exe = Test-Path -LiteralPath (Join-Path $ReleaseRoot 'manager-portable\semantic-memory-manager.exe')
$checks.portable_zip = (Get-ChildItem -LiteralPath $ReleaseRoot -Filter 'semantic-memory-manager-portable-*.zip' | Measure-Object).Count -eq 1
$checks.installer_zip = (Get-ChildItem -LiteralPath $ReleaseRoot -Filter 'semantic-memory-mcp-offline-installer-*.zip' | Measure-Object).Count -eq 1
$checks.stage14_installer_manifest = $installerManifest.schema -eq 'stage14-offline-installer-manifest/v1'
$checks.stage14_payload_manifest = $payloadManifest.schema -eq 'stage14-payload-manifest/v1'
$checks.stage14_release_manifest = $releaseManifest.schema -eq 'stage14-release-manifest/v1'
$checks.release_manifest_bound = (
    $releaseManifest.release_exe.sha256 -eq (Get-FileSha256 -Path (Join-Path $ReleaseRoot 'semantic-memory-mcp.exe')) -and
    $releaseManifest.stage14_installer.manifest.sha256 -eq (Get-FileSha256 -Path $installerManifestPath) -and
    $releaseManifest.stage14_installer.payload_manifest.sha256 -eq (Get-FileSha256 -Path $payloadManifestPath)
)
$checks.payload_manifest_bound = (Get-FileSha256 -Path $payloadManifestPath) -eq $installerManifest.payload.manifest_sha256
$checks.payload_records_bound = Test-ManifestRecords -Root (Join-Path $installerRoot 'payload') -Records $payloadManifest.files
$checks.installer_script_records_bound = Test-ManifestRecords -Root $installerRoot -Records $installerManifest.scripts
$checks.plugin_records_bound = Test-ManifestRecords -Root $installerRoot -Records $installerManifest.personal_plugin.files
$checksumBytes = [System.IO.File]::ReadAllBytes($checksumsPath)
$checks.checksums_utf8_no_bom = -not (
    $checksumBytes.Length -ge 3 -and
    $checksumBytes[0] -eq 0xEF -and
    $checksumBytes[1] -eq 0xBB -and
    $checksumBytes[2] -eq 0xBF
)
$checksumText = [System.Text.Encoding]::UTF8.GetString($checksumBytes)
$checksumLines = @($checksumText -split "`r?`n" | Where-Object { $_ })
$checksumRecords = @{}
$checks.checksums_parse = $true
$checks.checksums_all_bound = $true
foreach ($line in $checksumLines) {
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') {
        $checks.checksums_parse = $false
        $checks.checksums_all_bound = $false
        continue
    }
    $expectedSha256 = $Matches[1]
    $relative = $Matches[2]
    if ([System.IO.Path]::IsPathRooted($relative) -or $relative.Contains('..') -or
        $relative.Contains('\') -or $checksumRecords.ContainsKey($relative)) {
        $checks.checksums_parse = $false
        $checks.checksums_all_bound = $false
        continue
    }
    $checksumRecords[$relative] = $expectedSha256
    $boundPath = [System.IO.Path]::GetFullPath((Join-Path $ReleaseRoot $relative))
    $releasePrefix = [System.IO.Path]::GetFullPath($ReleaseRoot).TrimEnd('\') + '\'
    if (-not $boundPath.StartsWith($releasePrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $boundPath -PathType Leaf) -or
        (Get-FileSha256 -Path $boundPath) -ne $expectedSha256) {
        $checks.checksums_all_bound = $false
    }
}
$expectedChecksumPaths = @(
    Get-LeasedTreeFiles `
        -Root $ReleaseRoot `
        -LeasedDirectories $releaseTreeDirectories |
        Where-Object { $_.Name -notin @('checksums.sha256','release-manifest.json') } |
        ForEach-Object { $_.FullName.Substring($ReleaseRoot.Length).TrimStart('\').Replace('\','/') } |
        Sort-Object
)
$actualChecksumPaths = @($checksumRecords.Keys | Sort-Object)
$checks.checksums_file_set_exact = (
    [string]::Join("`n", $expectedChecksumPaths) -eq [string]::Join("`n", $actualChecksumPaths)
)
$checks.payload_entrypoints_exact = (
    $payloadManifest.entrypoints.mcp -eq 'semantic-memory-mcp.exe' -and
    $payloadManifest.entrypoints.hook -eq 'semantic-memory-hook.exe' -and
    $payloadManifest.entrypoints.manager -eq 'semantic-memory-manager.exe'
)
$checks.v2_scripts_present = -not (@('Install-SemanticMemoryV2.ps1','Install-SemanticMemoryPlugin.ps1','SemanticMemory.Common.psm1','semantic-memory-launcher.ps1','Invoke-Stage14Migration.ps1','Repair-SemanticMemory.ps1') | Where-Object { -not (Test-Path -LiteralPath (Join-Path $installerRoot $_) -PathType Leaf) })
$checks.legacy_installer_absent = -not (Test-Path -LiteralPath (Join-Path $installerRoot 'Install-SemanticMemory.ps1'))
$checks.personal_plugin_complete = (
    (Test-Path -LiteralPath (Join-Path $pluginRoot '.codex-plugin\plugin.json') -PathType Leaf) -and
    -not (Test-Path -LiteralPath (Join-Path $pluginRoot '.mcp.json')) -and
    $pluginManifest.PSObject.Properties.Name -notcontains 'mcpServers' -and
    @($pluginManifest.interface.capabilities) -notcontains 'Local MCP' -and
    (Test-Path -LiteralPath (Join-Path $pluginRoot 'hooks\hooks.json') -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $pluginRoot 'skills\neuroplastic-memory\SKILL.md') -PathType Leaf)
)

$releaseVersion = [string]$releaseManifest.version
$releasePublicVersion = $(if ($releaseVersion.StartsWith('v')) {
    $releaseVersion.Substring(1)
} else {
    $releaseVersion
})
$expectedPluginVersionPattern = '^{0}\+codex\.[0-9]+$' -f [regex]::Escape($releasePublicVersion)
$expectedReleaseProperties = @(
    'schema','product','version','public_version','release_channel','created_at',
    'build_id','version_file','source_server_json','source_manifest_sha256',
    'production_provenance','toolchain','candidate_exe','release_exe','manager_exe','portable_archive',
    'offline_installer','stage14_installer','personal_plugin',
    'authenticode_status','public_release_ready','real_second_machine_accepted',
    'files'
) | Sort-Object
$checks.release_manifest_top_level_exact = Test-ExactStringSequence `
    -Left @($releaseManifest.PSObject.Properties.Name | Sort-Object) `
    -Right $expectedReleaseProperties
$checks.release_manifest_version_contract = (
    $releaseVersion -match '^v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$' -and
    [string]$releaseManifest.product -ceq 'semantic-memory-mcp' -and
    [string]$releaseManifest.public_version -ceq $releasePublicVersion -and
    [string]$installerManifest.version -ceq $releaseVersion -and
    [string]$payloadManifest.version -ceq $releaseVersion -and
    [string]$pluginManifest.version -cmatch $expectedPluginVersionPattern -and
    [string]$releaseManifest.release_channel -ceq $(if (
        $releaseVersion.EndsWith('-rc.1')
    ) { 'rc' } else { 'final' }) -and
    [string]$releaseManifest.created_at -match
        '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$' -and
    [string]$releaseManifest.build_id -match '^[A-Za-z0-9._+-]+$'
)
$expectedVersionSha256 = Get-BytesSha256 -Bytes (
    [System.Text.Encoding]::ASCII.GetBytes($releaseVersion + "`n")
)
$checks.release_manifest_source_bindings = (
    [string]$releaseManifest.version_file.path -ceq 'VERSION' -and
    [string]$releaseManifest.version_file.sha256 -ceq $expectedVersionSha256 -and
    [string]$releaseManifest.source_server_json.path -ceq 'server.json' -and
    [string]$releaseManifest.source_server_json.sha256 -ceq (
        Get-FileSha256 -Path (Join-Path $ReleaseRoot 'server.json')
    ) -and
    [string]$releaseManifest.source_manifest_sha256 -cmatch '^[0-9a-f]{64}$'
)
$expectedProvenanceProperties = @(
    'schema','candidate_exe_sha256','source_manifest_sha256',
    'production_build_summary_sha256','source_manifest',
    'production_build_summary'
) | Sort-Object
$expectedProvenanceWitnessProperties = @('path','bytes','sha256') | Sort-Object
$sourceManifestWitness = $releaseManifest.production_provenance.source_manifest
$productionBuildSummaryWitness = `
    $releaseManifest.production_provenance.production_build_summary
$sourceManifestWitnessPath = [string]$sourceManifestWitness.path
$productionBuildSummaryWitnessPath = [string]$productionBuildSummaryWitness.path
$checks.release_manifest_production_provenance = (
    (Test-ExactStringSequence `
        -Left @($releaseManifest.production_provenance.PSObject.Properties.Name | Sort-Object) `
        -Right $expectedProvenanceProperties) -and
    [string]$releaseManifest.production_provenance.schema -ceq
        'stage14-production-provenance/v2' -and
    [string]$releaseManifest.production_provenance.candidate_exe_sha256 -cmatch
        '^[0-9a-f]{64}$' -and
    [string]$releaseManifest.production_provenance.candidate_exe_sha256 -ceq
        (Get-FileSha256 -Path (Join-Path $ReleaseRoot 'semantic-memory-mcp.exe')) -and
    [string]$releaseManifest.production_provenance.candidate_exe_sha256 -ceq
        [string]$releaseManifest.candidate_exe.sha256 -and
    [string]$releaseManifest.production_provenance.source_manifest_sha256 -ceq
        [string]$releaseManifest.source_manifest_sha256 -and
    [string]$releaseManifest.production_provenance.production_build_summary_sha256 `
        -cmatch '^[0-9a-f]{64}$' -and
    (Test-ExactStringSequence `
        -Left @($sourceManifestWitness.PSObject.Properties.Name | Sort-Object) `
        -Right $expectedProvenanceWitnessProperties) -and
    (Test-ExactStringSequence `
        -Left @($productionBuildSummaryWitness.PSObject.Properties.Name | Sort-Object) `
        -Right $expectedProvenanceWitnessProperties) -and
    [System.IO.Path]::IsPathRooted($sourceManifestWitnessPath) -and
    -not [string]::IsNullOrWhiteSpace(
        [System.IO.Path]::GetFileName($sourceManifestWitnessPath)
    ) -and
    [System.IO.Path]::IsPathRooted($productionBuildSummaryWitnessPath) -and
    -not [string]::IsNullOrWhiteSpace(
        [System.IO.Path]::GetFileName($productionBuildSummaryWitnessPath)
    ) -and
    (Test-NonnegativeJsonInteger -Value $sourceManifestWitness.bytes) -and
    (Test-NonnegativeJsonInteger -Value $productionBuildSummaryWitness.bytes) -and
    [string]$sourceManifestWitness.sha256 -ceq
        [string]$releaseManifest.production_provenance.source_manifest_sha256 -and
    [string]$productionBuildSummaryWitness.sha256 -ceq
        [string]$releaseManifest.production_provenance.production_build_summary_sha256
)
$expectedToolchainProperties = @(
    'schema','target_os','target_arch','packaging_runtime','manifest_runtime',
    'archive_format','package_script_sha256','manifest_generator_sha256'
) | Sort-Object
$checks.release_manifest_toolchain = (
    (Test-ExactStringSequence `
        -Left @($releaseManifest.toolchain.PSObject.Properties.Name | Sort-Object) `
        -Right $expectedToolchainProperties) -and
    [string]$releaseManifest.toolchain.schema -ceq 'stage14-packaging-toolchain/v1' -and
    [string]$releaseManifest.toolchain.target_os -ceq 'windows' -and
    [string]$releaseManifest.toolchain.target_arch -ceq 'x86_64' -and
    [string]$releaseManifest.toolchain.packaging_runtime -ceq 'PowerShell 5.1' -and
    [string]$releaseManifest.toolchain.manifest_runtime -ceq 'Python 3' -and
    [string]$releaseManifest.toolchain.archive_format -ceq 'zip' -and
    [string]$releaseManifest.toolchain.package_script_sha256 -cmatch '^[0-9a-f]{64}$' -and
    [string]$releaseManifest.toolchain.manifest_generator_sha256 -cmatch '^[0-9a-f]{64}$' -and
    [string]$releaseManifest.toolchain.package_script_sha256 -ceq
        [string]$installerManifest.toolchain.package_script_sha256 -and
    [string]$releaseManifest.toolchain.manifest_generator_sha256 -ceq
        [string]$installerManifest.toolchain.manifest_generator_sha256 -and
    [string]$installerManifest.toolchain.schema -ceq
        'stage14-packaging-toolchain/v1' -and
    [string]$installerManifest.toolchain.target_os -ceq 'windows' -and
    [string]$installerManifest.toolchain.target_arch -ceq 'x86_64'
)
$checks.release_manifest_binary_records = (
    (Test-NonnegativeJsonInteger -Value $releaseManifest.candidate_exe.bytes) -and
    [int64]$releaseManifest.candidate_exe.bytes -eq
        (Get-Item -LiteralPath (Join-Path $ReleaseRoot 'semantic-memory-mcp.exe')).Length -and
    [string]$releaseManifest.candidate_exe.sha256 -ceq
        (Get-FileSha256 -Path (Join-Path $ReleaseRoot 'semantic-memory-mcp.exe')) -and
    (Test-SingleFileRecord `
        -Root $ReleaseRoot `
        -Record $releaseManifest.release_exe `
        -ExpectedPath 'semantic-memory-mcp.exe') -and
    (Test-SingleFileRecord `
        -Root $ReleaseRoot `
        -Record $releaseManifest.manager_exe `
        -ExpectedPath 'manager-portable/semantic-memory-manager.exe')
)
$portableArchiveRelative = "semantic-memory-manager-portable-$releaseVersion.zip"
$installerArchiveRelative = "semantic-memory-mcp-offline-installer-$releaseVersion.zip"
$checks.release_manifest_archive_records = (
    (Test-SingleFileRecord `
        -Root $ReleaseRoot `
        -Record $releaseManifest.portable_archive `
        -ExpectedPath $portableArchiveRelative) -and
    (Test-SingleFileRecord `
        -Root $ReleaseRoot `
        -Record $releaseManifest.offline_installer `
        -ExpectedPath $installerArchiveRelative)
)
$requiredScripts = @(
    'Install-SemanticMemoryV2.ps1',
    'Install-SemanticMemoryPlugin.ps1',
    'SemanticMemory.Common.psm1',
    'semantic-memory-launcher.ps1',
    'Invoke-Stage14Migration.ps1',
    'Repair-SemanticMemory.ps1',
    'Invoke-PackageAcceptance.ps1',
    'README.zh-CN.md',
    'Install Semantic Memory.cmd'
)
$releaseScriptPaths = @(
    @($releaseManifest.stage14_installer.scripts) |
        ForEach-Object { [string]$_.path }
)
$expectedReleaseScriptPaths = @(
    $requiredScripts | ForEach-Object { "offline-installer/$_" }
)
$checks.release_manifest_installer_contract = (
    (Test-SingleFileRecord `
        -Root $ReleaseRoot `
        -Record $releaseManifest.stage14_installer.manifest `
        -ExpectedPath 'offline-installer/installer-manifest.json') -and
    (Test-SingleFileRecord `
        -Root $ReleaseRoot `
        -Record $releaseManifest.stage14_installer.payload_manifest `
        -ExpectedPath 'offline-installer/payload/payload-manifest.json') -and
    [string]$releaseManifest.stage14_installer.payload_version_id -ceq
        [string]$payloadManifest.version_id -and
    (Test-ExactStringSequence `
        -Left @(
            [string]$releaseManifest.stage14_installer.entrypoints.mcp,
            [string]$releaseManifest.stage14_installer.entrypoints.hook,
            [string]$releaseManifest.stage14_installer.entrypoints.manager
        ) `
        -Right @(
            'semantic-memory-mcp.exe',
            'semantic-memory-hook.exe',
            'semantic-memory-manager.exe'
        )) -and
    (Test-ManifestRecords `
        -Root $ReleaseRoot `
        -Records $releaseManifest.stage14_installer.scripts) -and
    (Test-ExactStringSequence -Left $releaseScriptPaths -Right $expectedReleaseScriptPaths)
)
$expectedCliCommands = @(
    @('plugin','marketplace','add','<personal-marketplace-path>'),
    @('plugin','add','semantic-memory@personal')
)
$installerPluginTransaction = $installerManifest.personal_plugin.transaction
$releasePluginTransaction = $releaseManifest.personal_plugin.transaction
$checks.release_manifest_plugin_transaction = (
    [string]$releaseManifest.personal_plugin.root -ceq
        'offline-installer/plugin/semantic-memory' -and
    [string]$releaseManifest.personal_plugin.mcp_registration -ceq
        'user_managed_config_only' -and
    -not [bool]$releaseManifest.personal_plugin.plugin_mcp_config_present -and
    (Test-SingleFileRecord `
        -Root $ReleaseRoot `
        -Record $releaseManifest.personal_plugin.plugin_manifest `
        -ExpectedPath 'offline-installer/plugin/semantic-memory/.codex-plugin/plugin.json') -and
    (Test-SingleFileRecord `
        -Root $ReleaseRoot `
        -Record $releasePluginTransaction.script `
        -ExpectedPath 'offline-installer/Install-SemanticMemoryPlugin.ps1') -and
    [string]$releasePluginTransaction.schema -ceq
        'stage14-personal-plugin-transaction/v1' -and
    (Test-ExactStringSequence `
        -Left @($releasePluginTransaction.actions) `
        -Right @('Preview','Apply','Verify','Rollback','Recover')) -and
    (Test-ExactStringSequence `
        -Left @($releasePluginTransaction.cas_preconditions) `
        -Right @(
            'ExpectedSourceTreeSha256',
            'ExpectedCacheTreeSha256',
            'ExpectedMarketplaceSha256'
        )) -and
    [string]$releasePluginTransaction.absent_sentinel -ceq 'ABSENT' -and
    [bool]$releasePluginTransaction.duplicate_mcp_registration_forbidden -and
    (Test-ExactStringSequence `
        -Left @($releasePluginTransaction.cli_commands | ForEach-Object {
            [string]::Join("`0", @($_))
        }) `
        -Right @($expectedCliCommands | ForEach-Object {
            [string]::Join("`0", @($_))
        })) -and
    [string]$installerPluginTransaction.script -ceq
        'Install-SemanticMemoryPlugin.ps1' -and
    [string]$installerPluginTransaction.schema -ceq
        'stage14-personal-plugin-transaction/v1' -and
    [bool]$installerPluginTransaction.duplicate_mcp_registration_forbidden
)
$releasePluginPaths = @(
    @($releaseManifest.personal_plugin.files) | ForEach-Object { [string]$_.path }
)
$expectedReleasePluginPaths = @(
    Get-RelativeFilePaths `
        -Root $pluginRoot `
        -LeasedDirectories $releaseTreeDirectories |
        ForEach-Object { "offline-installer/plugin/semantic-memory/$_" }
)
$checks.release_manifest_plugin_file_set = (
    (Test-ManifestRecords `
        -Root $ReleaseRoot `
        -Records $releaseManifest.personal_plugin.files) -and
    (Test-ExactStringSequence `
        -Left $releasePluginPaths `
        -Right $expectedReleasePluginPaths) -and
    (Test-SingleFileRecord `
        -Root $ReleaseRoot `
        -Record $releaseManifest.personal_plugin.hooks `
        -ExpectedPath 'offline-installer/plugin/semantic-memory/hooks/hooks.json')
)
$releaseManifestPaths = @(
    @($releaseManifest.files) | ForEach-Object { [string]$_.path } | Sort-Object
)
$actualReleasePaths = @(
    Get-RelativeFilePaths `
        -Root $ReleaseRoot `
        -LeasedDirectories $releaseTreeDirectories |
        Where-Object { $_ -cne 'release-manifest.json' }
)
$checks.release_manifest_file_set_exact = (
    (Test-ManifestRecords -Root $ReleaseRoot -Records $releaseManifest.files) -and
    (Test-ExactStringSequence -Left $releaseManifestPaths -Right $actualReleasePaths)
)
$checks.release_manifest_release_flags = (
    [string]$releaseManifest.authenticode_status -ceq 'not_signed' -and
    -not [bool]$releaseManifest.public_release_ready -and
    -not [bool]$releaseManifest.real_second_machine_accepted
)
$portableExtract = Join-Path $WorkRoot 'archive-portable-extracted'
$installerExtract = Join-Path $WorkRoot 'archive-installer-extracted'
$checks.release_manifest_portable_archive_source_exact = Test-ZipMatchesTree `
    -Archive (Join-Path $ReleaseRoot $portableArchiveRelative) `
    -SourceRoot (Join-Path $ReleaseRoot 'manager-portable') `
    -ExtractRoot $portableExtract `
    -SourceLeasedDirectories $releaseTreeDirectories
$checks.release_manifest_installer_archive_source_exact = Test-ZipMatchesTree `
    -Archive (Join-Path $ReleaseRoot $installerArchiveRelative) `
    -SourceRoot $installerRoot `
    -ExtractRoot $installerExtract `
    -SourceLeasedDirectories $releaseTreeDirectories

$manifestGateNames = @(
    'release_exe','manager_exe','portable_zip','installer_zip',
    'stage14_installer_manifest','stage14_payload_manifest',
    'stage14_release_manifest','release_manifest_bound',
    'payload_manifest_bound','payload_records_bound',
    'installer_script_records_bound','plugin_records_bound',
    'checksums_utf8_no_bom','checksums_parse','checksums_all_bound',
    'checksums_file_set_exact','payload_entrypoints_exact',
    'v2_scripts_present','legacy_installer_absent','personal_plugin_complete',
    'release_manifest_top_level_exact','release_manifest_version_contract',
    'release_manifest_source_bindings','release_manifest_production_provenance',
    'release_manifest_toolchain',
    'release_manifest_binary_records','release_manifest_archive_records',
    'release_manifest_installer_contract',
    'release_manifest_plugin_transaction',
    'release_manifest_plugin_file_set','release_manifest_file_set_exact',
    'release_manifest_release_flags',
    'release_manifest_portable_archive_source_exact',
    'release_manifest_installer_archive_source_exact'
)
$manifestGatePass = $true
foreach ($name in $manifestGateNames) {
    if (-not [bool]$checks[$name]) { $manifestGatePass = $false }
}
if (-not $manifestGatePass) {
    $early = [ordered]@{
        schema = 'stage14-package-acceptance/v1'
        status = 'FAIL_STAGE14_PACKAGE_ACCEPTANCE_MANIFEST_GATE'
        release_root_sha256 = $(if (
            Test-Path -LiteralPath (Join-Path $ReleaseRoot 'semantic-memory-mcp.exe') -PathType Leaf
        ) {
            Get-FileSha256 -Path (Join-Path $ReleaseRoot 'semantic-memory-mcp.exe')
        } else {
            $null
        })
        checks = $checks
    }
    if ($OutputFull) {
        Write-PackageAcceptanceResultCreateNew -Path $OutputFull -Value $early
    }
    $early | ConvertTo-Json -Depth 12
    exit 1
}

$pluginUserHome = Join-Path $WorkRoot 'isolated-plugin-user'
$pluginCodexHome = Join-Path $pluginUserHome '.codex'
$pluginStateRoot = Join-Path $pluginUserHome 'AppData\Local\SemanticMemory\backups\codex-plugin'
$pluginMarketplacePath = Join-Path $pluginUserHome '.agents\plugins\marketplace.json'
$pluginMarketplaceParent = Split-Path -Parent $pluginMarketplacePath
$pluginMockCli = Join-Path $WorkRoot 'mock-codex.cmd'
$pluginMockLog = Join-Path $WorkRoot 'mock-codex-commands.log'
$pluginSecretSentinel = 'stage14-plugin-third-party-sentinel-must-not-leak'
New-Item -ItemType Directory -Path $pluginMarketplaceParent | Out-Null
$pluginMarketplaceBeforeText = @"
{
  "name": "personal",
  "interface": {
    "displayName": "Personal",
    "thirdPartyUi": "preserve"
  },
  "thirdPartyTop": {
    "value": "$pluginSecretSentinel"
  },
  "plugins": [
    {
      "name": "other-plugin",
      "source": {
        "source": "local",
        "path": "./plugins/other"
      },
      "thirdPartyPluginField": "preserve"
    }
  ]
}
"@
[System.IO.File]::WriteAllText(
    $pluginMarketplacePath,
    $pluginMarketplaceBeforeText,
    [System.Text.UTF8Encoding]::new($false)
)
$pluginMarketplaceBeforeSha256 = Get-FileSha256 -Path $pluginMarketplacePath
[System.IO.File]::WriteAllText(
    $pluginMockCli,
    "@echo off`r`nif `"%SM_PLUGIN_MOCK_LOG%`"==`"`" exit /b 23`r`necho %*>>`"%SM_PLUGIN_MOCK_LOG%`"`r`nexit /b 0`r`n",
    [System.Text.Encoding]::ASCII
)
$pluginPreview = Invoke-PluginTransaction `
    -Action 'Preview' `
    -PackageRoot $installerRoot `
    -UserHome $pluginUserHome `
    -CodexHome $pluginCodexHome `
    -StateRoot $pluginStateRoot
$checks.plugin_transaction_preview_zero_write = (
    $pluginPreview.value.status -eq 'RED_PLUGIN_DRIFT' -and
    -not [bool]$pluginPreview.value.write_performed -and
    -not (Test-Path -LiteralPath $pluginStateRoot) -and
    (Get-FileSha256 -Path $pluginMarketplacePath) -eq $pluginMarketplaceBeforeSha256
)
$priorPluginMockLog = $env:SM_PLUGIN_MOCK_LOG
$env:SM_PLUGIN_MOCK_LOG = $pluginMockLog
try {
    $pluginApply = Invoke-PluginTransaction `
        -Action 'Apply' `
        -PackageRoot $installerRoot `
        -UserHome $pluginUserHome `
        -CodexHome $pluginCodexHome `
        -StateRoot $pluginStateRoot `
        -ExtraArguments @(
            '-ExpectedSourceTreeSha256','ABSENT',
            '-ExpectedCacheTreeSha256','ABSENT',
            '-ExpectedMarketplaceSha256',$pluginMarketplaceBeforeSha256,
            '-ConfirmUserMutation',
            '-InvokeCodexCli',
            '-CodexCliPath',$pluginMockCli
        )
} finally {
    $env:SM_PLUGIN_MOCK_LOG = $priorPluginMockLog
}
$checks.plugin_transaction_apply_verified = (
    $pluginApply.value.status -eq 'APPLIED_VERIFIED' -and
    [bool]$pluginApply.value.write_performed -and
    [bool]$pluginApply.value.cli_invoked -and
    [bool]$pluginApply.value.third_party_projection_preserved
)
$pluginVerify = Invoke-PluginTransaction `
    -Action 'Verify' `
    -PackageRoot $installerRoot `
    -UserHome $pluginUserHome `
    -CodexHome $pluginCodexHome `
    -StateRoot $pluginStateRoot
$pluginMarketplaceAfterText = Get-Content -Raw -Encoding UTF8 -LiteralPath $pluginMarketplacePath
$checks.plugin_transaction_verify_and_preserve = (
    $pluginVerify.value.status -eq 'PASS' -and
    [bool]$pluginVerify.value.source_hash_match -and
    [bool]$pluginVerify.value.cache_hash_match -and
    [bool]$pluginVerify.value.marketplace_unique -and
    $pluginMarketplaceAfterText.Contains($pluginSecretSentinel) -and
    $pluginMarketplaceAfterText.Contains('other-plugin') -and
    $pluginMarketplaceAfterText.Contains('thirdPartyPluginField') -and
    -not (Test-Path -LiteralPath (Join-Path $pluginUserHome 'plugins\semantic-memory\.mcp.json')) -and
    @(Get-ChildItem -LiteralPath (Join-Path $pluginCodexHome 'plugins\cache\personal\semantic-memory') -Recurse -Filter '.mcp.json' -File -ErrorAction SilentlyContinue).Count -eq 0
)
$pluginOriginalBackupPath = [string]$pluginApply.value.marketplace_original_backup_path
$checks.plugin_transaction_original_bytes_backed_up = (
    $pluginOriginalBackupPath -and
    (Test-Path -LiteralPath $pluginOriginalBackupPath -PathType Leaf) -and
    (Get-FileSha256 -Path $pluginOriginalBackupPath) -eq $pluginMarketplaceBeforeSha256
)
$pluginTransactionCount = @(
    Get-ChildItem -LiteralPath $pluginStateRoot -Directory -ErrorAction SilentlyContinue
).Count
$pluginJournalPath = [string]$pluginApply.value.journal_path
$pluginJournalBeforeReplay = Get-FileSha256 -Path $pluginJournalPath
$pluginMockBeforeReplay = Get-FileSha256 -Path $pluginMockLog
$pluginReplay = Invoke-PluginTransaction `
    -Action 'Apply' `
    -PackageRoot $installerRoot `
    -UserHome $pluginUserHome `
    -CodexHome $pluginCodexHome `
    -StateRoot $pluginStateRoot `
    -ExtraArguments @(
        '-ExpectedSourceTreeSha256',[string]$pluginVerify.value.states.source.tree_sha256,
        '-ExpectedCacheTreeSha256',[string]$pluginVerify.value.states.cache.tree_sha256,
        '-ExpectedMarketplaceSha256',[string]$pluginVerify.value.states.marketplace.sha256,
        '-ConfirmUserMutation',
        '-InvokeCodexCli',
        '-CodexCliPath',$pluginMockCli
    )
$checks.plugin_transaction_replay_zero_write = (
    $pluginReplay.value.status -eq 'REPLAYED_ZERO_WRITE' -and
    -not [bool]$pluginReplay.value.write_performed -and
    -not [bool]$pluginReplay.value.cli_invoked -and
    @(
        Get-ChildItem -LiteralPath $pluginStateRoot -Directory -ErrorAction SilentlyContinue
    ).Count -eq $pluginTransactionCount -and
    (Get-FileSha256 -Path $pluginJournalPath) -eq $pluginJournalBeforeReplay -and
    (Get-FileSha256 -Path $pluginMockLog) -eq $pluginMockBeforeReplay
)
$pluginDescriptorText = Get-Content -Raw -Encoding UTF8 -LiteralPath (
    [string]$pluginApply.value.transaction_path
)
$pluginJournalText = Get-Content -Raw -Encoding UTF8 -LiteralPath $pluginJournalPath
$checks.plugin_transaction_receipts_redacted = (
    -not $pluginPreview.text.Contains($pluginSecretSentinel) -and
    -not $pluginApply.text.Contains($pluginSecretSentinel) -and
    -not $pluginVerify.text.Contains($pluginSecretSentinel) -and
    -not $pluginReplay.text.Contains($pluginSecretSentinel) -and
    -not $pluginDescriptorText.Contains($pluginSecretSentinel) -and
    -not $pluginJournalText.Contains($pluginSecretSentinel)
)
$checks.no_workspace_path_in_release_text = $true
foreach ($file in @(Get-LeasedTreeFiles `
    -Root $ReleaseRoot `
    -LeasedDirectories $releaseTreeDirectories)) {
    if ($file.Extension -in @('.json','.txt','.md','.cmd','.ps1','.sha256')) {
        $text = Get-Content -Raw -LiteralPath $file.FullName
        if ($text -match 'H:\\Codex_H' -or $text -match 'C:\\Users\\ASUS') {
            $checks.no_workspace_path_in_release_text = $false
        }
    }
}

Invoke-Installer -Action 'Install' -PackageRoot $installerRoot -InstallRoot $installRoot | Out-Null
Invoke-Installer -Action 'Verify' -PackageRoot $installerRoot -InstallRoot $installRoot | Out-Null
$checks.clean_install_verify = $true
$pointerPath = Join-Path $installRoot 'state\current.json'
$pointerBytes = [System.IO.File]::ReadAllBytes($pointerPath)
$pointer = Get-Content -Raw -Encoding UTF8 -LiteralPath $pointerPath | ConvertFrom-Json
$payloadRoot = Join-Path $installRoot ("app\versions\{0}" -f $pointer.version_id)
$receiptPath = Join-Path $payloadRoot 'verification-receipt.json'
$checks.pointer_receipt_bound = (
    $pointer.schema -eq 'stage14-current-pointer/v1' -and
    (Get-FileSha256 -Path (Join-Path $payloadRoot 'payload-manifest.json')) -eq $pointer.manifest_sha256 -and
    (Get-FileSha256 -Path $receiptPath) -eq $pointer.receipt_sha256
)
$checks.stable_native_bin = -not (@('semantic-memory-mcp.exe','semantic-memory-hook.exe','semantic-memory-manager.exe') | Where-Object { -not (Test-Path -LiteralPath (Join-Path $installRoot ("bin\{0}" -f $_)) -PathType Leaf) })
$stableLauncherPath = Join-Path $installRoot 'bin\semantic-memory-launcher.ps1'
$packageLauncherPath = Join-Path $installerRoot 'semantic-memory-launcher.ps1'
$checks.stable_launcher = (
    (Test-Path -LiteralPath $stableLauncherPath -PathType Leaf) -and
    (Test-Path -LiteralPath $packageLauncherPath -PathType Leaf) -and
    (Get-FileSha256 -Path $stableLauncherPath) -eq (Get-FileSha256 -Path $packageLauncherPath)
)
$managerWrapperPath = Join-Path $pluginRoot 'scripts\semantic-memory-manager.ps1'
$managerVerifyExitCode = -1
$managerVerifyText = ''
if (Test-Path -LiteralPath $managerWrapperPath -PathType Leaf) {
    $managerHomeWasPresent = Test-Path Env:SEMANTIC_MEMORY_HOME
    $managerHomeBefore = $env:SEMANTIC_MEMORY_HOME
    try {
        $env:SEMANTIC_MEMORY_HOME = $installRoot
        $managerVerifyOutput = & powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $managerWrapperPath -VerifyOnly 2>&1
        $managerVerifyExitCode = $LASTEXITCODE
        $managerVerifyText = (@($managerVerifyOutput | ForEach-Object { [string]$_ }) -join "`n").Trim()
    } finally {
        if ($managerHomeWasPresent) {
            $env:SEMANTIC_MEMORY_HOME = $managerHomeBefore
        } else {
            Remove-Item Env:SEMANTIC_MEMORY_HOME -ErrorAction SilentlyContinue
        }
    }
}
$checks.manager_plugin_verify_only = (
    $managerVerifyExitCode -eq 0 -and $managerVerifyText -eq 'PASS_FULL_SHA256'
)

$configRoot = Join-Path $WorkRoot 'isolated-codex-config'
$configPath = Join-Path $configRoot 'config.toml'
$configBackupRoot = Join-Path $installRoot 'backups\package-acceptance-config'
New-Item -ItemType Directory -Path $configRoot | Out-Null
$secretSentinel = 'stage14-package-secret-sentinel-must-not-leak'
$originalConfigText = (
    'model = "unmanaged-package-acceptance"' + "`r`n" +
    'third_party_api_key = "' + $secretSentinel + '"' + "`r`n"
)
[System.IO.File]::WriteAllText(
    $configPath,
    $originalConfigText,
    [System.Text.UTF8Encoding]::new($false, $true)
)
$originalConfigBytes = [System.IO.File]::ReadAllBytes($configPath)
$originalConfigSha256 = Get-FileSha256 -Path $configPath
$previewConfigState = Get-Item -LiteralPath $configPath
$preview = Invoke-ConfigRepair `
    -Mode 'Preview' `
    -PackageRoot $installerRoot `
    -ConfigPath $configPath `
    -InstallRoot $installRoot
$checks.config_preview_zero_write = (
    $preview.value.classification -eq 'RED_MANAGED_CONFIG_DRIFT' -and
    -not [bool]$preview.value.apply_performed -and
    (Get-FileSha256 -Path $configPath) -eq $originalConfigSha256 -and
    (Get-Item -LiteralPath $configPath).LastWriteTimeUtc.Ticks -eq
        $previewConfigState.LastWriteTimeUtc.Ticks -and
    -not (Test-Path -LiteralPath $configBackupRoot)
)
$apply = Invoke-ConfigRepair `
    -Mode 'Apply' `
    -PackageRoot $installerRoot `
    -ConfigPath $configPath `
    -InstallRoot $installRoot `
    -ExpectedConfigSha256 $originalConfigSha256 `
    -BackupRoot $configBackupRoot
$checks.config_apply_verified = (
    $apply.value.status -eq 'APPLIED_VERIFIED' -and
    [bool]$apply.value.write_performed -and
    [bool]$apply.value.unmanaged_bytes_preserved -and
    [bool]$apply.value.acl_preserved -and
    [bool]$apply.value.attributes_preserved
)
$afterConfigBytes = [System.IO.File]::ReadAllBytes($configPath)
$prefixPreserved = $afterConfigBytes.Length -ge $originalConfigBytes.Length
if ($prefixPreserved) {
    for ($index = 0; $index -lt $originalConfigBytes.Length; $index++) {
        if ($afterConfigBytes[$index] -ne $originalConfigBytes[$index]) {
            $prefixPreserved = $false
            break
        }
    }
}
$configText = [System.Text.UTF8Encoding]::new($false, $true).GetString($afterConfigBytes)
$checks.config_unmanaged_bytes_preserved = (
    $prefixPreserved -and
    $configText.Contains($secretSentinel) -and
    ([regex]::Matches($configText, '(?m)^\[mcp_servers\.semantic_memory\]\s*$')).Count -eq 1 -and
    ([regex]::Matches($configText, '(?m)^\[mcp_servers\.semantic_memory\.env\]\s*$')).Count -eq 1
)
$checks.config_original_backup_exact = (
    $apply.value.backup_path -and
    (Test-Path -LiteralPath ([string]$apply.value.backup_path) -PathType Leaf) -and
    (Get-FileSha256 -Path ([string]$apply.value.backup_path) -eq $originalConfigSha256)
)
$checks.config_sensitive_swap_removed = (
    [bool]$apply.value.swap_backup_removed -and
    -not (Test-Path -LiteralPath ([string]$apply.value.swap_backup_path))
)
$verify = Invoke-ConfigRepair `
    -Mode 'Verify' `
    -PackageRoot $installerRoot `
    -ConfigPath $configPath `
    -InstallRoot $installRoot
$checks.config_verify_pass = (
    $verify.value.status -eq 'PASS' -and
    [bool]$verify.value.managed_fields_match
)

$canaryAuthManifestPath = Join-Path $WorkRoot 'bounded-canary-authorization.json'
[System.IO.File]::WriteAllText(
    $canaryAuthManifestPath,
    "{`"schema`":`"stage14-package-acceptance-canary/v1`",`"authorized`":true}`n",
    [System.Text.UTF8Encoding]::new($false)
)
$canaryAuthSha256 = Get-FileSha256 -Path $canaryAuthManifestPath
$defaultConfigSha256 = Get-FileSha256 -Path $configPath
$canaryApply = Invoke-ConfigRepair `
    -Mode 'Apply' `
    -PackageRoot $installerRoot `
    -ConfigPath $configPath `
    -InstallRoot $installRoot `
    -ExpectedConfigSha256 $defaultConfigSha256 `
    -BackupRoot $configBackupRoot `
    -EnableProductionCanary `
    -CanaryAuthManifestPath $canaryAuthManifestPath `
    -CanaryAuthSha256 $canaryAuthSha256
$canaryConfigText = Get-Content -Raw -Encoding UTF8 -LiteralPath $configPath
$checks.config_canary_apply_exact = (
    $canaryApply.value.status -eq 'APPLIED_VERIFIED' -and
    [bool]$canaryApply.value.production_canary_enabled -and
    ([regex]::Matches($canaryConfigText, '(?m)^\s*CBM_STAGE14_[A-Z0-9_]+\s*=')).Count -eq 4 -and
    $canaryConfigText.Contains('CBM_STAGE14_PRODUCTION_GATE = "1"') -and
    $canaryConfigText.Contains('CBM_STAGE14_EVOLUTION_MODE = "bounded_canary"') -and
    $canaryConfigText.Contains(('CBM_STAGE14_CANARY_AUTH_SHA256 = "{0}"' -f $canaryAuthSha256)) -and
    $canaryConfigText.Contains('CBM_MEMORY_AUTO_MAINTAIN = "0"') -and
    $canaryConfigText.Contains('CBM_MEMORY_EMBED_BACKEND = "static"') -and
    -not $canaryConfigText.Contains('CBM_MEMORY_NO_GLOBAL_UNION')
)
$canaryVerify = Invoke-ConfigRepair `
    -Mode 'Verify' `
    -PackageRoot $installerRoot `
    -ConfigPath $configPath `
    -InstallRoot $installRoot `
    -EnableProductionCanary `
    -CanaryAuthManifestPath $canaryAuthManifestPath `
    -CanaryAuthSha256 $canaryAuthSha256
$checks.config_canary_verify_pass = (
    $canaryVerify.value.status -eq 'PASS' -and
    [bool]$canaryVerify.value.managed_fields_match -and
    [bool]$canaryVerify.value.production_canary_enabled
)

$canaryConfigSha256 = Get-FileSha256 -Path $configPath
$canaryRemoval = Invoke-ConfigRepair `
    -Mode 'Apply' `
    -PackageRoot $installerRoot `
    -ConfigPath $configPath `
    -InstallRoot $installRoot `
    -ExpectedConfigSha256 $canaryConfigSha256 `
    -BackupRoot $configBackupRoot
$removedCanaryText = Get-Content -Raw -Encoding UTF8 -LiteralPath $configPath
$checks.config_default_removes_canary_exact = (
    $canaryRemoval.value.status -eq 'APPLIED_VERIFIED' -and
    -not [bool]$canaryRemoval.value.production_canary_enabled -and
    -not $removedCanaryText.Contains('CBM_STAGE14_') -and
    $removedCanaryText.Contains('CBM_MEMORY_AUTO_MAINTAIN = "0"') -and
    $removedCanaryText.Contains('CBM_MEMORY_EMBED_BACKEND = "static"') -and
    -not $removedCanaryText.Contains('CBM_MEMORY_NO_GLOBAL_UNION')
)
$removedCanaryVerify = Invoke-ConfigRepair `
    -Mode 'Verify' `
    -PackageRoot $installerRoot `
    -ConfigPath $configPath `
    -InstallRoot $installRoot
$checks.config_default_after_canary_verify_pass = (
    $removedCanaryVerify.value.status -eq 'PASS' -and
    [bool]$removedCanaryVerify.value.managed_fields_match -and
    -not [bool]$removedCanaryVerify.value.production_canary_enabled
)

$replaySha256 = Get-FileSha256 -Path $configPath
$replayState = Get-Item -LiteralPath $configPath
$transactionCount = @(
    Get-ChildItem -LiteralPath $configBackupRoot -Directory -ErrorAction SilentlyContinue
).Count
$replay = Invoke-ConfigRepair `
    -Mode 'Apply' `
    -PackageRoot $installerRoot `
    -ConfigPath $configPath `
    -InstallRoot $installRoot `
    -ExpectedConfigSha256 $replaySha256 `
    -BackupRoot $configBackupRoot
$checks.config_replay_zero_write = (
    $replay.value.status -eq 'REPLAYED_ZERO_WRITE' -and
    -not [bool]$replay.value.write_performed -and
    (Get-FileSha256 -Path $configPath) -eq $replaySha256 -and
    (Get-Item -LiteralPath $configPath).LastWriteTimeUtc.Ticks -eq
        $replayState.LastWriteTimeUtc.Ticks -and
    @(
        Get-ChildItem -LiteralPath $configBackupRoot -Directory -ErrorAction SilentlyContinue
    ).Count -eq $transactionCount
)
$transactionJson = Get-Content -Raw -Encoding UTF8 -LiteralPath ([string]$apply.value.transaction_path)
$canaryTransactionJson = Get-Content -Raw -Encoding UTF8 -LiteralPath (
    [string]$canaryApply.value.transaction_path
)
$canaryRemovalTransactionJson = Get-Content -Raw -Encoding UTF8 -LiteralPath (
    [string]$canaryRemoval.value.transaction_path
)
$checks.config_receipts_redacted = (
    -not $preview.text.Contains($secretSentinel) -and
    -not $apply.text.Contains($secretSentinel) -and
    -not $verify.text.Contains($secretSentinel) -and
    -not $canaryApply.text.Contains($secretSentinel) -and
    -not $canaryVerify.text.Contains($secretSentinel) -and
    -not $canaryRemoval.text.Contains($secretSentinel) -and
    -not $removedCanaryVerify.text.Contains($secretSentinel) -and
    -not $replay.text.Contains($secretSentinel) -and
    -not $transactionJson.Contains($secretSentinel) -and
    -not $canaryTransactionJson.Contains($secretSentinel) -and
    -not $canaryRemovalTransactionJson.Contains($secretSentinel)
)

$tamperedPointer = [ordered]@{schema='tampered-current-pointer/v1';version_id=$pointer.version_id;manifest_sha256=$pointer.manifest_sha256;receipt_sha256=$pointer.receipt_sha256}
[System.IO.File]::WriteAllText($pointerPath, (($tamperedPointer | ConvertTo-Json -Compress) + "`n"), [System.Text.UTF8Encoding]::new($false))
try {
    Invoke-Installer -Action 'Verify' -PackageRoot $installerRoot -InstallRoot $installRoot | Out-Null
    $checks.pointer_tamper_detected = $false
} catch {
    $checks.pointer_tamper_detected = $true
}
[System.IO.File]::WriteAllBytes($pointerPath, $pointerBytes)

$stableMcp = Join-Path $installRoot 'bin\semantic-memory-mcp.exe'
$replacement = Join-Path $installRoot 'bin\semantic-memory-mcp.tampered'
[System.IO.File]::WriteAllText($replacement, 'corrupt', [System.Text.UTF8Encoding]::new($false))
Move-Item -LiteralPath $replacement -Destination $stableMcp -Force
try {
    Invoke-Installer -Action 'Verify' -PackageRoot $installerRoot -InstallRoot $installRoot | Out-Null
    $checks.stable_tamper_detected = $false
} catch {
    $checks.stable_tamper_detected = $true
}
Invoke-Installer -Action 'Repair' -PackageRoot $installerRoot -InstallRoot $installRoot | Out-Null
Invoke-Installer -Action 'Verify' -PackageRoot $installerRoot -InstallRoot $installRoot | Out-Null
$checks.repair_verify = $true

New-Item -ItemType Directory -Force -Path (Join-Path $installRoot 'data') | Out-Null
Set-Content -Encoding UTF8 -LiteralPath (Join-Path $installRoot 'data\retained.txt') -Value 'retain'
Invoke-Installer -Action 'Uninstall' -PackageRoot $installerRoot -InstallRoot $installRoot | Out-Null
$checks.uninstall_retains_data = (
    (Test-Path -LiteralPath (Join-Path $installRoot 'data\retained.txt')) -and
    -not (Test-Path -LiteralPath (Join-Path $installRoot 'app')) -and
    -not (Test-Path -LiteralPath (Join-Path $installRoot 'bin'))
)

$pass = $true
foreach ($value in $checks.Values) {
    if (-not $value) { $pass = $false }
}

$result = [ordered]@{
    schema = 'stage14-package-acceptance/v1'
    status = $(if ($pass) { 'PASS_STAGE14_PACKAGE_ACCEPTANCE' } else { 'FAIL_STAGE14_PACKAGE_ACCEPTANCE' })
    release_root_sha256 = Get-FileSha256 -Path (Join-Path $ReleaseRoot 'semantic-memory-mcp.exe')
    checks = $checks
}

if ($OutputFull) {
    if (Test-Path -LiteralPath $OutputFull) {
        throw 'PACKAGE_ACCEPTANCE_OUTPUT_ALREADY_EXISTS'
    }
    Write-PackageAcceptanceResultCreateNew -Path $OutputFull -Value $result
}
$result | ConvertTo-Json -Depth 8
} finally {
    for ($index = $directoryLeases.Count - 1; $index -ge 0; $index--) {
        Exit-SmStableDirectoryLease -Lease $directoryLeases[$index]
    }
}
if (-not $pass) { exit 1 }
