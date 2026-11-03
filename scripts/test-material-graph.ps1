$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$output = Join-Path $workspace 'build/material-graph-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$json = 'External/RTXNS/external/donut/thirdparty/jsoncpp'
Push-Location -LiteralPath $workspace
try {
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /Gy /W4 /utf-8 /DNOMINMAX /I source /I source/rendering /I source/assets /I source/editor /I source/procedural /I External/ImGUI /I {1}/include tests/material_graph_tests.cpp source/assets/materialgraph.cpp source/assets/materialgraphevaluator.cpp source/assets/materialgraphcompiler.cpp source/editor/materialgraphbake.cpp source/assets/materialasset.cpp External/ImGUI/imgui.cpp External/ImGUI/imgui_draw.cpp External/ImGUI/imgui_tables.cpp External/ImGUI/imgui_widgets.cpp {1}/src/lib_json/json_reader.cpp {1}/src/lib_json/json_value.cpp {1}/src/lib_json/json_writer.cpp /Fe:build/material-graph-tests/tests.exe /Fo:build/material-graph-tests/ /link /OPT:REF user32.lib gdi32.lib imm32.lib && build\material-graph-tests\tests.exe' -f $developerCommand, $json
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Material graph tests failed.' }
    $dxc = Join-Path $workspace 'packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxc.exe'
    $shaderFailures = 0
    $shaders = @(Get-ChildItem -LiteralPath $output -Filter '*.hlsl')
    foreach ($shader in $shaders) {
        & $dxc -T ps_6_0 -E main -WX -Fo (Join-Path $output 'test.dxil') $shader.FullName
        if ($LASTEXITCODE) {
            Write-Output "Shader failed: $($shader.Name)"
            ++$shaderFailures
        }
    }
    if ($shaderFailures) { throw "$shaderFailures graph shaders failed." }
    Write-Output "Material graph shaders: PASS ($($shaders.Count))"
}
finally { Pop-Location }

