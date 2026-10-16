param()
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$downloads = Join-Path $workspace 'build/optix-sdk/install'
New-Item -ItemType Directory -Force $downloads | Out-Null

function Install-Headers($Name, $Url, $Hash, $ArchiveRoot, $Destination) {
    $archive = Join-Path $downloads "$Name.zip"
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri $Url -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Hash) {
        throw "Checksum mismatch: $archive. Remove this archive and retry."
    }
    $expanded = Join-Path $downloads $Name
    Expand-Archive -LiteralPath $archive -DestinationPath $expanded -Force
    $source = Join-Path $expanded $ArchiveRoot
    $target = Join-Path $workspace $Destination
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item -LiteralPath (Join-Path $source 'include') -Destination $target -Recurse -Force
    Get-ChildItem -LiteralPath $source -File | Where-Object { $_.Name -match 'LICENSE|EULA|copyright' } |
        Copy-Item -Destination $target -Force
    Write-Output "Installed $Name headers: $target"
}

# Project-local headers only; CUDA is loaded from the NVIDIA display driver.
Install-Headers 'optix' 'https://codeload.github.com/NVIDIA/optix-dev/zip/refs/tags/v9.0.0' `
    '461664FDA99A901D364996BF048A3DAE399CE1162C725E5C1855C336C6C9D0A8' `
    'optix-dev-9.0.0' 'External/OptiX/sdk'
Install-Headers 'cuda-cudart' 'https://developer.download.nvidia.com/compute/cuda/redist/cuda_cudart/windows-x86_64/cuda_cudart-windows-x86_64-12.9.79-archive.zip' `
    '179E9C43B0735FFE67207B3DA556EB5A0C50F3047961882B7657D3B822D34EF8' `
    'cuda_cudart-windows-x86_64-12.9.79-archive' 'External/CUDA/sdk'
