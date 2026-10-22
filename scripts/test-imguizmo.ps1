$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'Visual Studio C++ tools are required.'
}
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Path bin/validation/imguizmo -Force | Out-Null
    $command = 'cl.exe /nologo /utf-8 /EHsc /W3 /std:c++17 /O2 /I source /I External\ImGUI /I External\ImGuizmo tests\imguizmo_transform_tests.cpp External\ImGUI\imgui.cpp External\ImGUI\imgui_draw.cpp External\ImGUI\imgui_tables.cpp External\ImGUI\imgui_widgets.cpp /Fe:bin\validation\imguizmo\tests.exe /Fo:bin\validation\imguizmo\ /link user32.lib gdi32.lib imm32.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE -ne 0) {
        throw 'ImGuizmo regression test compilation failed.'
    }
    & ./bin/validation/imguizmo/tests.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'ImGuizmo regression tests failed.'
    }
}
finally {
    Pop-Location
}
