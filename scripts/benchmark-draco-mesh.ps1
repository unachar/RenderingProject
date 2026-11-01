$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    New-Item -ItemType Directory bin/validation/draco -Force | Out-Null
    $command = 'cl /nologo /utf-8 /EHsc /std:c++20 /O2 /MT /Z7 /I . /I External/Draco/install-dx12/include tests/draco_mesh_benchmark.cpp External/meshoptimizer/src/indexcodec.cpp External/meshoptimizer/src/vertexcodec.cpp External/meshoptimizer/src/allocator.cpp External/zstd/zstd.c /Fo:bin/validation/draco/ /Fe:bin/validation/draco/mesh-benchmark.exe /link External/Draco/install-dx12/lib/draco.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'Draco mesh benchmark compilation failed.' }
    & ./bin/validation/draco/mesh-benchmark.exe
    if ($LASTEXITCODE) { throw 'Draco mesh comparison failed.' }
}
finally { Pop-Location }
