param([string]$CandidateDll = 'External/fastgltf/install-dx12-md/Release/bin/FastGltfAdapter.dll')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    if (-not (Test-Path bin/validation/fastgltf/optimization-before/FastGltfAdapter.dll)) {
        throw 'Preserve the original adapter DLL in bin/validation/fastgltf/optimization-before before editing.'
    }
    Copy-Item External/dll/assimp/assimp-vc143-mt.dll bin/validation/fastgltf -Force
    $command = 'cl /nologo /utf-8 /EHsc /std:c++20 /O2 /MT /Z7 /I . /I assimp tests/fastgltf_optimization_benchmark.cpp /Fo:bin/validation/fastgltf/ /Fe:bin/validation/fastgltf/optimization-benchmark.exe /link External/lib/assimp-vc143-mt.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'Optimization benchmark compilation failed.' }
    & ./bin/validation/fastgltf/optimization-benchmark.exe $CandidateDll
    if ($LASTEXITCODE) { throw 'Optimization benchmark failed.' }
}
finally { Pop-Location }
