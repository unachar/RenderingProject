param()

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sdkRoot = Join-Path $workspace 'External\DLSS\sdk'
$sdkCommit = '374959484e79a640feaba44c93ac8cfb0a03f5b5'
$files = @(
    'LICENSE.txt',
    'include/nvsdk_ngx.h',
    'include/nvsdk_ngx_defs.h',
    'include/nvsdk_ngx_params.h',
    'include/nvsdk_ngx_helpers_d3d.h',
    'lib/Windows_x86_64/x64/nvsdk_ngx_s.lib',
    'lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib',
    'lib/Windows_x86_64/rel/nvngx_dlss.dll'
)
$binaryHashes = @{
    'nvsdk_ngx_s.lib' = '4E5D355086D2BC11E1A0842457D2519EA528EE1F3E112C45679A84960C07DFF3'
    'nvsdk_ngx_s_dbg.lib' = 'F62F94F7A444DDFFA118E71BC412298D803784BF042464BE178A989915198CAB'
    'nvngx_dlss.dll' = '3975567B8943C53ACCE397F2B72380092F84F162D00B0D2C7D08A1025C563983'
}

foreach ($file in $files) {
    $destination = Join-Path $sdkRoot $file
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/NVIDIA/DLSS/$sdkCommit/$file" -OutFile $destination
    $name = Split-Path -Leaf $destination
    if ($binaryHashes.ContainsKey($name) -and (Get-FileHash -LiteralPath $destination).Hash -ne $binaryHashes[$name]) {
        throw "SDK binary hash mismatch: $name"
    }
}
$runtime = Join-Path $sdkRoot 'lib\Windows_x86_64\rel\nvngx_dlss.dll'
$signature = Get-AuthenticodeSignature -LiteralPath $runtime
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA Corporation') {
    throw 'The DLSS runtime did not pass NVIDIA signature verification.'
}
Set-Content -LiteralPath (Join-Path $sdkRoot 'sdk-commit.txt') -Value $sdkCommit
Write-Output "Installed NVIDIA DLSS SDK $sdkCommit"
Write-Output "License: $(Join-Path $sdkRoot 'LICENSE.txt')"
