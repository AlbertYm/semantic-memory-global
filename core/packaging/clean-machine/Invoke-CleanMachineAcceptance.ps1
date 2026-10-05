param(
    [Parameter(Mandatory=$true)][string]$TransferKitRoot,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
$TransferKitRoot = [System.IO.Path]::GetFullPath($TransferKitRoot)
$installerPath = Join-Path $TransferKitRoot 'semantic-memory-mcp-offline-installer-v1.1.0-rc.1.zip'
$requiredEntries = @(
    'Install-SemanticMemoryV2.ps1',
    'SemanticMemory.Common.psm1',
    'semantic-memory-launcher.ps1',
    'Invoke-Stage14Migration.ps1',
    'Repair-SemanticMemory.ps1',
    'Invoke-PackageAcceptance.ps1',
    'payload/payload-manifest.json',
    'payload/semantic-memory-mcp.exe',
    'payload/semantic-memory-hook.exe',
    'payload/semantic-memory-manager.exe',
    'plugin/semantic-memory/.codex-plugin/plugin.json',
    'plugin/semantic-memory/hooks/hooks.json',
    'plugin/semantic-memory/skills/neuroplastic-memory/SKILL.md'
)
$entries = @()
$payloadManifest = $null
$pluginManifest = $null
if (Test-Path -LiteralPath $installerPath -PathType Leaf) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($installerPath)
    try {
        $entries = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\','/').TrimStart('/') })
        $manifestEntry = $archive.Entries | Where-Object { $_.FullName.Replace('\','/').TrimStart('/') -eq 'payload/payload-manifest.json' } | Select-Object -First 1
        if ($manifestEntry) {
            $stream = $manifestEntry.Open()
            $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8, $true)
            try { $payloadManifest = $reader.ReadToEnd() | ConvertFrom-Json }
            finally { $reader.Dispose(); $stream.Dispose() }
        }
        $pluginManifestEntry = $archive.Entries | Where-Object { $_.FullName.Replace('\','/').TrimStart('/') -eq 'plugin/semantic-memory/.codex-plugin/plugin.json' } | Select-Object -First 1
        if ($pluginManifestEntry) {
            $stream = $pluginManifestEntry.Open()
            $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8, $true)
            try { $pluginManifest = $reader.ReadToEnd() | ConvertFrom-Json }
            finally { $reader.Dispose(); $stream.Dispose() }
        }
    } finally {
        $archive.Dispose()
    }
}
$checks = [ordered]@{
    transfer_kit_exists = Test-Path -LiteralPath $TransferKitRoot -PathType Container
    installer_present = Test-Path -LiteralPath $installerPath -PathType Leaf
    stage14_installer_entries_present = -not ($requiredEntries | Where-Object { $_ -notin $entries })
    legacy_installer_absent = 'Install-SemanticMemory.ps1' -notin $entries
    plugin_mcp_registration_absent = (
        'plugin/semantic-memory/.mcp.json' -notin $entries -and
        $null -ne $pluginManifest -and
        $pluginManifest.PSObject.Properties.Name -notcontains 'mcpServers'
    )
    stage14_payload_manifest = (
        $null -ne $payloadManifest -and
        $payloadManifest.schema -eq 'stage14-payload-manifest/v1' -and
        $payloadManifest.entrypoints.mcp -eq 'semantic-memory-mcp.exe' -and
        $payloadManifest.entrypoints.hook -eq 'semantic-memory-hook.exe' -and
        $payloadManifest.entrypoints.manager -eq 'semantic-memory-manager.exe'
    )
    verify_only_default = $true
    real_second_machine_accepted = $false
}
$pass = $true
foreach ($name in $checks.Keys) {
    if ($name -eq 'real_second_machine_accepted') { continue }
    if (-not $checks[$name]) { $pass = $false }
}
$result = [ordered]@{
    schema = 'stage14-clean-machine-acceptance/v1'
    status = $(if ($pass) { 'PASS_VERIFY_ONLY_TRANSFER_KIT' } else { 'FAIL_VERIFY_ONLY_TRANSFER_KIT' })
    checks = $checks
}
if ($Output) {
    $result | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath $Output
}
$result | ConvertTo-Json -Depth 8
if (-not $pass) { exit 1 }
