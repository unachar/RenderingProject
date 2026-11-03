param([switch]$Warp)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$output = Join-Path $workspace 'build/material-graph-gpu'
New-Item -ItemType Directory -Force -Path $output | Out-Null
Push-Location -LiteralPath $workspace
try {
    $json = 'External/RTXNS/external/donut/thirdparty/jsoncpp'
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 /DNOMINMAX /I source /I source/rendering /I source/assets /I External/ImGUI /I {1}/include tests/material_graph_gpu_tests.cpp source/assets/materialgraph.cpp source/assets/materialgraphcompiler.cpp source/assets/materialasset.cpp {1}/src/lib_json/json_reader.cpp {1}/src/lib_json/json_value.cpp {1}/src/lib_json/json_writer.cpp /Fe:build/material-graph-gpu/tests.exe /Fo:build/material-graph-gpu/ /link d3d12.lib dxgi.lib d3dcompiler.lib && build\material-graph-gpu\tests.exe --generate' -f $developer, $json
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'GPU graph harness build failed.' }
    $dxc = Join-Path $workspace 'packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxc.exe'
    foreach ($shader in (Get-ChildItem -LiteralPath $output -Filter '*.hlsl')) {
        & $dxc -T cs_6_0 -E main -O3 -WX -Fo ([IO.Path]::ChangeExtension($shader.FullName, '.cso')) $shader.FullName
        if ($LASTEXITCODE) { throw "GPU graph shader failed: $($shader.Name)" }
    }
    if ($Warp) { & (Join-Path $output 'tests.exe') --warp }
    else { & (Join-Path $output 'tests.exe') }
    if ($LASTEXITCODE) { throw 'GPU graph evaluation failed.' }
}
finally { Pop-Location }
