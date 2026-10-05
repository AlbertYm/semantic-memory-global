param(
    [Parameter(Mandatory=$true)][string]$FilePath,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
$signature = Get-AuthenticodeSignature -LiteralPath $FilePath
$result = [ordered]@{
    schema = 'stage13-code-signing-status/v1'
    file = Split-Path -Leaf $FilePath
    authenticode_status = 'not_signed'
    powershell_status = [string]$signature.Status
    public_release_ready = $false
    trusted_certificate_available = $false
    signing_performed = $false
}
if ($Output) {
    $result | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath $Output
}
$result | ConvertTo-Json -Depth 8
