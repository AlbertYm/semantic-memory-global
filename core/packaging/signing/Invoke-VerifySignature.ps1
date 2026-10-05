param(
    [Parameter(Mandatory=$true)][string]$FilePath,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
$signature = Get-AuthenticodeSignature -LiteralPath $FilePath
$signed = $signature.Status -eq 'Valid'
$result = [ordered]@{
    schema = 'stage13-signature-verification/v1'
    file = Split-Path -Leaf $FilePath
    authenticode_status = $(if ($signed) { 'signed_valid' } else { 'not_signed' })
    powershell_status = [string]$signature.Status
    public_release_ready = $false
}
if ($Output) {
    $result | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath $Output
}
$result | ConvertTo-Json -Depth 8
