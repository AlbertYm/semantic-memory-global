[CmdletBinding()]
param(
    [string]$UserHome = $env:USERPROFILE,
    [string]$InstallRoot,
    [string]$CodexHome
)
$ErrorActionPreference = 'Stop'
if (-not $InstallRoot) { $InstallRoot = Join-Path $env:LOCALAPPDATA 'SemanticMemory' }
if (-not $CodexHome) { $CodexHome = Join-Path $UserHome '.codex' }
$checks = [ordered]@{}
try {
    $runtimeOut = @(& powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Install-SemanticMemoryV2.ps1') -Action Verify -InstallRoot $InstallRoot 2>&1)
    $checks.runtime = ($LASTEXITCODE -eq 0 -and (($runtimeOut -join "`n") | ConvertFrom-Json).status -eq 'PASS')
    $pluginOut = @(& powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Install-SemanticMemoryPlugin.ps1') -Action Verify -PackageRoot $PSScriptRoot -UserHome $UserHome -CodexHome $CodexHome -StateRoot (Join-Path $UserHome 'AppData\Local\SemanticMemory\backups\codex-plugin') 2>&1)
    $checks.plugin = ($LASTEXITCODE -eq 0 -and (($pluginOut -join "`n") | ConvertFrom-Json).status -eq 'PASS')
    $configOut = @(& powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Repair-SemanticMemory.ps1') -Mode Verify -ConfigPath (Join-Path $CodexHome 'config.toml') -InstallRoot $InstallRoot 2>&1)
    $checks.config = ($LASTEXITCODE -eq 0 -and (($configOut -join "`n") | ConvertFrom-Json).status -eq 'PASS')
    $launcherOut = @(& powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $InstallRoot 'bin\semantic-memory-launcher.ps1') -Mode mcp -InstallRoot $InstallRoot -VerifyOnly 2>&1)
    $checks.full_payload_sha256 = ($LASTEXITCODE -eq 0 -and ($launcherOut -join "`n").Trim() -eq 'PASS_FULL_SHA256')
    $priorDataRoot = $env:CBM_DATA_ROOT
    $priorCacheDir = $env:CBM_CACHE_DIR
    $priorArtifactDir = $env:CBM_ARTIFACT_DIR
    try {
        $env:CBM_DATA_ROOT = Join-Path $InstallRoot 'data'
        $env:CBM_CACHE_DIR = Join-Path $InstallRoot 'data'
        $env:CBM_ARTIFACT_DIR = Join-Path $InstallRoot 'data\artifacts'
        $messages = @(
            '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"semantic-memory-transfer-verifier","version":"1.0"}}}',
            '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}',
            '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
        )
        $protocolLines = @($messages | & (Join-Path $InstallRoot 'bin\semantic-memory-mcp.exe') 2>&1)
        $protocolCode = $LASTEXITCODE
        $responses = @($protocolLines | ForEach-Object { ([string]$_) | ConvertFrom-Json })
        $init = @($responses | Where-Object { $_.id -eq 1 })[0]
        $toolResponse = @($responses | Where-Object { $_.id -eq 2 })[0]
        $toolNames = @($toolResponse.result.tools | ForEach-Object { [string]$_.name })
        $checks.mcp_initialize = ($protocolCode -eq 0 -and $init.result.protocolVersion -eq '2025-06-18')
        $checks.mcp_tools_list = ($toolNames.Count -ge 15 -and
            $toolNames -contains 'memories_retrieve' -and
            $toolNames -contains 'memory_task_status' -and
            $toolNames -contains 'memory_task_complete')
    } finally {
        if ($null -eq $priorDataRoot) { Remove-Item Env:CBM_DATA_ROOT -ErrorAction SilentlyContinue } else { $env:CBM_DATA_ROOT = $priorDataRoot }
        if ($null -eq $priorCacheDir) { Remove-Item Env:CBM_CACHE_DIR -ErrorAction SilentlyContinue } else { $env:CBM_CACHE_DIR = $priorCacheDir }
        if ($null -eq $priorArtifactDir) { Remove-Item Env:CBM_ARTIFACT_DIR -ErrorAction SilentlyContinue } else { $env:CBM_ARTIFACT_DIR = $priorArtifactDir }
    }
    $checks.state_file = Test-Path -LiteralPath (Join-Path $InstallRoot 'install-bundle-state.json') -PathType Leaf
    $pass = -not ($checks.Values -contains $false)
    [ordered]@{schema='semantic-memory-transfer-verification/v2';status=$(if($pass){'PASS_AUTOMATED_CORE'}else{'FAIL'});checks=$checks;manual_acceptance=[ordered]@{hook_trust='CHECK_IN_CODEX_DESKTOP';app_mcp_namespace='CHECK_AFTER_RESTART';automatic_recall='CHECK_IN_NEW_TASK';automatic_evidence='CHECK_IN_NEW_TASK';answer_visibility='CHECK_IN_NEW_TASK'};verified_at=[DateTimeOffset]::Now.ToString('o')} | ConvertTo-Json -Depth 8
    if (-not $pass) { exit 1 }
} catch {
    [ordered]@{schema='semantic-memory-transfer-verification/v2';status='FAIL';checks=$checks;error=$_.Exception.Message} | ConvertTo-Json -Depth 6
    exit 1
}
