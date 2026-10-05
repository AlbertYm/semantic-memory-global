[CmdletBinding()]
param([switch]$AllowRunningCodexForIsolatedTest,[string]$UserHome=$env:USERPROFILE,[string]$InstallRoot,[string]$CodexHome)
$ErrorActionPreference='Stop'
if(-not $InstallRoot){$InstallRoot=Join-Path $env:LOCALAPPDATA 'SemanticMemory'}
$state=Join-Path $InstallRoot 'install-bundle-state.json'
$target = 'Install-Bundle.ps1'
if (Test-Path -LiteralPath $state -PathType Leaf) {
    $installed = Get-Content -LiteralPath $state -Encoding UTF8 -Raw | ConvertFrom-Json
    $package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'RELEASE.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    $pointerPath = Join-Path $InstallRoot 'state\current.json'
    if (Test-Path -LiteralPath $pointerPath) {
        $pointer = Get-Content -LiteralPath $pointerPath -Encoding UTF8 -Raw | ConvertFrom-Json
        if ($installed.version -eq $package.version -and $pointer.version_id -eq $package.runtime_version_id) { $target = 'Repair-Bundle.ps1' }
    }
}
$argsList=@('-UserHome',$UserHome,'-InstallRoot',$InstallRoot)
if($CodexHome){$argsList+=@('-CodexHome',$CodexHome)}
if($AllowRunningCodexForIsolatedTest){$argsList+='-AllowRunningCodexForIsolatedTest'}
& powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $target) @argsList
exit $LASTEXITCODE

