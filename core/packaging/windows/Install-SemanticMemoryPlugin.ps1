[CmdletBinding()]
param(
    [ValidateSet('Preview','Apply','Verify','Rollback','Recover')]
    [string]$Action = 'Preview',
    [Parameter(Mandatory=$true)][string]$PackageRoot,
    [string]$UserHome = $env:USERPROFILE,
    [string]$CodexHome,
    [string]$StateRoot,
    [AllowEmptyString()][string]$ExpectedMarketplaceSha256,
    [AllowEmptyString()][string]$ExpectedSourceTreeSha256,
    [AllowEmptyString()][string]$ExpectedCacheTreeSha256,
    [string]$TransactionPath,
    [string]$CodexCliPath,
    [switch]$InvokeCodexCli,
    [switch]$ConfirmUserMutation,
    [ValidateSet(
        'none',
        'after_source_swap',
        'after_cache_swap',
        'after_marketplace_swap',
        'after_cli',
        'delay_before_source_swap',
        'delay_before_cache_swap',
        'delay_before_marketplace_swap',
        'crash_after_source_displace',
        'crash_after_cache_displace',
        'crash_after_source_swap',
        'crash_after_cache_swap',
        'crash_after_marketplace_replace',
        'crash_after_marketplace_swap'
    )]
    [string]$FaultInjection = 'none'
)

$ErrorActionPreference = 'Stop'

function Get-SmPluginSha256File {
    param([Parameter(Mandatory=$true)][string]$Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Get-SmPluginSha256Bytes {
    param([Parameter(Mandatory=$true)][byte[]]$Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Get-SmPluginSha256Text {
    param([Parameter(Mandatory=$true)][AllowEmptyString()][string]$Text)
    return Get-SmPluginSha256Bytes -Bytes ([System.Text.UTF8Encoding]::new($false).GetBytes($Text))
}

function Test-SmPluginPathWithin {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Root
    )
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    if ([string]::Equals($full, $rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    return $full.StartsWith(
        $rootFull + '\',
        [System.StringComparison]::OrdinalIgnoreCase
    )
}

function Assert-SmPluginNoReparsePath {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [switch]$AllowMissingLeaf
    )
    $full = [System.IO.Path]::GetFullPath($Path)
    $probe = $full
    $isLeaf = $true
    while ($probe) {
        if (Test-Path -LiteralPath $probe) {
            $item = Get-Item -Force -LiteralPath $probe
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'PLUGIN_REPARSE_PATH_FORBIDDEN'
            }
        } elseif (-not $AllowMissingLeaf) {
            throw 'PLUGIN_PATH_MISSING'
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
        $isLeaf = $false
    }
    return $full
}

function Assert-SmPluginTreeNoReparse {
    param([Parameter(Mandatory=$true)][string]$Root)
    [void](Assert-SmPluginNoReparsePath -Path $Root)
    foreach ($item in Get-ChildItem -Force -Recurse -LiteralPath $Root) {
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'PLUGIN_REPARSE_TREE_FORBIDDEN'
        }
    }
}

function Test-SmPluginSafeRelativePath {
    param([Parameter(Mandatory=$true)][string]$Path)
    if (-not $Path -or [System.IO.Path]::IsPathRooted($Path) -or $Path.Contains('\')) {
        return $false
    }
    $parts = @($Path.Split('/'))
    return -not ($parts | Where-Object { -not $_ -or $_ -eq '.' -or $_ -eq '..' })
}

function ConvertTo-SmPluginCanonicalJson {
    param([Parameter(Mandatory=$true)]$Value)
    return ($Value | ConvertTo-Json -Depth 50 -Compress)
}

function Write-SmPluginJsonCreateNew {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)]$Value
    )
    $text = ($Value | ConvertTo-Json -Depth 50) + "`n"
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($text)
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

function Add-SmPluginJournalEvent {
    param(
        [Parameter(Mandatory=$true)][string]$JournalPath,
        [Parameter(Mandatory=$true)][string]$Event,
        [Parameter(Mandatory=$true)]$Data
    )
    $sequence = 1
    $previousEventSha256 = '0' * 64
    if (Test-Path -LiteralPath $JournalPath -PathType Leaf) {
        $priorLines = @(
            [System.IO.File]::ReadAllLines(
                $JournalPath,
                [System.Text.UTF8Encoding]::new($false, $true)
            ) | Where-Object { $_ }
        )
        $sequence = $priorLines.Count + 1
        if ($priorLines.Count -gt 0) {
            try { $previousEventSha256 = [string]($priorLines[-1] | ConvertFrom-Json).event_sha256 }
            catch { throw 'PLUGIN_JOURNAL_INVALID' }
            if ($previousEventSha256 -notmatch '^[0-9a-f]{64}$') {
                throw 'PLUGIN_JOURNAL_INVALID'
            }
        }
    }
    $body = [ordered]@{
        schema = 'stage14-personal-plugin-journal-event/v1'
        sequence = $sequence
        event = $Event
        at = [DateTimeOffset]::Now.ToString('o')
        data = $Data
        previous_event_sha256 = $previousEventSha256
    }
    $record = [ordered]@{}
    foreach ($name in $body.Keys) { $record[$name] = $body[$name] }
    $record.event_sha256 = Get-SmPluginSha256Text -Text (
        ConvertTo-SmPluginCanonicalJson -Value $body
    )
    $line = (ConvertTo-SmPluginCanonicalJson -Value $record) + "`n"
    [System.IO.File]::AppendAllText(
        $JournalPath,
        $line,
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Get-SmPluginTreeState {
    param([Parameter(Mandatory=$true)][string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $full)) {
        return [pscustomobject][ordered]@{
            schema = 'stage14-plugin-tree-state/v1'
            exists = $false
            tree_sha256 = $null
            file_count = 0
            files = @()
        }
    }
    if (-not (Test-Path -LiteralPath $full -PathType Container)) {
        throw 'PLUGIN_TREE_TARGET_NOT_DIRECTORY'
    }
    Assert-SmPluginTreeNoReparse -Root $full
    $records = New-Object System.Collections.Generic.List[object]
    foreach ($file in Get-ChildItem -Force -Recurse -File -LiteralPath $full | Sort-Object FullName) {
        $relative = $file.FullName.Substring($full.TrimEnd('\').Length).TrimStart('\').Replace('\','/')
        if (-not (Test-SmPluginSafeRelativePath -Path $relative)) {
            throw 'PLUGIN_TREE_RELATIVE_PATH_INVALID'
        }
        $records.Add([ordered]@{
            path = $relative
            bytes = [long]$file.Length
            sha256 = Get-SmPluginSha256File -Path $file.FullName
        })
    }
    $fingerprint = (
        @($records.ToArray() | ForEach-Object {
            '{0}`0{1}`0{2}' -f [string]$_.path, [long]$_.bytes, [string]$_.sha256
        }) -join "`n"
    )
    return [pscustomobject][ordered]@{
        schema = 'stage14-plugin-tree-state/v1'
        exists = $true
        tree_sha256 = Get-SmPluginSha256Text -Text $fingerprint
        file_count = $records.Count
        files = @($records.ToArray())
    }
}

function Get-SmPluginMarketplaceState {
    param([Parameter(Mandatory=$true)][string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $full)) {
        return [pscustomobject][ordered]@{
            schema = 'stage14-plugin-marketplace-state/v1'
            exists = $false
            sha256 = $null
            bytes = 0
        }
    }
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw 'PLUGIN_MARKETPLACE_NOT_FILE'
    }
    [void](Assert-SmPluginNoReparsePath -Path $full)
    return [pscustomobject][ordered]@{
        schema = 'stage14-plugin-marketplace-state/v1'
        exists = $true
        sha256 = Get-SmPluginSha256File -Path $full
        bytes = [long](Get-Item -LiteralPath $full).Length
    }
}

function Test-SmPluginExpectation {
    param(
        [Parameter(Mandatory=$true)]$State,
        [Parameter(Mandatory=$true)][string]$Expected,
        [Parameter(Mandatory=$true)][string]$HashProperty
    )
    if ($Expected -ceq 'ABSENT') {
        return -not [bool]$State.exists
    }
    if ($Expected -notmatch '^[0-9a-fA-F]{64}$' -or -not [bool]$State.exists) {
        return $false
    }
    return ([string]$State.$HashProperty).ToLowerInvariant() -ceq $Expected.ToLowerInvariant()
}

function Set-SmPluginProperty {
    param(
        [Parameter(Mandatory=$true)]$Object,
        [Parameter(Mandatory=$true)][string]$Name,
        $Value
    )
    if ($Object.PSObject.Properties.Name -contains $Name) {
        $Object.$Name = $Value
    } else {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    }
}

function Get-SmPluginPropertyValue {
    param(
        [Parameter(Mandatory=$true)]$Object,
        [Parameter(Mandatory=$true)][string]$Name
    )
    if ($Object -is [System.Collections.IDictionary]) {
        if (-not $Object.Contains($Name)) { throw 'PLUGIN_PROPERTY_MISSING' }
        return $Object[$Name]
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { throw 'PLUGIN_PROPERTY_MISSING' }
    return $property.Value
}

function Remove-SmPluginProperty {
    param(
        [Parameter(Mandatory=$true)]$Object,
        [Parameter(Mandatory=$true)][string]$Name
    )
    if ($Object.PSObject.Properties.Name -contains $Name) {
        $Object.PSObject.Properties.Remove($Name)
    }
}

function Get-SmPluginMarketplaceProjection {
    param([Parameter(Mandatory=$true)]$Marketplace)
    $copy = (ConvertTo-SmPluginCanonicalJson -Value $Marketplace) | ConvertFrom-Json
    Remove-SmPluginProperty -Object $copy -Name 'name'
    if ($copy.PSObject.Properties.Name -contains 'interface' -and $null -ne $copy.interface) {
        Remove-SmPluginProperty -Object $copy.interface -Name 'displayName'
    }
    $projectedPlugins = @()
    foreach ($plugin in @($copy.plugins)) {
        if ($null -eq $plugin -or [string]$plugin.name -cne 'semantic-memory') {
            $projectedPlugins += ,$plugin
            continue
        }
        Remove-SmPluginProperty -Object $plugin -Name 'name'
        Remove-SmPluginProperty -Object $plugin -Name 'category'
        if ($plugin.PSObject.Properties.Name -contains 'source' -and $null -ne $plugin.source) {
            Remove-SmPluginProperty -Object $plugin.source -Name 'source'
            Remove-SmPluginProperty -Object $plugin.source -Name 'path'
            if (@($plugin.source.PSObject.Properties).Count -eq 0) {
                Remove-SmPluginProperty -Object $plugin -Name 'source'
            }
        }
        if ($plugin.PSObject.Properties.Name -contains 'policy' -and $null -ne $plugin.policy) {
            Remove-SmPluginProperty -Object $plugin.policy -Name 'installation'
            Remove-SmPluginProperty -Object $plugin.policy -Name 'authentication'
            if (@($plugin.policy.PSObject.Properties).Count -eq 0) {
                Remove-SmPluginProperty -Object $plugin -Name 'policy'
            }
        }
        if (@($plugin.PSObject.Properties).Count -gt 0) {
            $projectedPlugins += ,$plugin
        }
    }
    if ($projectedPlugins.Count -eq 0) {
        Remove-SmPluginProperty -Object $copy -Name 'plugins'
    } else {
        $copy.plugins = @($projectedPlugins)
    }
    return Get-SmPluginSha256Text -Text (ConvertTo-SmPluginCanonicalJson -Value $copy)
}

function Get-SmPluginMarketplaceCandidate {
    param([Parameter(Mandatory=$true)][string]$MarketplacePath)
    $beforeState = Get-SmPluginMarketplaceState -Path $MarketplacePath
    if ($beforeState.exists) {
        try {
            $marketplace = [System.Text.UTF8Encoding]::new($false, $true).GetString(
                [System.IO.File]::ReadAllBytes($MarketplacePath)
            ) | ConvertFrom-Json
        } catch {
            throw 'PLUGIN_MARKETPLACE_JSON_INVALID'
        }
        if ($null -eq $marketplace -or $marketplace -is [array]) {
            throw 'PLUGIN_MARKETPLACE_JSON_INVALID'
        }
    } else {
        $marketplace = [pscustomobject][ordered]@{
            name = 'personal'
            interface = [pscustomobject][ordered]@{ displayName = 'Personal' }
            plugins = @()
        }
    }
    $projectionBefore = Get-SmPluginMarketplaceProjection -Marketplace $marketplace
    if ($marketplace.PSObject.Properties.Name -contains 'name' -and
        -not [string]::IsNullOrWhiteSpace([string]$marketplace.name) -and
        [string]$marketplace.name -cne 'personal') {
        throw 'PLUGIN_MARKETPLACE_NAME_CONFLICT'
    }
    Set-SmPluginProperty -Object $marketplace -Name 'name' -Value 'personal'
    if (-not ($marketplace.PSObject.Properties.Name -contains 'interface') -or
        $null -eq $marketplace.interface) {
        Set-SmPluginProperty -Object $marketplace -Name 'interface' -Value (
            [pscustomobject][ordered]@{ displayName = 'Personal' }
        )
    }
    Set-SmPluginProperty -Object $marketplace.interface -Name 'displayName' -Value 'Personal'
    if (-not ($marketplace.PSObject.Properties.Name -contains 'plugins') -or
        $null -eq $marketplace.plugins) {
        Set-SmPluginProperty -Object $marketplace -Name 'plugins' -Value @()
    }
    $plugins = @($marketplace.plugins)
    $semantic = @($plugins | Where-Object { $null -ne $_ -and [string]$_.name -ceq 'semantic-memory' })
    if ($semantic.Count -gt 1) { throw 'PLUGIN_MARKETPLACE_DUPLICATE_SEMANTIC_MEMORY' }
    if ($semantic.Count -eq 0) {
        $semanticRecord = [pscustomobject][ordered]@{}
        $plugins += $semanticRecord
    } else {
        $semanticRecord = $semantic[0]
    }
    Set-SmPluginProperty -Object $semanticRecord -Name 'name' -Value 'semantic-memory'
    if (-not ($semanticRecord.PSObject.Properties.Name -contains 'source') -or
        $null -eq $semanticRecord.source) {
        Set-SmPluginProperty -Object $semanticRecord -Name 'source' -Value ([pscustomobject][ordered]@{})
    }
    if ($semanticRecord.source -is [string] -or $semanticRecord.source -is [array]) {
        throw 'PLUGIN_MARKETPLACE_SOURCE_SHAPE_INVALID'
    }
    Set-SmPluginProperty -Object $semanticRecord.source -Name 'source' -Value 'local'
    Set-SmPluginProperty -Object $semanticRecord.source -Name 'path' -Value './plugins/semantic-memory'
    if (-not ($semanticRecord.PSObject.Properties.Name -contains 'policy') -or
        $null -eq $semanticRecord.policy) {
        Set-SmPluginProperty -Object $semanticRecord -Name 'policy' -Value ([pscustomobject][ordered]@{})
    }
    if ($semanticRecord.policy -is [string] -or $semanticRecord.policy -is [array]) {
        throw 'PLUGIN_MARKETPLACE_POLICY_SHAPE_INVALID'
    }
    Set-SmPluginProperty -Object $semanticRecord.policy -Name 'installation' -Value 'AVAILABLE'
    Set-SmPluginProperty -Object $semanticRecord.policy -Name 'authentication' -Value 'ON_INSTALL'
    Set-SmPluginProperty -Object $semanticRecord -Name 'category' -Value 'Productivity'
    $marketplace.plugins = @($plugins)
    $projectionAfter = Get-SmPluginMarketplaceProjection -Marketplace $marketplace
    if ($beforeState.exists -and $projectionBefore -cne $projectionAfter) {
        throw 'PLUGIN_MARKETPLACE_THIRD_PARTY_PROJECTION_CHANGED'
    }
    $text = ($marketplace | ConvertTo-Json -Depth 50) + "`n"
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($text)
    return [pscustomobject][ordered]@{
        value = $marketplace
        bytes = $bytes
        sha256 = Get-SmPluginSha256Bytes -Bytes $bytes
        before_state = $beforeState
        third_party_projection_sha256_before = $projectionBefore
        third_party_projection_sha256_after = $projectionAfter
    }
}

function Test-SmPluginMarketplaceManaged {
    param([Parameter(Mandatory=$true)][string]$MarketplacePath)
    if (-not (Test-Path -LiteralPath $MarketplacePath -PathType Leaf)) { return $false }
    try {
        $marketplace = [System.Text.UTF8Encoding]::new($false, $true).GetString(
            [System.IO.File]::ReadAllBytes($MarketplacePath)
        ) | ConvertFrom-Json
        if ([string]$marketplace.name -cne 'personal') { return $false }
        $semantic = @(@($marketplace.plugins) | Where-Object {
            $null -ne $_ -and [string]$_.name -ceq 'semantic-memory'
        })
        if ($semantic.Count -ne 1) { return $false }
        $record = $semantic[0]
        return (
            [string]$record.source.source -ceq 'local' -and
            [string]$record.source.path -ceq './plugins/semantic-memory' -and
            [string]$record.policy.installation -ceq 'AVAILABLE' -and
            [string]$record.policy.authentication -ceq 'ON_INSTALL' -and
            [string]$record.category -ceq 'Productivity'
        )
    } catch {
        return $false
    }
}

function Get-SmPluginPackage {
    param([Parameter(Mandatory=$true)][string]$Root)
    $packageFull = Assert-SmPluginNoReparsePath -Path ([System.IO.Path]::GetFullPath($Root))
    if (-not (Test-Path -LiteralPath $packageFull -PathType Container)) {
        throw 'PLUGIN_PACKAGE_ROOT_MISSING'
    }
    $installerManifestPath = Join-Path $packageFull 'installer-manifest.json'
    $pluginRoot = Join-Path $packageFull 'plugin\semantic-memory'
    [void](Assert-SmPluginNoReparsePath -Path $installerManifestPath)
    Assert-SmPluginTreeNoReparse -Root $pluginRoot
    try {
        $installer = Get-Content -Raw -Encoding UTF8 -LiteralPath $installerManifestPath |
            ConvertFrom-Json
    } catch {
        throw 'PLUGIN_INSTALLER_MANIFEST_INVALID'
    }
    if ([string]$installer.schema -cne 'stage14-offline-installer-manifest/v1' -or
        [string]$installer.personal_plugin.root -cne 'plugin/semantic-memory' -or
        [string]$installer.personal_plugin.manifest -cne
            'plugin/semantic-memory/.codex-plugin/plugin.json') {
        throw 'PLUGIN_INSTALLER_MANIFEST_INVALID'
    }
    $expected = @{}
    foreach ($record in @($installer.personal_plugin.files)) {
        $relative = [string]$record.path
        if (-not (Test-SmPluginSafeRelativePath -Path $relative) -or
            -not $relative.StartsWith('plugin/semantic-memory/', [System.StringComparison]::Ordinal)) {
            throw 'PLUGIN_PACKAGE_RECORD_PATH_INVALID'
        }
        if ($expected.ContainsKey($relative)) { throw 'PLUGIN_PACKAGE_DUPLICATE_RECORD' }
        $file = [System.IO.Path]::GetFullPath((Join-Path $packageFull $relative))
        if (-not (Test-SmPluginPathWithin -Path $file -Root $pluginRoot) -or
            -not (Test-Path -LiteralPath $file -PathType Leaf) -or
            [long](Get-Item -LiteralPath $file).Length -ne [long]$record.bytes -or
            (Get-SmPluginSha256File -Path $file) -cne ([string]$record.sha256).ToLowerInvariant()) {
            throw 'PLUGIN_PACKAGE_RECORD_MISMATCH'
        }
        $stripped = $relative.Substring('plugin/semantic-memory/'.Length)
        $expected[$relative] = [ordered]@{
            path = $stripped
            bytes = [long]$record.bytes
            sha256 = ([string]$record.sha256).ToLowerInvariant()
        }
    }
    $actual = @(
        Get-ChildItem -Force -Recurse -File -LiteralPath $pluginRoot |
            ForEach-Object {
                'plugin/semantic-memory/' +
                $_.FullName.Substring($pluginRoot.TrimEnd('\').Length).TrimStart('\').Replace('\','/')
            } |
            Sort-Object
    )
    if ([string]::Join("`n", @($expected.Keys | Sort-Object)) -cne
        [string]::Join("`n", $actual)) {
        throw 'PLUGIN_PACKAGE_FILE_SET_MISMATCH'
    }
    if ($actual | Where-Object { $_.ToLowerInvariant().EndsWith('/.mcp.json') }) {
        throw 'PLUGIN_DUPLICATE_MCP_REGISTRATION_FORBIDDEN'
    }
    $pluginManifestPath = Join-Path $pluginRoot '.codex-plugin\plugin.json'
    try {
        $pluginManifest = Get-Content -Raw -Encoding UTF8 -LiteralPath $pluginManifestPath |
            ConvertFrom-Json
    } catch {
        throw 'PLUGIN_MANIFEST_INVALID'
    }
    if ([string]$pluginManifest.name -cne 'semantic-memory' -or
        [string]$pluginManifest.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+codex\.[0-9]+)?$' -or
        $pluginManifest.PSObject.Properties.Name -contains 'mcpServers') {
        throw 'PLUGIN_DUPLICATE_MCP_REGISTRATION_FORBIDDEN'
    }
    $sourceRecords = @($expected.Values | Sort-Object { [string]$_['path'] })
    $sourceFingerprint = @($sourceRecords | ForEach-Object {
        '{0}`0{1}`0{2}' -f [string]$_.path, [long]$_.bytes, [string]$_.sha256
    }) -join "`n"
    $cacheRecords = @($sourceRecords | ForEach-Object {
        [ordered]@{
            path = ([string]$pluginManifest.version + '/' + [string]$_.path)
            bytes = [long]$_.bytes
            sha256 = [string]$_.sha256
        }
    } | Sort-Object { [string]$_['path'] })
    $cacheFingerprint = @($cacheRecords | ForEach-Object {
        '{0}`0{1}`0{2}' -f [string]$_.path, [long]$_.bytes, [string]$_.sha256
    }) -join "`n"
    return [pscustomobject][ordered]@{
        root = $packageFull
        plugin_root = $pluginRoot
        installer_manifest_path = $installerManifestPath
        installer_manifest_sha256 = Get-SmPluginSha256File -Path $installerManifestPath
        plugin_manifest_sha256 = Get-SmPluginSha256File -Path $pluginManifestPath
        version = [string]$pluginManifest.version
        source_files = $sourceRecords
        cache_files = $cacheRecords
        source_tree_sha256 = Get-SmPluginSha256Text -Text $sourceFingerprint
        cache_tree_sha256 = Get-SmPluginSha256Text -Text $cacheFingerprint
    }
}

function Copy-SmPluginTree {
    param(
        [Parameter(Mandatory=$true)][string]$Source,
        [Parameter(Mandatory=$true)][string]$Destination
    )
    if (Test-Path -LiteralPath $Destination) { throw 'PLUGIN_STAGE_ALREADY_EXISTS' }
    New-Item -ItemType Directory -Path $Destination | Out-Null
    foreach ($item in Get-ChildItem -Force -LiteralPath $Source) {
        Copy-Item -Force -Recurse -LiteralPath $item.FullName -Destination (
            Join-Path $Destination $item.Name
        )
    }
    Assert-SmPluginTreeNoReparse -Root $Destination
}

function Test-SmPluginTreeMatches {
    param($State,[string]$ExpectedSha256)
    return [bool]$State.exists -and
        [string]$State.tree_sha256 -ceq $ExpectedSha256
}

function Get-SmPluginPaths {
    param([string]$UserRoot,[string]$Codex,[string]$State)
    $homeFull = Assert-SmPluginNoReparsePath -Path ([System.IO.Path]::GetFullPath($UserRoot))
    if (-not (Test-Path -LiteralPath $homeFull -PathType Container)) {
        throw 'PLUGIN_USER_HOME_MISSING'
    }
    $codexFull = Assert-SmPluginNoReparsePath `
        -Path ([System.IO.Path]::GetFullPath($Codex)) `
        -AllowMissingLeaf
    $stateFull = Assert-SmPluginNoReparsePath `
        -Path ([System.IO.Path]::GetFullPath($State)) `
        -AllowMissingLeaf
    if (-not (Test-SmPluginPathWithin -Path $codexFull -Root $homeFull) -or
        -not (Test-SmPluginPathWithin -Path $stateFull -Root $homeFull)) {
        throw 'PLUGIN_USER_PATH_OUTSIDE_HOME'
    }
    $sourceTarget = Join-Path $homeFull 'plugins\semantic-memory'
    $marketplace = Join-Path $homeFull '.agents\plugins\marketplace.json'
    $cacheTarget = Join-Path $codexFull 'plugins\cache\personal\semantic-memory'
    foreach ($path in @($sourceTarget,$marketplace,$cacheTarget)) {
        [void](Assert-SmPluginNoReparsePath -Path $path -AllowMissingLeaf)
    }
    Assert-SmPluginPathsDisjoint -NamedPaths ([ordered]@{
        source_target = $sourceTarget
        cache_target = $cacheTarget
        marketplace_path = $marketplace
        state_root = $stateFull
    })
    return [pscustomobject][ordered]@{
        user_home = $homeFull
        codex_home = $codexFull
        state_root = $stateFull
        source_target = $sourceTarget
        marketplace_path = $marketplace
        cache_target = $cacheTarget
    }
}

function Get-SmPluginCurrentStates {
    param($Paths)
    return [pscustomobject][ordered]@{
        source = Get-SmPluginTreeState -Path $Paths.source_target
        cache = Get-SmPluginTreeState -Path $Paths.cache_target
        marketplace = Get-SmPluginMarketplaceState -Path $Paths.marketplace_path
    }
}

function Test-SmPluginTreeStatesEqual {
    param($Left,$Right)
    if ([bool]$Left.exists -ne [bool]$Right.exists) { return $false }
    if (-not [bool]$Left.exists) { return $true }
    return [string]$Left.tree_sha256 -ceq [string]$Right.tree_sha256
}

function Test-SmPluginMarketplaceStatesEqual {
    param($Left,$Right)
    if ([bool]$Left.exists -ne [bool]$Right.exists) { return $false }
    if (-not [bool]$Left.exists) { return $true }
    return [string]$Left.sha256 -ceq [string]$Right.sha256
}

function Test-SmPluginStatesEqual {
    param($Left,$Right)
    return (
        (Test-SmPluginTreeStatesEqual -Left $Left.source -Right $Right.source) -and
        (Test-SmPluginTreeStatesEqual -Left $Left.cache -Right $Right.cache) -and
        (Test-SmPluginMarketplaceStatesEqual -Left $Left.marketplace -Right $Right.marketplace)
    )
}

function Assert-SmPluginThreeStateCas {
    param(
        [Parameter(Mandatory=$true)]$Paths,
        [Parameter(Mandatory=$true)]$Expected,
        [Parameter(Mandatory=$true)][string]$Phase
    )
    $actual = Get-SmPluginCurrentStates -Paths $Paths
    if (-not (Test-SmPluginStatesEqual -Left $actual -Right $Expected)) {
        throw "PLUGIN_THREE_STATE_CAS_DRIFT:$Phase"
    }
    return $actual
}

function Assert-SmPluginPathsDisjoint {
    param([Parameter(Mandatory=$true)]$NamedPaths)
    $names = @($NamedPaths.Keys)
    for ($leftIndex = 0; $leftIndex -lt $names.Count; $leftIndex++) {
        for ($rightIndex = $leftIndex + 1; $rightIndex -lt $names.Count; $rightIndex++) {
            $leftName = [string]$names[$leftIndex]
            $rightName = [string]$names[$rightIndex]
            $leftPath = [System.IO.Path]::GetFullPath([string]$NamedPaths[$leftName])
            $rightPath = [System.IO.Path]::GetFullPath([string]$NamedPaths[$rightName])
            if ((Test-SmPluginPathWithin -Path $leftPath -Root $rightPath) -or
                (Test-SmPluginPathWithin -Path $rightPath -Root $leftPath)) {
                throw "PLUGIN_USER_PATHS_OVERLAP:${leftName}:${rightName}"
            }
        }
    }
}

function Get-SmPluginDescriptorBinding {
    param(
        [Parameter(Mandatory=$true)]$Descriptor,
        [Parameter(Mandatory=$true)][string]$DescriptorSha256
    )
    if ($DescriptorSha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'PLUGIN_TRANSACTION_DESCRIPTOR_BINDING_INVALID'
    }
    return [ordered]@{
        schema = 'stage14-personal-plugin-descriptor-binding/v1'
        descriptor_sha256 = $DescriptorSha256
        transaction_id = [string]$Descriptor.transaction_id
        version = [string]$Descriptor.version
        package_installer_manifest_sha256 =
            [string]$Descriptor.package_installer_manifest_sha256
        package_plugin_manifest_sha256 =
            [string]$Descriptor.package_plugin_manifest_sha256
        paths_sha256 = Get-SmPluginSha256Text -Text (
            ConvertTo-SmPluginCanonicalJson -Value $Descriptor.paths
        )
        states_before_sha256 = Get-SmPluginSha256Text -Text (
            ConvertTo-SmPluginCanonicalJson -Value $Descriptor.states_before
        )
        states_desired_sha256 = Get-SmPluginSha256Text -Text (
            ConvertTo-SmPluginCanonicalJson -Value $Descriptor.states_desired
        )
        staging_sha256 = Get-SmPluginSha256Text -Text (
            ConvertTo-SmPluginCanonicalJson -Value $Descriptor.staging
        )
        backups_sha256 = Get-SmPluginSha256Text -Text (
            ConvertTo-SmPluginCanonicalJson -Value $Descriptor.backups
        )
        third_party_projection_sha256_before =
            [string]$Descriptor.third_party_projection_sha256_before
        third_party_projection_sha256_after =
            [string]$Descriptor.third_party_projection_sha256_after
        cli_invocation_requested = [bool]$Descriptor.cli_invocation_requested
        cli_commands_sha256 = Get-SmPluginSha256Text -Text (
            ConvertTo-SmPluginCanonicalJson -Value @($Descriptor.cli_commands)
        )
        raw_credential_values_recorded =
            [bool]$Descriptor.raw_credential_values_recorded
    }
}

function Assert-SmPluginExactProperties {
    param(
        [Parameter(Mandatory=$true)]$Value,
        [Parameter(Mandatory=$true)][string[]]$Names,
        [Parameter(Mandatory=$true)][string]$ErrorCode
    )
    if ($null -eq $Value -or $Value -is [array] -or $Value -is [string]) {
        throw $ErrorCode
    }
    $actual = @($Value.PSObject.Properties.Name | Sort-Object)
    $expected = @($Names | Sort-Object)
    if ([string]::Join("`n", $actual) -cne [string]::Join("`n", $expected)) {
        throw $ErrorCode
    }
}

function Assert-SmPluginDescriptorTreeState {
    param([Parameter(Mandatory=$true)]$State)
    Assert-SmPluginExactProperties -Value $State -Names @(
        'schema','exists','tree_sha256','file_count','files'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    if ([string]$State.schema -cne 'stage14-plugin-tree-state/v1') {
        throw 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    }
    if (-not [bool]$State.exists) {
        if ($null -ne $State.tree_sha256 -or [int]$State.file_count -ne 0 -or
            @($State.files).Count -ne 0) {
            throw 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
        }
        return
    }
    if ([string]$State.tree_sha256 -notmatch '^[0-9a-f]{64}$' -or
        [int]$State.file_count -ne @($State.files).Count) {
        throw 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    }
    $seen = @{}
    $records = @()
    foreach ($record in @($State.files)) {
        Assert-SmPluginExactProperties -Value $record -Names @(
            'path','bytes','sha256'
        ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
        $relative = [string]$record.path
        if (-not (Test-SmPluginSafeRelativePath -Path $relative) -or
            $seen.ContainsKey($relative) -or [long]$record.bytes -lt 0 -or
            [string]$record.sha256 -notmatch '^[0-9a-f]{64}$') {
            throw 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
        }
        $seen[$relative] = $true
        $records += ,$record
    }
    $fingerprint = @($records | Sort-Object { [string]$_.path } | ForEach-Object {
        '{0}`0{1}`0{2}' -f [string]$_.path, [long]$_.bytes, [string]$_.sha256
    }) -join "`n"
    if ((Get-SmPluginSha256Text -Text $fingerprint) -cne [string]$State.tree_sha256) {
        throw 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    }
}

function Assert-SmPluginDescriptorMarketplaceState {
    param([Parameter(Mandatory=$true)]$State)
    Assert-SmPluginExactProperties -Value $State -Names @(
        'schema','exists','sha256','bytes'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    if ([string]$State.schema -cne 'stage14-plugin-marketplace-state/v1') {
        throw 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    }
    if ([bool]$State.exists) {
        if ([string]$State.sha256 -notmatch '^[0-9a-f]{64}$' -or [long]$State.bytes -lt 0) {
            throw 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
        }
    } elseif ($null -ne $State.sha256 -or [long]$State.bytes -ne 0) {
        throw 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    }
}

function Get-SmPluginJournal {
    param([Parameter(Mandatory=$true)][string]$Path)
    $full = Assert-SmPluginNoReparsePath -Path ([System.IO.Path]::GetFullPath($Path))
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw 'PLUGIN_JOURNAL_INVALID'
    }
    try {
        $rawLines = [System.IO.File]::ReadAllLines(
            $full,
            [System.Text.UTF8Encoding]::new($false, $true)
        )
    } catch {
        throw 'PLUGIN_JOURNAL_INVALID'
    }
    if ($rawLines.Count -eq 0 -or @($rawLines | Where-Object { -not $_ }).Count -ne 0) {
        throw 'PLUGIN_JOURNAL_INVALID'
    }
    $allowed = @(
        'PREPARED','SOURCE_DISPLACED','SOURCE_SWAPPED','CACHE_DISPLACED',
        'CACHE_SWAPPED','MARKETPLACE_SWAPPED','CLI_COMMAND_SUCCEEDED',
        'COMMIT_VERIFIED','CRASH_SIMULATED','FAILURE_OBSERVED',
        'ROLLBACK_VERIFIED','ROLLBACK_OWNED_CHANGES_VERIFIED_EXTERNAL_DRIFT',
        'RECOVERED_COMMIT_VERIFIED'
    )
    $events = @()
    $previous = '0' * 64
    $seen = @{}
    for ($index = 0; $index -lt $rawLines.Count; $index++) {
        try { $record = $rawLines[$index] | ConvertFrom-Json }
        catch { throw 'PLUGIN_JOURNAL_INVALID' }
        Assert-SmPluginExactProperties -Value $record -Names @(
            'schema','sequence','event','at','data',
            'previous_event_sha256','event_sha256'
        ) -ErrorCode 'PLUGIN_JOURNAL_INVALID'
        $event = [string]$record.event
        if ([string]$record.schema -cne 'stage14-personal-plugin-journal-event/v1' -or
            [int]$record.sequence -ne ($index + 1) -or
            $allowed -cnotcontains $event -or
            [string]$record.previous_event_sha256 -cne $previous -or
            [string]$record.event_sha256 -notmatch '^[0-9a-f]{64}$') {
            throw 'PLUGIN_JOURNAL_INVALID'
        }
        $parsedAt = [DateTimeOffset]::MinValue
        if (-not [DateTimeOffset]::TryParse([string]$record.at, [ref]$parsedAt)) {
            throw 'PLUGIN_JOURNAL_INVALID'
        }
        $body = [ordered]@{
            schema = [string]$record.schema
            sequence = [int]$record.sequence
            event = $event
            at = [string]$record.at
            data = $record.data
            previous_event_sha256 = [string]$record.previous_event_sha256
        }
        if ((Get-SmPluginSha256Text -Text (
            ConvertTo-SmPluginCanonicalJson -Value $body
        )) -cne [string]$record.event_sha256) {
            throw 'PLUGIN_JOURNAL_INVALID'
        }
        if ($index -eq 0 -and $event -cne 'PREPARED') {
            throw 'PLUGIN_JOURNAL_ORDER_INVALID'
        }
        if ($event -in @(
            'PREPARED','SOURCE_DISPLACED','SOURCE_SWAPPED','CACHE_DISPLACED','CACHE_SWAPPED',
            'MARKETPLACE_SWAPPED','COMMIT_VERIFIED','CRASH_SIMULATED',
            'FAILURE_OBSERVED','ROLLBACK_VERIFIED',
            'ROLLBACK_OWNED_CHANGES_VERIFIED_EXTERNAL_DRIFT',
            'RECOVERED_COMMIT_VERIFIED'
        )) {
            if ($seen.ContainsKey($event)) { throw 'PLUGIN_JOURNAL_ORDER_INVALID' }
            $seen[$event] = $true
        }
        if ($event -eq 'SOURCE_SWAPPED' -and -not $seen.ContainsKey('PREPARED')) {
            throw 'PLUGIN_JOURNAL_ORDER_INVALID'
        }
        if ($event -in @('CACHE_DISPLACED','CACHE_SWAPPED') -and
            -not $seen.ContainsKey('SOURCE_SWAPPED')) {
            throw 'PLUGIN_JOURNAL_ORDER_INVALID'
        }
        if ($event -eq 'MARKETPLACE_SWAPPED' -and -not $seen.ContainsKey('CACHE_SWAPPED')) {
            throw 'PLUGIN_JOURNAL_ORDER_INVALID'
        }
        if ($event -in @('CLI_COMMAND_SUCCEEDED','COMMIT_VERIFIED','RECOVERED_COMMIT_VERIFIED') -and
            -not $seen.ContainsKey('MARKETPLACE_SWAPPED')) {
            throw 'PLUGIN_JOURNAL_ORDER_INVALID'
        }
        $events += ,$record
        $previous = [string]$record.event_sha256
    }
    $terminalEvents = @(
        'ROLLBACK_VERIFIED',
        'ROLLBACK_OWNED_CHANGES_VERIFIED_EXTERNAL_DRIFT',
        'RECOVERED_COMMIT_VERIFIED'
    )
    for ($index = 0; $index -lt $events.Count - 1; $index++) {
        if ($terminalEvents -ccontains [string]$events[$index].event) {
            throw 'PLUGIN_JOURNAL_ORDER_INVALID'
        }
    }
    return [pscustomobject][ordered]@{
        path = $full
        events = @($events)
        last = $events[-1]
        sha256 = Get-SmPluginSha256File -Path $full
    }
}

function Test-SmPluginDesiredState {
    param($Paths,$Package)
    $states = Get-SmPluginCurrentStates -Paths $Paths
    return [pscustomobject][ordered]@{
        match = (
            (Test-SmPluginTreeMatches -State $states.source -ExpectedSha256 $Package.source_tree_sha256) -and
            (Test-SmPluginTreeMatches -State $states.cache -ExpectedSha256 $Package.cache_tree_sha256) -and
            (Test-SmPluginMarketplaceManaged -MarketplacePath $Paths.marketplace_path)
        )
        states = $states
    }
}

function Move-SmPluginIfPresent {
    param([string]$Path,[string]$Destination)
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    if (Test-Path -LiteralPath $Destination) { throw 'PLUGIN_RECOVERY_DESTINATION_EXISTS' }
    Move-Item -LiteralPath $Path -Destination $Destination
    return $true
}

function Restore-SmPluginTransactionBeforeState {
    param(
        [Parameter(Mandatory=$true)]$Descriptor,
        [Parameter(Mandatory=$true)][string]$DescriptorPath,
        [Parameter(Mandatory=$true)][string]$JournalPath,
        [Parameter(Mandatory=$true)][string]$Reason
    )
    $paths = $Descriptor.paths
    $before = $Descriptor.states_before
    $desired = $Descriptor.states_desired
    $externalDrift = New-Object System.Collections.Generic.List[string]

    foreach ($name in @('source','cache')) {
        $target = [string](Get-SmPluginPropertyValue `
            -Object $paths `
            -Name "${name}_target")
        $backup = [string](Get-SmPluginPropertyValue `
            -Object $Descriptor.backups `
            -Name "${name}_before")
        $failed = [string](Get-SmPluginPropertyValue `
            -Object $Descriptor.backups `
            -Name "${name}_failed")
        $current = Get-SmPluginTreeState -Path $target
        $beforeState = $before.$name
        $desiredState = [pscustomobject]@{
            exists = $true
            tree_sha256 = [string]$desired.$name.tree_sha256
        }
        $isBefore = Test-SmPluginTreeStatesEqual -Left $current -Right $beforeState
        $isDesired = Test-SmPluginTreeStatesEqual -Left $current -Right $desiredState
        $isDisplaced = -not [bool]$current.exists -and [bool]$beforeState.exists

        if ($isDesired) {
            if (Test-Path -LiteralPath $failed) {
                throw 'PLUGIN_ROLLBACK_FAILED_ARTIFACT_EXISTS'
            }
            Move-Item -LiteralPath $target -Destination $failed
            $failedState = Get-SmPluginTreeState -Path $failed
            if (-not (Test-SmPluginTreeStatesEqual -Left $failedState -Right $desiredState)) {
                throw 'PLUGIN_ROLLBACK_FAILED_ARTIFACT_INVALID'
            }
            $isDisplaced = [bool]$beforeState.exists
        } elseif (-not $isBefore -and -not $isDisplaced) {
            $externalDrift.Add($name)
            continue
        }

        if ($isDisplaced) {
            if (-not (Test-Path -LiteralPath $backup -PathType Container)) {
                throw 'PLUGIN_ROLLBACK_BACKUP_MISSING'
            }
            $backupState = Get-SmPluginTreeState -Path $backup
            if (-not (Test-SmPluginTreeStatesEqual -Left $backupState -Right $beforeState)) {
                throw 'PLUGIN_ROLLBACK_BACKUP_INVALID'
            }
            if (Test-Path -LiteralPath $target) {
                throw 'PLUGIN_ROLLBACK_TARGET_REAPPEARED'
            }
            Move-Item -LiteralPath $backup -Destination $target
        }
    }

    $marketTarget = [string]$paths.marketplace_path
    $marketCurrent = Get-SmPluginMarketplaceState -Path $marketTarget
    $marketBefore = $before.marketplace
    $marketDesired = [pscustomobject]@{
        exists = $true
        sha256 = [string]$desired.marketplace.sha256
    }
    $marketIsBefore = Test-SmPluginMarketplaceStatesEqual -Left $marketCurrent -Right $marketBefore
    $marketIsDesired = Test-SmPluginMarketplaceStatesEqual -Left $marketCurrent -Right $marketDesired
    if ($marketIsDesired) {
        $failedMarket = [string]$Descriptor.backups.marketplace_failed
        if (Test-Path -LiteralPath $failedMarket) {
            throw 'PLUGIN_ROLLBACK_FAILED_ARTIFACT_EXISTS'
        }
        Move-Item -LiteralPath $marketTarget -Destination $failedMarket
        if ((Get-SmPluginSha256File -Path $failedMarket) -cne [string]$marketDesired.sha256) {
            throw 'PLUGIN_ROLLBACK_FAILED_ARTIFACT_INVALID'
        }
        $marketCurrent = Get-SmPluginMarketplaceState -Path $marketTarget
    } elseif (-not $marketIsBefore) {
        $externalDrift.Add('marketplace')
    }
    if (-not ($externalDrift -contains 'marketplace') -and
        [bool]$marketBefore.exists -and -not [bool]$marketCurrent.exists) {
        $marketBackup = [string]$Descriptor.backups.marketplace_before_preserved
        if (-not (Test-Path -LiteralPath $marketBackup -PathType Leaf) -or
            (Get-SmPluginSha256File -Path $marketBackup) -cne [string]$marketBefore.sha256) {
            throw 'PLUGIN_ROLLBACK_MARKETPLACE_BACKUP_INVALID'
        }
        Move-Item -LiteralPath $marketBackup -Destination $marketTarget
    }

    foreach ($entry in @(
        @{name='source';path=[string]$Descriptor.staging.source;kind='tree';sha=[string]$desired.source.tree_sha256},
        @{name='cache';path=[string]$Descriptor.staging.cache;kind='tree';sha=[string]$desired.cache.tree_sha256},
        @{name='marketplace';path=[string]$Descriptor.staging.marketplace;kind='file';sha=[string]$desired.marketplace.sha256}
    )) {
        if (-not (Test-Path -LiteralPath ([string]$entry.path))) { continue }
        if ([string]$entry.kind -ceq 'tree') {
            $stageState = Get-SmPluginTreeState -Path ([string]$entry.path)
            if (-not [bool]$stageState.exists -or
                [string]$stageState.tree_sha256 -cne [string]$entry.sha) {
                throw 'PLUGIN_ROLLBACK_STAGE_INVALID'
            }
            Remove-Item -LiteralPath ([string]$entry.path) -Recurse -Force
        } else {
            if (-not (Test-Path -LiteralPath ([string]$entry.path) -PathType Leaf) -or
                (Get-SmPluginSha256File -Path ([string]$entry.path)) -cne [string]$entry.sha) {
                throw 'PLUGIN_ROLLBACK_STAGE_INVALID'
            }
            Remove-Item -LiteralPath ([string]$entry.path) -Force
        }
    }

    $restored = Get-SmPluginCurrentStates -Paths $paths
    foreach ($name in @('source','cache')) {
        if ($externalDrift -contains $name) { continue }
        if (-not (Test-SmPluginTreeStatesEqual -Left $restored.$name -Right $before.$name)) {
            throw 'PLUGIN_ROLLBACK_VERIFY_FAILED'
        }
    }
    if (-not ($externalDrift -contains 'marketplace') -and
        -not (Test-SmPluginMarketplaceStatesEqual -Left $restored.marketplace -Right $before.marketplace)) {
        throw 'PLUGIN_ROLLBACK_VERIFY_FAILED'
    }
    $restored | Add-Member -NotePropertyName external_drift_detected -NotePropertyValue (
        $externalDrift.Count -gt 0
    )
    $restored | Add-Member -NotePropertyName external_drift_components -NotePropertyValue (
        @($externalDrift.ToArray())
    )
    $event = $(if ($externalDrift.Count -gt 0) {
        'ROLLBACK_OWNED_CHANGES_VERIFIED_EXTERNAL_DRIFT'
    } else {
        'ROLLBACK_VERIFIED'
    })
    Add-SmPluginJournalEvent -JournalPath $JournalPath -Event $event -Data (
        [ordered]@{ reason=$Reason; states=$restored; raw_credential_values_recorded=$false }
    )
    return $restored
}

function Invoke-SmPluginCliContract {
    param($Paths,[string]$CliPath,[string]$MarketplacePath,[string]$JournalPath)
    $cliFull = Assert-SmPluginNoReparsePath -Path ([System.IO.Path]::GetFullPath($CliPath))
    if (-not (Test-Path -LiteralPath $cliFull -PathType Leaf)) {
        throw 'PLUGIN_CODEX_CLI_MISSING'
    }
    $prior = @{
        HOME = $env:HOME
        USERPROFILE = $env:USERPROFILE
        CODEX_HOME = $env:CODEX_HOME
    }
    try {
        $env:HOME = [string]$Paths.user_home
        $env:USERPROFILE = [string]$Paths.user_home
        $env:CODEX_HOME = [string]$Paths.codex_home
        $commands = @(
            @('plugin','marketplace','add',$MarketplacePath),
            @('plugin','add','semantic-memory@personal')
        )
        foreach ($arguments in $commands) {
            $captured = & $cliFull @arguments 2>&1
            $exitCode = $LASTEXITCODE
            $captured = $null
            if ($exitCode -ne 0) { throw 'PLUGIN_CODEX_CLI_CONTRACT_FAILED' }
            Add-SmPluginJournalEvent -JournalPath $JournalPath -Event 'CLI_COMMAND_SUCCEEDED' -Data (
                [ordered]@{
                    arguments = @($arguments)
                    exit_code = $exitCode
                    stdout_recorded = $false
                    stderr_recorded = $false
                }
            )
        }
    } finally {
        $env:HOME = $prior.HOME
        $env:USERPROFILE = $prior.USERPROFILE
        $env:CODEX_HOME = $prior.CODEX_HOME
    }
}

function Read-SmPluginDescriptor {
    param([string]$Path,$Paths,$Package)
    $full = Assert-SmPluginNoReparsePath -Path ([System.IO.Path]::GetFullPath($Path))
    if (-not (Test-SmPluginPathWithin -Path $full -Root $Paths.state_root)) {
        throw 'PLUGIN_TRANSACTION_OUTSIDE_STATE_ROOT'
    }
    try { $value = Get-Content -Raw -Encoding UTF8 -LiteralPath $full | ConvertFrom-Json }
    catch { throw 'PLUGIN_TRANSACTION_DESCRIPTOR_INVALID' }
    if ([string]$value.schema -cne 'stage14-personal-plugin-transaction/v1' -or
        [string]$value.transaction_id -notmatch '^[0-9]{8}_[0-9]{9}-[0-9a-f]{12}$') {
        throw 'PLUGIN_TRANSACTION_DESCRIPTOR_INVALID'
    }
    Assert-SmPluginExactProperties -Value $value -Names @(
        'schema','transaction_id','created_at','version',
        'package_installer_manifest_sha256','package_plugin_manifest_sha256',
        'paths','states_before','states_desired','staging','backups',
        'third_party_projection_sha256_before',
        'third_party_projection_sha256_after',
        'cli_invocation_requested','cli_commands',
        'raw_credential_values_recorded'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_INVALID'
    $createdAt = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParse([string]$value.created_at, [ref]$createdAt) -or
        [string]$value.version -cne [string]$Package.version -or
        [string]$value.package_installer_manifest_sha256 -cne
            [string]$Package.installer_manifest_sha256 -or
        [string]$value.package_plugin_manifest_sha256 -cne
            [string]$Package.plugin_manifest_sha256 -or
        [bool]$value.raw_credential_values_recorded -or
        [string]$value.third_party_projection_sha256_before -notmatch '^[0-9a-f]{64}$' -or
        [string]$value.third_party_projection_sha256_after -notmatch '^[0-9a-f]{64}$' -or
        [string]$value.third_party_projection_sha256_before -cne
            [string]$value.third_party_projection_sha256_after) {
        throw 'PLUGIN_TRANSACTION_DESCRIPTOR_INVALID'
    }
    Assert-SmPluginExactProperties -Value $value.paths -Names @(
        'user_home','codex_home','state_root',
        'source_target','marketplace_path','cache_target'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_INVALID'
    foreach ($name in @('user_home','codex_home','state_root','source_target','marketplace_path','cache_target')) {
        if (-not [string]::Equals(
            [System.IO.Path]::GetFullPath([string]$value.paths.$name),
            [System.IO.Path]::GetFullPath([string]$Paths.$name),
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
            throw 'PLUGIN_TRANSACTION_PATH_MISMATCH'
        }
    }
    $transactionRoot = Split-Path -Parent $full
    $expectedTransactionRoot = Join-Path $Paths.state_root (
        'plugin-install__' + [string]$value.transaction_id
    )
    $expectedDescriptorPath = Join-Path $expectedTransactionRoot 'transaction.json'
    if (-not [string]::Equals(
        $transactionRoot,
        $expectedTransactionRoot,
        [System.StringComparison]::OrdinalIgnoreCase
    ) -or -not [string]::Equals(
        $full,
        $expectedDescriptorPath,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw 'PLUGIN_TRANSACTION_DIRECTORY_MISMATCH'
    }

    Assert-SmPluginExactProperties -Value $value.states_before -Names @(
        'source','cache','marketplace'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    Assert-SmPluginDescriptorTreeState -State $value.states_before.source
    Assert-SmPluginDescriptorTreeState -State $value.states_before.cache
    Assert-SmPluginDescriptorMarketplaceState -State $value.states_before.marketplace

    Assert-SmPluginExactProperties -Value $value.states_desired -Names @(
        'source','cache','marketplace'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    Assert-SmPluginExactProperties -Value $value.states_desired.source -Names @(
        'exists','tree_sha256'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    Assert-SmPluginExactProperties -Value $value.states_desired.cache -Names @(
        'exists','tree_sha256'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    Assert-SmPluginExactProperties -Value $value.states_desired.marketplace -Names @(
        'exists','sha256'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    if (-not [bool]$value.states_desired.source.exists -or
        -not [bool]$value.states_desired.cache.exists -or
        -not [bool]$value.states_desired.marketplace.exists -or
        [string]$value.states_desired.source.tree_sha256 -cne
            [string]$Package.source_tree_sha256 -or
        [string]$value.states_desired.cache.tree_sha256 -cne
            [string]$Package.cache_tree_sha256 -or
        [string]$value.states_desired.marketplace.sha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'PLUGIN_TRANSACTION_DESCRIPTOR_STATE_INVALID'
    }

    Assert-SmPluginExactProperties -Value $value.staging -Names @(
        'source','cache','marketplace'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_INVALID'
    Assert-SmPluginExactProperties -Value $value.backups -Names @(
        'source_before','cache_before','marketplace_before_preserved',
        'marketplace_swap_backup','source_failed','cache_failed',
        'marketplace_failed'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_INVALID'
    $expectedStaging = [ordered]@{
        source = Join-Path (Split-Path -Parent $Paths.source_target) (
            ".semantic-memory.$($value.transaction_id).source.stage"
        )
        cache = Join-Path (Split-Path -Parent $Paths.cache_target) (
            ".semantic-memory.$($value.transaction_id).cache.stage"
        )
        marketplace = Join-Path (Split-Path -Parent $Paths.marketplace_path) (
            ".marketplace.$($value.transaction_id).candidate.tmp"
        )
    }
    $expectedBackups = [ordered]@{
        source_before = Join-Path $transactionRoot 'source.before'
        cache_before = Join-Path $transactionRoot 'cache.before'
        marketplace_before_preserved = Join-Path $transactionRoot 'marketplace.before.preserved'
        marketplace_swap_backup = Join-Path $transactionRoot 'marketplace.swap-backup'
        source_failed = Join-Path $transactionRoot 'source.failed'
        cache_failed = Join-Path $transactionRoot 'cache.failed'
        marketplace_failed = Join-Path $transactionRoot 'marketplace.failed.json'
    }
    foreach ($name in @('source','cache','marketplace')) {
        if (-not [string]::Equals(
            [System.IO.Path]::GetFullPath([string]$value.staging.$name),
            [System.IO.Path]::GetFullPath([string]$expectedStaging[$name]),
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
            throw 'PLUGIN_TRANSACTION_STAGING_PATH_MISMATCH'
        }
    }
    foreach ($name in $expectedBackups.Keys) {
        if (-not [string]::Equals(
            [System.IO.Path]::GetFullPath(
                [string](Get-SmPluginPropertyValue -Object $value.backups -Name $name)
            ),
            [System.IO.Path]::GetFullPath([string]$expectedBackups[$name]),
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
            throw 'PLUGIN_TRANSACTION_BACKUP_PATH_MISMATCH'
        }
    }

    $expectedCommands = @(
        @('plugin','marketplace','add',[string]$Paths.marketplace_path),
        @('plugin','add','semantic-memory@personal')
    )
    if ((ConvertTo-SmPluginCanonicalJson -Value @($value.cli_commands)) -cne
        (ConvertTo-SmPluginCanonicalJson -Value $expectedCommands)) {
        throw 'PLUGIN_TRANSACTION_CLI_CONTRACT_MISMATCH'
    }

    foreach ($name in @('source','cache')) {
        $beforeState = $value.states_before.$name
        $backupPath = [string](Get-SmPluginPropertyValue `
            -Object $value.backups `
            -Name "${name}_before")
        if (Test-Path -LiteralPath $backupPath) {
            $backupState = Get-SmPluginTreeState -Path $backupPath
            if (-not (Test-SmPluginTreeStatesEqual -Left $backupState -Right $beforeState)) {
                throw 'PLUGIN_TRANSACTION_BACKUP_STATE_MISMATCH'
            }
        }
        $stagePath = [string]$value.staging.$name
        if (Test-Path -LiteralPath $stagePath) {
            $stageState = Get-SmPluginTreeState -Path $stagePath
            if (-not [bool]$stageState.exists -or
                [string]$stageState.tree_sha256 -cne
                    [string]$value.states_desired.$name.tree_sha256) {
                throw 'PLUGIN_TRANSACTION_STAGING_STATE_MISMATCH'
            }
        }
        $failedPath = [string](Get-SmPluginPropertyValue `
            -Object $value.backups `
            -Name "${name}_failed")
        if (Test-Path -LiteralPath $failedPath) {
            $failedState = Get-SmPluginTreeState -Path $failedPath
            if (-not [bool]$failedState.exists -or
                [string]$failedState.tree_sha256 -cne
                    [string]$value.states_desired.$name.tree_sha256) {
                throw 'PLUGIN_TRANSACTION_FAILED_STATE_MISMATCH'
            }
        }
    }
    $marketBeforePath = [string]$value.backups.marketplace_before_preserved
    if (Test-Path -LiteralPath $marketBeforePath) {
        if (-not [bool]$value.states_before.marketplace.exists -or
            (Get-SmPluginSha256File -Path $marketBeforePath) -cne
                [string]$value.states_before.marketplace.sha256) {
            throw 'PLUGIN_TRANSACTION_BACKUP_STATE_MISMATCH'
        }
    }
    foreach ($marketPath in @(
        [string]$value.staging.marketplace,
        [string]$value.backups.marketplace_failed
    )) {
        if (Test-Path -LiteralPath $marketPath) {
            if (-not (Test-Path -LiteralPath $marketPath -PathType Leaf) -or
                (Get-SmPluginSha256File -Path $marketPath) -cne
                    [string]$value.states_desired.marketplace.sha256) {
                throw 'PLUGIN_TRANSACTION_STAGING_STATE_MISMATCH'
            }
        }
    }
    $marketSwapPath = [string]$value.backups.marketplace_swap_backup
    if (Test-Path -LiteralPath $marketSwapPath) {
        if (-not [bool]$value.states_before.marketplace.exists -or
            (Get-SmPluginSha256File -Path $marketSwapPath) -cne
                [string]$value.states_before.marketplace.sha256) {
            throw 'PLUGIN_TRANSACTION_BACKUP_STATE_MISMATCH'
        }
    }

    $journal = Get-SmPluginJournal -Path (Join-Path $transactionRoot 'journal.jsonl')
    $prepared = $journal.events[0]
    Assert-SmPluginExactProperties -Value $prepared.data -Names @(
        'descriptor_binding','raw_credential_values_recorded'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_JOURNAL_MISMATCH'
    if ([bool]$prepared.data.raw_credential_values_recorded) {
        throw 'PLUGIN_TRANSACTION_DESCRIPTOR_JOURNAL_MISMATCH'
    }
    $descriptorSha256 = Get-SmPluginSha256File -Path $full
    $expectedBinding = Get-SmPluginDescriptorBinding `
        -Descriptor $value `
        -DescriptorSha256 $descriptorSha256
    Assert-SmPluginExactProperties -Value $prepared.data.descriptor_binding -Names @(
        'schema','descriptor_sha256','transaction_id','version',
        'package_installer_manifest_sha256','package_plugin_manifest_sha256',
        'paths_sha256','states_before_sha256','states_desired_sha256',
        'staging_sha256','backups_sha256',
        'third_party_projection_sha256_before',
        'third_party_projection_sha256_after',
        'cli_invocation_requested','cli_commands_sha256',
        'raw_credential_values_recorded'
    ) -ErrorCode 'PLUGIN_TRANSACTION_DESCRIPTOR_JOURNAL_MISMATCH'
    if ((ConvertTo-SmPluginCanonicalJson -Value $prepared.data.descriptor_binding) -cne
        (ConvertTo-SmPluginCanonicalJson -Value $expectedBinding)) {
        throw 'PLUGIN_TRANSACTION_DESCRIPTOR_JOURNAL_MISMATCH'
    }
    return [pscustomobject]@{
        path = $full
        value = $value
        journal = $journal.path
        journal_state = $journal
    }
}

if (-not $UserHome) {
    throw 'PLUGIN_USER_HOME_REQUIRED'
}
if (-not $CodexHome) { $CodexHome = Join-Path $UserHome '.codex' }
if (-not $StateRoot) {
    $StateRoot = Join-Path $UserHome 'AppData\Local\SemanticMemory\backups\codex-plugin'
}

$mutex = $null
$mutexTaken = $false
try {
    $package = Get-SmPluginPackage -Root $PackageRoot
    $paths = Get-SmPluginPaths -UserRoot $UserHome -Codex $CodexHome -State $StateRoot
    $mutexName = 'Local\SemanticMemoryPlugin-' + (
        Get-SmPluginSha256Text -Text ([string]$paths.state_root)
    ).Substring(0, 24)
    $mutex = [System.Threading.Mutex]::new($false, $mutexName)
    $mutexTaken = $mutex.WaitOne(0)
    if (-not $mutexTaken) { throw 'PLUGIN_TRANSACTION_BUSY' }

    if ($Action -eq 'Preview') {
        $states = Get-SmPluginCurrentStates -Paths $paths
        $candidate = Get-SmPluginMarketplaceCandidate -MarketplacePath $paths.marketplace_path
        [ordered]@{
            schema = 'stage14-personal-plugin-preview/v1'
            status = $(if (
                (Test-SmPluginTreeMatches -State $states.source -ExpectedSha256 $package.source_tree_sha256) -and
                (Test-SmPluginTreeMatches -State $states.cache -ExpectedSha256 $package.cache_tree_sha256) -and
                (Test-SmPluginMarketplaceManaged -MarketplacePath $paths.marketplace_path)
            ) { 'GREEN_PLUGIN_CURRENT' } else { 'RED_PLUGIN_DRIFT' })
            version = $package.version
            package_installer_manifest_sha256 = $package.installer_manifest_sha256
            package_plugin_manifest_sha256 = $package.plugin_manifest_sha256
            package_source_files = $package.source_files
            package_cache_files = $package.cache_files
            paths = $paths
            states_before = $states
            states_desired = [ordered]@{
                source = [ordered]@{exists=$true;tree_sha256=$package.source_tree_sha256}
                cache = [ordered]@{exists=$true;tree_sha256=$package.cache_tree_sha256}
                marketplace = [ordered]@{exists=$true;sha256=$candidate.sha256}
            }
            third_party_projection_sha256_before = $candidate.third_party_projection_sha256_before
            third_party_projection_sha256_after = $candidate.third_party_projection_sha256_after
            cli_commands = @(
                @('plugin','marketplace','add',[string]$paths.marketplace_path),
                @('plugin','add','semantic-memory@personal')
            )
            write_performed = $false
            raw_credential_values_recorded = $false
        } | ConvertTo-Json -Depth 30
        exit 0
    }

    if ($Action -eq 'Verify') {
        $verified = Test-SmPluginDesiredState -Paths $paths -Package $package
        [ordered]@{
            schema = 'stage14-personal-plugin-verification/v1'
            status = $(if ($verified.match) { 'PASS' } else { 'RED_PLUGIN_DRIFT' })
            version = $package.version
            source_hash_match = Test-SmPluginTreeMatches -State $verified.states.source -ExpectedSha256 $package.source_tree_sha256
            cache_hash_match = Test-SmPluginTreeMatches -State $verified.states.cache -ExpectedSha256 $package.cache_tree_sha256
            marketplace_unique = Test-SmPluginMarketplaceManaged -MarketplacePath $paths.marketplace_path
            plugin_mcp_config_present = $false
            states = $verified.states
            write_performed = $false
            raw_credential_values_recorded = $false
        } | ConvertTo-Json -Depth 30
        exit $(if ($verified.match) { 0 } else { 1 })
    }

    if (-not $ConfirmUserMutation) { throw 'PLUGIN_EXPLICIT_MUTATION_CONFIRMATION_REQUIRED' }

    if ($Action -in @('Rollback','Recover')) {
        if (-not $TransactionPath) { throw 'PLUGIN_TRANSACTION_PATH_REQUIRED' }
        $loaded = Read-SmPluginDescriptor -Path $TransactionPath -Paths $paths -Package $package
        $currentStates = Get-SmPluginCurrentStates -Paths $paths
        $desiredExact = (
            (Test-SmPluginTreeStatesEqual -Left $currentStates.source -Right $loaded.value.states_desired.source) -and
            (Test-SmPluginTreeStatesEqual -Left $currentStates.cache -Right $loaded.value.states_desired.cache) -and
            (Test-SmPluginMarketplaceStatesEqual -Left $currentStates.marketplace -Right $loaded.value.states_desired.marketplace) -and
            (Test-SmPluginMarketplaceManaged -MarketplacePath $paths.marketplace_path)
        )
        $beforeExact = Test-SmPluginStatesEqual `
            -Left $currentStates `
            -Right $loaded.value.states_before
        $lastEvent = [string]$loaded.journal_state.last.event
        if ($Action -eq 'Recover' -and
            $lastEvent -in @('COMMIT_VERIFIED','RECOVERED_COMMIT_VERIFIED')) {
            if (-not $desiredExact) { throw 'PLUGIN_RECOVERY_TERMINAL_STATE_MISMATCH' }
            [ordered]@{
                schema='stage14-personal-plugin-recovery-result/v1'
                status='RECOVERED_COMMIT_VERIFIED'
                transaction_path=$loaded.path
                journal_write_performed=$false
                write_performed=$false
                states=$currentStates
                raw_credential_values_recorded=$false
            } | ConvertTo-Json -Depth 30
            exit 0
        }
        if ($Action -eq 'Recover' -and $lastEvent -eq 'ROLLBACK_VERIFIED') {
            if (-not $beforeExact) { throw 'PLUGIN_RECOVERY_TERMINAL_STATE_MISMATCH' }
            [ordered]@{
                schema='stage14-personal-plugin-recovery-result/v1'
                status='RECOVERED_ROLLBACK_VERIFIED'
                transaction_path=$loaded.path
                journal_write_performed=$false
                write_performed=$false
                states=$currentStates
                raw_credential_values_recorded=$false
            } | ConvertTo-Json -Depth 30
            exit 0
        }
        if ($Action -eq 'Recover' -and
            $lastEvent -eq 'ROLLBACK_OWNED_CHANGES_VERIFIED_EXTERNAL_DRIFT') {
            [ordered]@{
                schema='stage14-personal-plugin-recovery-result/v1'
                status='RECOVERY_EXTERNAL_DRIFT_PRESERVED'
                transaction_path=$loaded.path
                journal_write_performed=$false
                write_performed=$false
                states=$currentStates
                raw_credential_values_recorded=$false
            } | ConvertTo-Json -Depth 30
            exit 1
        }
        if ($Action -eq 'Recover' -and $desiredExact) {
            $journalEvents = @(
                $loaded.journal_state.events |
                    ForEach-Object { [string]$_.event }
            )
            if ($journalEvents -cnotcontains 'MARKETPLACE_SWAPPED') {
                $marketplaceSwapBackup = [string](
                    $loaded.value.backups.marketplace_swap_backup
                )
                $marketplaceSwapProven = (
                    $lastEvent -ceq 'CRASH_SIMULATED' -and
                    $journalEvents -ccontains 'CACHE_SWAPPED' -and
                    [bool]$loaded.value.states_before.marketplace.exists -and
                    (Test-Path -LiteralPath $marketplaceSwapBackup -PathType Leaf) -and
                    (Get-SmPluginSha256File -Path $marketplaceSwapBackup) -ceq
                        [string]$loaded.value.states_before.marketplace.sha256
                )
                if (-not $marketplaceSwapProven) {
                    throw 'PLUGIN_RECOVERY_MARKETPLACE_SWAP_PROOF_MISSING'
                }
                Add-SmPluginJournalEvent `
                    -JournalPath $loaded.journal `
                    -Event 'MARKETPLACE_SWAPPED' `
                    -Data $currentStates.marketplace
            }
            Add-SmPluginJournalEvent -JournalPath $loaded.journal -Event 'RECOVERED_COMMIT_VERIFIED' -Data (
                [ordered]@{states=$currentStates;raw_credential_values_recorded=$false}
            )
            [ordered]@{
                schema='stage14-personal-plugin-recovery-result/v1'
                status='RECOVERED_COMMIT_VERIFIED'
                transaction_path=$loaded.path
                journal_write_performed=$true
                write_performed=$false
                states=$currentStates
                raw_credential_values_recorded=$false
            } | ConvertTo-Json -Depth 30
            exit 0
        }
        if ($Action -eq 'Rollback' -and $lastEvent -eq 'ROLLBACK_VERIFIED') {
            if (-not $beforeExact) { throw 'PLUGIN_RECOVERY_TERMINAL_STATE_MISMATCH' }
            [ordered]@{
                schema='stage14-personal-plugin-recovery-result/v1'
                status='ROLLED_BACK_VERIFIED'
                transaction_path=$loaded.path
                journal_write_performed=$false
                write_performed=$false
                states=$currentStates
                raw_credential_values_recorded=$false
            } | ConvertTo-Json -Depth 30
            exit 0
        }
        $restored = Restore-SmPluginTransactionBeforeState `
            -Descriptor $loaded.value `
            -DescriptorPath $loaded.path `
            -JournalPath $loaded.journal `
            -Reason $Action
        [ordered]@{
            schema = 'stage14-personal-plugin-recovery-result/v1'
            status = $(if ([bool]$restored.external_drift_detected) {
                'RECOVERY_EXTERNAL_DRIFT_PRESERVED'
            } elseif ($Action -eq 'Recover') {
                'RECOVERED_ROLLBACK_VERIFIED'
            } else {
                'ROLLED_BACK_VERIFIED'
            })
            transaction_path = $loaded.path
            journal_write_performed = $true
            write_performed = $true
            states = $restored
            raw_credential_values_recorded = $false
        } | ConvertTo-Json -Depth 30
        exit $(if ([bool]$restored.external_drift_detected) { 1 } else { 0 })
    }

    $before = Get-SmPluginCurrentStates -Paths $paths
    if ([string]::IsNullOrWhiteSpace($ExpectedMarketplaceSha256) -or
        [string]::IsNullOrWhiteSpace($ExpectedSourceTreeSha256) -or
        [string]::IsNullOrWhiteSpace($ExpectedCacheTreeSha256)) {
        throw 'PLUGIN_ALL_CAS_PRECONDITIONS_REQUIRED'
    }
    if (-not (Test-SmPluginExpectation -State $before.marketplace -Expected $ExpectedMarketplaceSha256 -HashProperty 'sha256') -or
        -not (Test-SmPluginExpectation -State $before.source -Expected $ExpectedSourceTreeSha256 -HashProperty 'tree_sha256') -or
        -not (Test-SmPluginExpectation -State $before.cache -Expected $ExpectedCacheTreeSha256 -HashProperty 'tree_sha256')) {
        throw 'PLUGIN_CAS_PRECONDITION_CONFLICT'
    }
    $already = Test-SmPluginDesiredState -Paths $paths -Package $package
    if ($already.match) {
        [ordered]@{
            schema = 'stage14-personal-plugin-transaction-result/v1'
            status = 'REPLAYED_ZERO_WRITE'
            version = $package.version
            transaction_id = $null
            transaction_path = $null
            journal_path = $null
            states = $already.states
            write_performed = $false
            cli_invoked = $false
            raw_credential_values_recorded = $false
        } | ConvertTo-Json -Depth 30
        exit 0
    }

    $candidate = Get-SmPluginMarketplaceCandidate -MarketplacePath $paths.marketplace_path
    foreach ($root in @(
        $paths.state_root,
        (Split-Path -Parent $paths.source_target),
        (Split-Path -Parent $paths.cache_target),
        (Split-Path -Parent $paths.marketplace_path)
    )) {
        if (-not (Test-Path -LiteralPath $root)) {
            New-Item -ItemType Directory -Path $root | Out-Null
        }
        [void](Assert-SmPluginNoReparsePath -Path $root)
    }
    $volume = [System.IO.Path]::GetPathRoot($paths.state_root)
    foreach ($target in @($paths.source_target,$paths.cache_target,$paths.marketplace_path)) {
        if (-not [string]::Equals(
            [System.IO.Path]::GetPathRoot($target),
            $volume,
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
            throw 'PLUGIN_TRANSACTION_CROSS_VOLUME_FORBIDDEN'
        }
    }
    $transactionId = [DateTimeOffset]::Now.ToString('yyyyMMdd_HHmmssfff') + '-' +
        [guid]::NewGuid().ToString('N').Substring(0, 12)
    $transactionRoot = Join-Path $paths.state_root ("plugin-install__$transactionId")
    New-Item -ItemType Directory -Path $transactionRoot | Out-Null
    $descriptorPath = Join-Path $transactionRoot 'transaction.json'
    $journalPath = Join-Path $transactionRoot 'journal.jsonl'
    $sourceStage = Join-Path (Split-Path -Parent $paths.source_target) (
        ".semantic-memory.$transactionId.source.stage"
    )
    $cacheStage = Join-Path (Split-Path -Parent $paths.cache_target) (
        ".semantic-memory.$transactionId.cache.stage"
    )
    $marketTemp = Join-Path (Split-Path -Parent $paths.marketplace_path) (
        ".marketplace.$transactionId.candidate.tmp"
    )
    $backupPaths = [ordered]@{
        source_before = Join-Path $transactionRoot 'source.before'
        cache_before = Join-Path $transactionRoot 'cache.before'
        marketplace_before_preserved = Join-Path $transactionRoot 'marketplace.before.preserved'
        marketplace_swap_backup = Join-Path $transactionRoot 'marketplace.swap-backup'
        source_failed = Join-Path $transactionRoot 'source.failed'
        cache_failed = Join-Path $transactionRoot 'cache.failed'
        marketplace_failed = Join-Path $transactionRoot 'marketplace.failed.json'
    }
    Copy-SmPluginTree -Source $package.plugin_root -Destination $sourceStage
    New-Item -ItemType Directory -Path $cacheStage | Out-Null
    Copy-SmPluginTree -Source $package.plugin_root -Destination (
        Join-Path $cacheStage $package.version
    )
    [System.IO.File]::WriteAllBytes($marketTemp, [byte[]]$candidate.bytes)
    $sourceStageState = Get-SmPluginTreeState -Path $sourceStage
    $cacheStageState = Get-SmPluginTreeState -Path $cacheStage
    if ($sourceStageState.tree_sha256 -cne $package.source_tree_sha256 -or
        $cacheStageState.tree_sha256 -cne $package.cache_tree_sha256 -or
        (Get-SmPluginSha256File -Path $marketTemp) -cne $candidate.sha256) {
        throw 'PLUGIN_STAGING_VERIFICATION_FAILED'
    }
    if ($before.marketplace.exists) {
        [System.IO.File]::WriteAllBytes(
            $backupPaths.marketplace_before_preserved,
            [System.IO.File]::ReadAllBytes($paths.marketplace_path)
        )
    }
    $descriptor = [ordered]@{
        schema = 'stage14-personal-plugin-transaction/v1'
        transaction_id = $transactionId
        created_at = [DateTimeOffset]::Now.ToString('o')
        version = $package.version
        package_installer_manifest_sha256 = $package.installer_manifest_sha256
        package_plugin_manifest_sha256 = $package.plugin_manifest_sha256
        paths = $paths
        states_before = $before
        states_desired = [ordered]@{
            source = [ordered]@{exists=$true;tree_sha256=$package.source_tree_sha256}
            cache = [ordered]@{exists=$true;tree_sha256=$package.cache_tree_sha256}
            marketplace = [ordered]@{exists=$true;sha256=$candidate.sha256}
        }
        staging = [ordered]@{
            source = $sourceStage
            cache = $cacheStage
            marketplace = $marketTemp
        }
        backups = $backupPaths
        third_party_projection_sha256_before = $candidate.third_party_projection_sha256_before
        third_party_projection_sha256_after = $candidate.third_party_projection_sha256_after
        cli_invocation_requested = [bool]$InvokeCodexCli
        cli_commands = @(
            @('plugin','marketplace','add',[string]$paths.marketplace_path),
            @('plugin','add','semantic-memory@personal')
        )
        raw_credential_values_recorded = $false
    }
    Write-SmPluginJsonCreateNew -Path $descriptorPath -Value $descriptor
    $descriptorSha256 = Get-SmPluginSha256File -Path $descriptorPath
    $descriptorBinding = Get-SmPluginDescriptorBinding `
        -Descriptor ([pscustomobject]$descriptor) `
        -DescriptorSha256 $descriptorSha256
    Add-SmPluginJournalEvent -JournalPath $journalPath -Event 'PREPARED' -Data (
        [ordered]@{
            descriptor_binding = $descriptorBinding
            raw_credential_values_recorded = $false
        }
    )
    $crashSimulation = $false
    try {
        $phaseBefore = $before
        $phaseAfterSource = [pscustomobject]@{
            source = $descriptor.states_desired.source
            cache = $before.cache
            marketplace = $before.marketplace
        }
        $phaseAfterCache = [pscustomobject]@{
            source = $descriptor.states_desired.source
            cache = $descriptor.states_desired.cache
            marketplace = $before.marketplace
        }
        $phaseDesired = [pscustomobject]$descriptor.states_desired

        if ($FaultInjection -eq 'delay_before_source_swap') {
            Start-Sleep -Milliseconds 1500
        }
        [void](Assert-SmPluginThreeStateCas `
            -Paths $paths `
            -Expected $phaseBefore `
            -Phase 'BEFORE_SOURCE_DISPLACE')
        [void](Move-SmPluginIfPresent `
            -Path $paths.source_target `
            -Destination $backupPaths.source_before)
        $sourceDisplacedExpected = [pscustomobject]@{
            source = [pscustomobject]@{exists=$false;tree_sha256=$null}
            cache = $before.cache
            marketplace = $before.marketplace
        }
        [void](Assert-SmPluginThreeStateCas `
            -Paths $paths `
            -Expected $sourceDisplacedExpected `
            -Phase 'AFTER_SOURCE_DISPLACE')
        if ([bool]$before.source.exists) {
            $sourceBackupState = Get-SmPluginTreeState -Path $backupPaths.source_before
            if (-not (Test-SmPluginTreeStatesEqual -Left $sourceBackupState -Right $before.source)) {
                throw 'PLUGIN_SOURCE_DISPLACE_BACKUP_INVALID'
            }
        }
        Add-SmPluginJournalEvent -JournalPath $journalPath -Event 'SOURCE_DISPLACED' -Data (
            [ordered]@{
                target_absent=$true
                backup_state=$(if ([bool]$before.source.exists) {
                    Get-SmPluginTreeState -Path $backupPaths.source_before
                } else { $null })
                raw_credential_values_recorded=$false
            }
        )
        if ($FaultInjection -eq 'crash_after_source_displace') {
            $crashSimulation = $true
            throw 'FAULT_INJECTION_AFTER_SOURCE_DISPLACE'
        }
        [void](Assert-SmPluginThreeStateCas `
            -Paths $paths `
            -Expected $sourceDisplacedExpected `
            -Phase 'BEFORE_SOURCE_STAGE_SWITCH')
        Move-Item -LiteralPath $sourceStage -Destination $paths.source_target
        [void](Assert-SmPluginThreeStateCas `
            -Paths $paths `
            -Expected $phaseAfterSource `
            -Phase 'AFTER_SOURCE_STAGE_SWITCH')
        Add-SmPluginJournalEvent -JournalPath $journalPath -Event 'SOURCE_SWAPPED' -Data (
            Get-SmPluginTreeState -Path $paths.source_target
        )
        if ($FaultInjection -in @('after_source_swap','crash_after_source_swap')) {
            if ($FaultInjection.StartsWith('crash_')) { $crashSimulation = $true }
            throw 'FAULT_INJECTION_AFTER_SOURCE_SWAP'
        }

        if ($FaultInjection -eq 'delay_before_cache_swap') {
            Start-Sleep -Milliseconds 1500
        }
        [void](Assert-SmPluginThreeStateCas `
            -Paths $paths `
            -Expected $phaseAfterSource `
            -Phase 'BEFORE_CACHE_DISPLACE')
        [void](Move-SmPluginIfPresent `
            -Path $paths.cache_target `
            -Destination $backupPaths.cache_before)
        $cacheDisplacedExpected = [pscustomobject]@{
            source = $descriptor.states_desired.source
            cache = [pscustomobject]@{exists=$false;tree_sha256=$null}
            marketplace = $before.marketplace
        }
        [void](Assert-SmPluginThreeStateCas `
            -Paths $paths `
            -Expected $cacheDisplacedExpected `
            -Phase 'AFTER_CACHE_DISPLACE')
        if ([bool]$before.cache.exists) {
            $cacheBackupState = Get-SmPluginTreeState -Path $backupPaths.cache_before
            if (-not (Test-SmPluginTreeStatesEqual -Left $cacheBackupState -Right $before.cache)) {
                throw 'PLUGIN_CACHE_DISPLACE_BACKUP_INVALID'
            }
        }
        Add-SmPluginJournalEvent -JournalPath $journalPath -Event 'CACHE_DISPLACED' -Data (
            [ordered]@{
                target_absent=$true
                backup_state=$(if ([bool]$before.cache.exists) {
                    Get-SmPluginTreeState -Path $backupPaths.cache_before
                } else { $null })
                raw_credential_values_recorded=$false
            }
        )
        if ($FaultInjection -eq 'crash_after_cache_displace') {
            $crashSimulation = $true
            throw 'FAULT_INJECTION_AFTER_CACHE_DISPLACE'
        }
        [void](Assert-SmPluginThreeStateCas `
            -Paths $paths `
            -Expected $cacheDisplacedExpected `
            -Phase 'BEFORE_CACHE_STAGE_SWITCH')
        Move-Item -LiteralPath $cacheStage -Destination $paths.cache_target
        [void](Assert-SmPluginThreeStateCas `
            -Paths $paths `
            -Expected $phaseAfterCache `
            -Phase 'AFTER_CACHE_STAGE_SWITCH')
        Add-SmPluginJournalEvent -JournalPath $journalPath -Event 'CACHE_SWAPPED' -Data (
            Get-SmPluginTreeState -Path $paths.cache_target
        )
        if ($FaultInjection -in @('after_cache_swap','crash_after_cache_swap')) {
            if ($FaultInjection.StartsWith('crash_')) { $crashSimulation = $true }
            throw 'FAULT_INJECTION_AFTER_CACHE_SWAP'
        }

        if ($FaultInjection -eq 'delay_before_marketplace_swap') {
            Start-Sleep -Milliseconds 1500
        }
        [void](Assert-SmPluginThreeStateCas `
            -Paths $paths `
            -Expected $phaseAfterCache `
            -Phase 'BEFORE_MARKETPLACE_SWITCH')
        if ($before.marketplace.exists) {
            if (Test-Path -LiteralPath $backupPaths.marketplace_swap_backup) {
                throw 'PLUGIN_MARKETPLACE_SWAP_BACKUP_EXISTS'
            }
            [System.IO.File]::Replace(
                $marketTemp,
                $paths.marketplace_path,
                $backupPaths.marketplace_swap_backup,
                $false
            )
            if ($FaultInjection -eq 'crash_after_marketplace_replace') {
                $crashSimulation = $true
                throw 'FAULT_INJECTION_AFTER_MARKETPLACE_REPLACE'
            }
            if (-not (Test-Path -LiteralPath $backupPaths.marketplace_swap_backup -PathType Leaf) -or
                (Get-SmPluginSha256File -Path $backupPaths.marketplace_swap_backup) -cne
                    [string]$before.marketplace.sha256) {
                throw 'PLUGIN_MARKETPLACE_SWAP_BACKUP_INVALID'
            }
        } else {
            Move-Item -LiteralPath $marketTemp -Destination $paths.marketplace_path
        }
        [void](Assert-SmPluginThreeStateCas `
            -Paths $paths `
            -Expected $phaseDesired `
            -Phase 'AFTER_MARKETPLACE_SWITCH')
        Add-SmPluginJournalEvent -JournalPath $journalPath -Event 'MARKETPLACE_SWAPPED' -Data (
            Get-SmPluginMarketplaceState -Path $paths.marketplace_path
        )
        if ($FaultInjection -in @('after_marketplace_swap','crash_after_marketplace_swap')) {
            if ($FaultInjection.StartsWith('crash_')) { $crashSimulation = $true }
            throw 'FAULT_INJECTION_AFTER_MARKETPLACE_SWAP'
        }

        if ($InvokeCodexCli) {
            if (-not $CodexCliPath) { throw 'PLUGIN_CODEX_CLI_PATH_REQUIRED' }
            $preCli = Get-SmPluginCurrentStates -Paths $paths
            Invoke-SmPluginCliContract `
                -Paths $paths `
                -CliPath $CodexCliPath `
                -MarketplacePath $paths.marketplace_path `
                -JournalPath $journalPath
            $postCli = Get-SmPluginCurrentStates -Paths $paths
            if ((ConvertTo-SmPluginCanonicalJson -Value $preCli) -cne
                (ConvertTo-SmPluginCanonicalJson -Value $postCli)) {
                throw 'PLUGIN_CODEX_CLI_UNCONTROLLED_STATE_MUTATION'
            }
        }
        if ($FaultInjection -eq 'after_cli') { throw 'FAULT_INJECTION_AFTER_CLI' }
        $verified = Test-SmPluginDesiredState -Paths $paths -Package $package
        if (-not $verified.match -or
            (Get-SmPluginMarketplaceState -Path $paths.marketplace_path).sha256 -cne $candidate.sha256) {
            throw 'PLUGIN_POST_SWAP_VERIFY_FAILED'
        }
        Add-SmPluginJournalEvent -JournalPath $journalPath -Event 'COMMIT_VERIFIED' -Data (
            [ordered]@{states=$verified.states;raw_credential_values_recorded=$false}
        )
        [ordered]@{
            schema = 'stage14-personal-plugin-transaction-result/v1'
            status = 'APPLIED_VERIFIED'
            version = $package.version
            transaction_id = $transactionId
            transaction_path = $descriptorPath
            journal_path = $journalPath
            states_before = $before
            states_after = $verified.states
            third_party_projection_preserved = (
                $candidate.third_party_projection_sha256_before -ceq
                $candidate.third_party_projection_sha256_after
            )
            marketplace_original_backup_path = $(if ($before.marketplace.exists) {
                $backupPaths.marketplace_before_preserved
            } else { $null })
            write_performed = $true
            cli_invoked = [bool]$InvokeCodexCli
            raw_credential_values_recorded = $false
        } | ConvertTo-Json -Depth 30
        exit 0
    } catch {
        $errorCode = ([string]$_.Exception.Message).Split(':')[0].Trim()
        Add-SmPluginJournalEvent -JournalPath $journalPath -Event (
            $(if ($crashSimulation) { 'CRASH_SIMULATED' } else { 'FAILURE_OBSERVED' })
        ) -Data ([ordered]@{error_code=$errorCode;raw_credential_values_recorded=$false})
        if ($crashSimulation) {
            [ordered]@{
                schema='stage14-personal-plugin-transaction-result/v1'
                status='CRASH_SIMULATED_RECOVERY_REQUIRED'
                error_code=$errorCode
                transaction_id=$transactionId
                transaction_path=$descriptorPath
                journal_path=$journalPath
                write_performed=$true
                rollback_performed=$false
                raw_credential_values_recorded=$false
            } | ConvertTo-Json -Depth 20
            exit 1
        }
        $restored = Restore-SmPluginTransactionBeforeState `
            -Descriptor ([pscustomobject]$descriptor) `
            -DescriptorPath $descriptorPath `
            -JournalPath $journalPath `
            -Reason $errorCode
        [ordered]@{
            schema='stage14-personal-plugin-transaction-result/v1'
            status=$(if ([bool]$restored.external_drift_detected) {
                'ROLLED_BACK_OWNED_CHANGES_EXTERNAL_DRIFT'
            } else {
                'ROLLED_BACK_VERIFIED'
            })
            error_code=$errorCode
            transaction_id=$transactionId
            transaction_path=$descriptorPath
            journal_path=$journalPath
            states_after=$restored
            write_performed=$true
            rollback_performed=$true
            raw_credential_values_recorded=$false
        } | ConvertTo-Json -Depth 30
        exit 1
    }
} catch {
    $errorCode = ([string]$_.Exception.Message).Split(':')[0].Trim()
    if ($errorCode -notmatch '^(PLUGIN|FAULT_INJECTION)_[A-Z0-9_]+$') {
        $errorCode = 'PLUGIN_TRANSACTION_FAILED'
    }
    [ordered]@{
        schema = 'stage14-personal-plugin-error/v1'
        action = $Action
        status = 'FAILED_CLOSED'
        error_code = $errorCode
        write_performed = $false
        raw_credential_values_recorded = $false
    } | ConvertTo-Json -Depth 10
    exit 1
} finally {
    if ($mutexTaken -and $mutex) {
        try { $mutex.ReleaseMutex() } catch {}
    }
    if ($mutex) { $mutex.Dispose() }
}
