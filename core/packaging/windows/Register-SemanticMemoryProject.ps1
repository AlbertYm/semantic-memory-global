param(
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    [Parameter(Mandatory=$true)][string]$ExePath,
    [ValidateSet('Preview','Apply')][string]$Mode = 'Preview',
    [string]$BackupDir
)

$ErrorActionPreference = 'Stop'

function Get-FileSha256 {
    param([string]$Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
$ExePath = [System.IO.Path]::GetFullPath($ExePath)
$config = Join-Path $ProjectRoot '.codex\config.toml'
$hooks = Join-Path $ProjectRoot '.codex\hooks.json'
if (-not (Test-Path -LiteralPath $config)) { throw "Missing project config: $config" }
if (-not (Test-Path -LiteralPath $hooks)) { throw "Missing project hooks: $hooks" }
if (-not (Test-Path -LiteralPath $ExePath)) { throw "Missing release exe: $ExePath" }

$oldConfigSha = Get-FileSha256 -Path $config
$oldHooksSha = Get-FileSha256 -Path $hooks
$oldConfig = Get-Content -Raw -LiteralPath $config
$oldHooks = Get-Content -Raw -LiteralPath $hooks
$oldConfigExe = 'H:\Codex_H\release\semantic-memory-mcp\stage12-product-beta-v3\semantic-memory-mcp.exe'
$oldHooksExe = 'H:\\Codex_H\\release\\semantic-memory-mcp\\stage12-product-beta-v3\\semantic-memory-mcp.exe'
$newHooksExe = $ExePath -replace '\\','\\'
$newConfig = $oldConfig -replace [regex]::Escape($oldConfigExe), $ExePath
$newHooks = $oldHooks -replace [regex]::Escape($oldHooksExe), $newHooksExe

$checks = [ordered]@{
    config_has_safe_env = ($newConfig -match "CBM_MEMORY_AUTO_MAINTAIN = '0'") -and ($newConfig -match "CBM_MEMORY_NO_GLOBAL_UNION = '1'")
    hooks_have_safe_env = ($newHooks -match 'CBM_MEMORY_AUTO_MAINTAIN=0') -and ($newHooks -match 'CBM_MEMORY_NO_GLOBAL_UNION=1')
    global_config_written = $false
    exe_sha256 = Get-FileSha256 -Path $ExePath
}

if ($Mode -eq 'Apply') {
    if (-not $BackupDir) {
        throw 'Apply requires BackupDir.'
    }
    New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
    Copy-Item -LiteralPath $config -Destination (Join-Path $BackupDir 'config.toml.before') -Force
    Copy-Item -LiteralPath $hooks -Destination (Join-Path $BackupDir 'hooks.json.before') -Force
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($config, $newConfig, $utf8NoBom)
    [System.IO.File]::WriteAllText($hooks, $newHooks, $utf8NoBom)
}

[ordered]@{
    schema = 'stage13-project-registration/v1'
    mode = $Mode
    project_root = $ProjectRoot
    exe_path = $ExePath
    old_config_sha256 = $oldConfigSha
    old_hooks_sha256 = $oldHooksSha
    current_config_sha256 = Get-FileSha256 -Path $config
    current_hooks_sha256 = Get-FileSha256 -Path $hooks
    checks = $checks
} | ConvertTo-Json -Depth 8
