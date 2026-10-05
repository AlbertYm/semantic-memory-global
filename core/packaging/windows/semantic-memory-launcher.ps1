param(
    [ValidateSet('mcp','hook','manager')][string]$Mode = 'mcp',
    [ValidateSet('recall','post-tool','stop')][string]$HookAction,
    [string]$InstallRoot,
    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'
[void][Reflection.Assembly]::LoadWithPartialName('System.Web.Extensions')
$script:SmLauncherJson = [Web.Script.Serialization.JavaScriptSerializer]::new()
$script:SmLauncherJson.MaxJsonLength = 1048576
$script:SmLauncherSha256 = [Security.Cryptography.SHA256]::Create()

function Get-SmLauncherSha256 {
    param([Parameter(Mandatory=$true)][string]$Path)
    $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try {
        return ([BitConverter]::ToString($script:SmLauncherSha256.ComputeHash($stream))).Replace('-','').ToLowerInvariant()
    } finally {
        $stream.Dispose()
    }
}

function Read-SmLauncherJson {
    param([Parameter(Mandatory=$true)][string]$Path)
    return $script:SmLauncherJson.DeserializeObject([IO.File]::ReadAllText($Path,[Text.Encoding]::UTF8))
}

function Resolve-SmLauncherChildPath {
    param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$RelativePath)
    if([string]::IsNullOrWhiteSpace($RelativePath)-or[IO.Path]::IsPathRooted($RelativePath)){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: unsafe relative payload path.'}
    $normalized=$RelativePath.Replace('/','\')
    if($normalized -match '(^|\\)\.\.?(\\|$)'){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: unsafe relative payload path.'}
    $prefix=[IO.Path]::GetFullPath($Root).TrimEnd('\')+'\'
    $candidate=[IO.Path]::GetFullPath([IO.Path]::Combine($prefix,$normalized))
    if(-not$candidate.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: payload path escaped its root.'}
    return $candidate
}

if(-not$InstallRoot){
    $local=$env:LOCALAPPDATA
    if(-not$local){$local=[Environment]::GetFolderPath('LocalApplicationData')}
    $InstallRoot=[IO.Path]::Combine($local,'SemanticMemory')
}
$InstallRoot=[IO.Path]::GetFullPath($InstallRoot)
$pointerPath=[IO.Path]::Combine($InstallRoot,'state\current.json')
$pointer=Read-SmLauncherJson $pointerPath
if($pointer.schema-ne'stage14-current-pointer/v1'-or([string]$pointer.version_id)-notmatch'^[A-Za-z0-9._+-]+$'){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: invalid current pointer.'}
foreach($field in @('manifest_sha256','receipt_sha256')){if(([string]$pointer.$field)-notmatch'^[0-9a-f]{64}$'){throw "RED_PAYLOAD_INTEGRITY_FAILURE: invalid pointer $field."}}

$payloadRoot=Resolve-SmLauncherChildPath -Root $InstallRoot -RelativePath ("app/versions/{0}"-f$pointer.version_id)
$manifestPath=[IO.Path]::Combine($payloadRoot,'payload-manifest.json')
$receiptPath=[IO.Path]::Combine($payloadRoot,'verification-receipt.json')
$manifestSha=Get-SmLauncherSha256 $manifestPath
$receiptSha=Get-SmLauncherSha256 $receiptPath
if($manifestSha-ne$pointer.manifest_sha256-or$receiptSha-ne$pointer.receipt_sha256){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: pointer binding mismatch.'}

$manifest=Read-SmLauncherJson $manifestPath
if($manifest.schema-ne'stage14-payload-manifest/v1'-or$manifest.version_id-ne$pointer.version_id-or-not$manifest.files){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: invalid payload manifest.'}
$manifestFiles=@($manifest.files)
if($manifestFiles.Count-eq0){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: manifest file set is empty.'}

for($i=0;$i-lt$manifestFiles.Count;$i++){
    $record=$manifestFiles[$i]
    if(([string]$record.sha256)-notmatch'^[0-9a-f]{64}$'-or[long]$record.bytes-lt0){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: invalid manifest file record.'}
    $file=Resolve-SmLauncherChildPath -Root $payloadRoot -RelativePath ([string]$record.path)
    if(-not[IO.File]::Exists($file)){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: payload file is missing.'}
    $info=[IO.FileInfo]::new($file)
    if($info.Length-ne[long]$record.bytes-or(Get-SmLauncherSha256 $file)-ne$record.sha256){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: full payload SHA256 verification failed.'}
}

$entryName=$Mode
if($Mode-eq'hook'){
    if(-not$HookAction){throw 'Hook mode requires -HookAction.'}
    $entryName='hook'
}
$relative=[string]$manifest.entrypoints.$entryName
if(-not$relative){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: payload entrypoint is missing.'}
$executable=Resolve-SmLauncherChildPath -Root $payloadRoot -RelativePath $relative
if(-not[IO.File]::Exists($executable)){throw 'RED_PAYLOAD_INTEGRITY_FAILURE: payload entrypoint file is missing.'}

if($VerifyOnly){$script:SmLauncherSha256.Dispose();[Console]::Out.WriteLine('PASS_FULL_SHA256');exit 0}

$env:CBM_DATA_ROOT=Join-Path $InstallRoot 'data'
$env:CBM_CACHE_DIR=Join-Path $InstallRoot 'data'
$env:CBM_ARTIFACT_DIR=Join-Path $InstallRoot 'data\artifacts'
$env:CBM_MEMORY_EMBED_BACKEND='static'
$env:CBM_MEMORY_AUTO_MAINTAIN='0'

$arguments=@()
if($Mode-eq'manager'){$arguments=@('manager')}
elseif($Mode-eq'hook'){$hookMap=@{recall='memory-recall';'post-tool'='memory-post-tool';stop='memory-stop'};$arguments=@($hookMap[$HookAction])}

$script:SmLauncherSha256.Dispose()
& $executable @arguments
exit $LASTEXITCODE
