$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    New-Item -ItemType Directory bin/validation/draco -Force | Out-Null
    Copy-Item External/fastgltf/install-dx12/Release/bin/FastGltfAdapter.dll bin/validation/draco -Force
    Copy-Item External/dll/assimp/assimp-vc143-mt.dll bin/validation/draco -Force
    $command = 'cl /nologo /utf-8 /EHsc /std:c++20 /O2 /MT /Z7 /DRENDERINGPROJECT_USE_FASTGLTF=1 /I . /I assimp tests/draco_gltf_benchmark.cpp /Fo:bin/validation/draco/ /Fe:bin/validation/draco/gltf-benchmark.exe /link /LIBPATH:External/fastgltf/install-dx12/Release/lib FastGltfAdapter.lib External/lib/assimp-vc143-mt.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'Draco import benchmark compilation failed.' }
    & ./bin/validation/draco/gltf-benchmark.exe
    if ($LASTEXITCODE) { throw 'Draco import benchmark failed.' }
}
finally { Pop-Location }
