param([ValidateSet('Static', 'Shared')][string]$Runtime = 'Shared')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    New-Item -ItemType Directory bin/validation/fastgltf -Force | Out-Null
    $suffix = if ($Runtime -eq 'Shared') { '-md' } else { '' }
    $compilerRuntime = if ($Runtime -eq 'Shared') { '/MD' } else { '/MT' }
    $command = 'cl /nologo /utf-8 /EHsc /std:c++20 /O2 {0} /Z7 /I External/fastgltf/install-dx12{1}/Release/include tests/fastgltf_sdk_probe.cpp /Fe:bin/validation/fastgltf/sdk-probe.exe /Fo:bin/validation/fastgltf/ /link /LIBPATH:External/fastgltf/install-dx12{1}/Release/lib fastgltf.lib fastgltf_simdjson.lib fastgltf_simdutf.lib' -f $compilerRuntime, $suffix
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'fastgltf SDK probe compilation failed.' }
    foreach ($asset in @('Box.glb', 'TriangleWithoutIndices.gltf')) {
        & ./bin/validation/fastgltf/sdk-probe.exe "External/RTXNS/external/donut/thirdparty/cgltf/fuzz/data/$asset"
        if ($LASTEXITCODE) { throw "fastgltf SDK probe failed: $asset" }
    }
}
finally { Pop-Location }
