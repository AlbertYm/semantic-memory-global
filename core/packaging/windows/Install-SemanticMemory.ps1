param(
    [ValidateSet('Install','Verify','Repair','Uninstall')]
    [string]$Action = 'Install',
    [string]$PackageRoot = $PSScriptRoot,
    [string]$InstallRoot,
    [switch]$PurgeData,
    [string]$ConfirmDisposablePurge = ''
)

$ErrorActionPreference = 'Stop'

function Get-FileSha256 {
    param([string]$Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Get-DefaultInstallRoot {
    if ($env:LOCALAPPDATA) {
        return (Join-Path $env:LOCALAPPDATA 'SemanticMemoryMcp')
    }
    return (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'SemanticMemoryMcp')
}

function Get-PayloadRoot {
    param([string]$Root)
    $candidate = Join-Path $Root 'payload'
    if (Test-Path -LiteralPath (Join-Path $candidate 'semantic-memory-mcp.exe')) {
        return $candidate
    }
    return $Root
}

function Read-VersionFromPackage {
    param([string]$Root)
    $manifest = Join-Path $Root 'installer-manifest.json'
    if (Test-Path -LiteralPath $manifest) {
        return (Get-Content -Raw -LiteralPath $manifest | ConvertFrom-Json).version
    }
    $releaseManifest = Join-Path (Split-Path -Parent $Root) 'release-manifest.json'
    if (Test-Path -LiteralPath $releaseManifest) {
        return (Get-Content -Raw -LiteralPath $releaseManifest | ConvertFrom-Json).version
    }
    return 'unknown'
}

function Assert-ManagedRoot {
    param([string]$Root)
    $marker = Join-Path $Root '.semantic-memory-managed.json'
    if (-not (Test-Path -LiteralPath $marker)) {
        throw "InstallRoot is not managed by this installer: $Root"
    }
}

function Assert-DisposablePurgeRoot {
    param([string]$Root)
    $full = [System.IO.Path]::GetFullPath($Root)
    $needle = [System.IO.Path]::DirectorySeparatorChar + 'runtime-data' +
        [System.IO.Path]::DirectorySeparatorChar + 'stage13' + [System.IO.Path]::DirectorySeparatorChar
    if ($full.IndexOf($needle, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw 'PurgeData is restricted to explicit Stage13 disposable roots.'
    }
    if ($ConfirmDisposablePurge -ne 'DELETE-DISPOSABLE-DATA') {
        throw 'PurgeData requires ConfirmDisposablePurge=DELETE-DISPOSABLE-DATA.'
    }
}

function Write-Result {
    param([hashtable]$Payload)
    $Payload | ConvertTo-Json -Depth 8
}

if (-not $InstallRoot) {
    $InstallRoot = Get-DefaultInstallRoot
}

$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
$PayloadRoot = Get-PayloadRoot -Root $PackageRoot
$PayloadExe = Join-Path $PayloadRoot 'semantic-memory-mcp.exe'
$PayloadServerJson = Join-Path $PayloadRoot 'server.json'
$BinDir = Join-Path $InstallRoot 'bin'
$ManagerDir = Join-Path $InstallRoot 'manager-portable'
$DataDir = Join-Path $InstallRoot 'data'
$TargetExe = Join-Path $BinDir 'semantic-memory-mcp.exe'
$ManagerExe = Join-Path $ManagerDir 'semantic-memory-manager.exe'
$Marker = Join-Path $InstallRoot '.semantic-memory-managed.json'

if ($Action -in @('Install','Repair')) {
    if (-not (Test-Path -LiteralPath $PayloadExe)) {
        throw "Missing payload executable: $PayloadExe"
    }
    New-Item -ItemType Directory -Force -Path $BinDir,$ManagerDir,$DataDir | Out-Null
    Copy-Item -LiteralPath $PayloadExe -Destination $TargetExe -Force
    Copy-Item -LiteralPath $PayloadExe -Destination $ManagerExe -Force
    if (Test-Path -LiteralPath $PayloadServerJson) {
        Copy-Item -LiteralPath $PayloadServerJson -Destination (Join-Path $InstallRoot 'server.json') -Force
    }
    $payloadHash = Get-FileSha256 -Path $PayloadExe
    $markerPayload = [ordered]@{
        schema = 'stage13-local-install-marker/v1'
        version = Read-VersionFromPackage -Root $PackageRoot
        payload_sha256 = $payloadHash
        data_retained_by_default = $true
        registry_written = $false
        path_modified = $false
        service_installed = $false
        autostart_installed = $false
    }
    $markerPayload | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 -LiteralPath $Marker
}

if ($Action -eq 'Verify') {
    Assert-ManagedRoot -Root $InstallRoot
    if (-not (Test-Path -LiteralPath $TargetExe)) { throw "Missing installed exe: $TargetExe" }
    if (-not (Test-Path -LiteralPath $ManagerExe)) { throw "Missing manager exe: $ManagerExe" }
    $markerJson = Get-Content -Raw -LiteralPath $Marker | ConvertFrom-Json
    $targetHash = Get-FileSha256 -Path $TargetExe
    $managerHash = Get-FileSha256 -Path $ManagerExe
    $ok = ($targetHash -eq $markerJson.payload_sha256) -and ($managerHash -eq $markerJson.payload_sha256)
    if (-not $ok) { throw 'Installed payload hash mismatch.' }
}

if ($Action -eq 'Uninstall') {
    if ($PurgeData) {
        Assert-DisposablePurgeRoot -Root $InstallRoot
    }
    Assert-ManagedRoot -Root $InstallRoot
    if (Test-Path -LiteralPath $BinDir) { Remove-Item -LiteralPath $BinDir -Recurse -Force }
    if (Test-Path -LiteralPath $ManagerDir) { Remove-Item -LiteralPath $ManagerDir -Recurse -Force }
    if (Test-Path -LiteralPath (Join-Path $InstallRoot 'server.json')) {
        Remove-Item -LiteralPath (Join-Path $InstallRoot 'server.json') -Force
    }
    if (Test-Path -LiteralPath $Marker) { Remove-Item -LiteralPath $Marker -Force }
    if ($PurgeData) {
        if (Test-Path -LiteralPath $DataDir) { Remove-Item -LiteralPath $DataDir -Recurse -Force }
    }
}

Write-Result @{
    schema = 'stage13-installer-result/v1'
    action = $Action
    install_root = $InstallRoot
    data_dir_exists = (Test-Path -LiteralPath $DataDir)
    registry_written = $false
    path_modified = $false
    service_installed = $false
    autostart_installed = $false
    exit = 'PASS'
}
