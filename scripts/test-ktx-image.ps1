param(
    [switch]$Benchmark,
    [switch]$Disabled,
    [string]$Cache = 'Data/Cache/Textures',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    New-Item -ItemType Directory -Path bin/validation/ktx -Force | Out-Null
    $loaders = Get-Content source/assets/texturemanager.cpp -Raw
    $loaderStart = $loaders.IndexOf('HRESULT LoadImageFromFile(')
    $loaderEnd = $loaders.IndexOf('void EnsureMipChain(', $loaderStart)
    $thumbnail = Get-Content source/rendering/thumbnailrenderer.cpp -Raw
    $thumbnailStart = $thumbnail.IndexOf('bool Decode(ThumbnailSourceImage&')
    $thumbnailEnd = $thumbnail.IndexOf('ThumbnailPrepared::MeshPart ReadMeshMaterial(', $thumbnailStart)
    $assets = Get-Content source/editor/editorassets.cpp -Raw
    $assetStart = $assets.IndexOf('bool ImGuiManager::IsTextureFile(')
    $assetEnd = $assets.IndexOf('bool ImGuiManager::IsModelFile(', $assetStart)
    if ($loaderStart -lt 0 -or $loaderEnd -lt 0 -or $thumbnailStart -lt 0 -or $thumbnailEnd -lt 0 -or $assetStart -lt 0 -or $assetEnd -lt 0) {
        throw 'Cannot locate production texture import/thumbnail/classification methods.'
    }
    Set-Content bin/validation/ktx/texture-image-loaders.inc -Value $loaders.Substring($loaderStart, $loaderEnd - $loaderStart) -Encoding utf8
    Set-Content bin/validation/ktx/texture-thumbnail-decode.inc -Value $thumbnail.Substring($thumbnailStart, $thumbnailEnd - $thumbnailStart) -Encoding utf8
    Set-Content bin/validation/ktx/texture-file-classification.inc -Value $assets.Substring($assetStart, $assetEnd - $assetStart) -Encoding utf8
    $name = 'ktx_image_tests'
    if ($Benchmark) { $name = 'ktx_image_benchmark' }
    if ($Disabled) { $name = 'ktx_disabled_tests' }
    $flags = '/O2 /MT'
    if ($Configuration -eq 'Debug') { $flags = '/Od /MTd /RTC1' }
    $enabled = [int](-not $Disabled)
    $sdkInclude = ''
    $sdkLibrary = ''
    if (-not $Disabled) {
        $sdkInclude = '/I External/KTX-Software/install-dx12/include'
        $sdkLibrary = 'External/KTX-Software/install-dx12/lib/ktx.lib'
    }
    $command = 'cl /nologo /utf-8 /EHsc /std:c++17 {1} /Z7 /DRENDERINGPROJECT_USE_KTX={3} /I source /I source/rendering /I bin/validation/ktx /I External/DirectXTex/DirectXTex {4} tests/{0}.cpp source/rendering/ktxbasis.cpp /Fe:bin/validation/ktx/{0}-{2}.exe /Fo:bin/validation/ktx/ /link External/DirectXTex/DirectXTex/Bin/Desktop_2022/x64/{2}/DirectXTex.lib {5} d3d11.lib dxgi.lib dxguid.lib ole32.lib windowscodecs.lib' -f $name, $flags, $Configuration, $enabled, $sdkInclude, $sdkLibrary
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'KTX image compilation failed.' }
    if (-not $Disabled) { Copy-Item External/KTX-Software/install-dx12/bin/ktx.dll bin/validation/ktx -Force }
    & "./bin/validation/ktx/$name-$Configuration.exe" $Cache
    if ($LASTEXITCODE) { throw 'KTX image tests failed.' }
}
finally { Pop-Location }
