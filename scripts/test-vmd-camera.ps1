param([switch]$RuntimeFixture)

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
    New-Item -ItemType Directory -Path bin/validation -Force | Out-Null
    $includes = '/I source /I source/ecs /I source/models /I source/rendering /I source/physics /I source/scene /I source/assets /I source/core /I assimp /I External/PhysX/physx/include /I External/ImGUI /I External/DirectXTex/DirectXTex'
    $includes += ' /I source/entities /I source/procedural'
    $includes += ' /Zc:inline'
    $jsonRoot = 'External\RTXNS\external\donut\thirdparty\jsoncpp'
    $command = 'cl.exe /nologo /utf-8 /EHsc /std:c++20 /O2 /Gy /DNOMINMAX {0} /I {1}\include tests\vmd_camera_tests.cpp source\models\vmdanimationimpoter.cpp source\models\animator.cpp {1}\src\lib_json\json_reader.cpp {1}\src\lib_json\json_value.cpp {1}\src\lib_json\json_writer.cpp /Fe:bin\validation\vmd_camera_tests.exe /Fo:bin\validation\ /link /OPT:REF /LIBPATH:packages\directxtex_desktop_win10.2025.10.28.1\native\lib\x64\Release' -f $includes, $jsonRoot
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE -ne 0) {
        throw 'VMD camera test compilation failed.'
    }
    $testArguments = @()
    if ($RuntimeFixture) {
        $testArguments += '--runtime-fixture'
    }
    & ./bin/validation/vmd_camera_tests.exe @testArguments
    if ($LASTEXITCODE -ne 0) {
        throw 'VMD camera tests failed.'
    }
    $gpuCommand = 'cl.exe /nologo /utf-8 /EHsc /W4 /WX /std:c++20 /O2 /I source tests\camera_projection_gpu_tests.cpp /Fe:bin\validation\camera_projection_gpu_tests.exe /Fo:bin\validation\camera_projection_gpu_tests.obj /link d3d11.lib d3dcompiler.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $gpuCommand)
    if ($LASTEXITCODE -ne 0) {
        throw 'Camera projection GPU test compilation failed.'
    }
    & ./bin/validation/camera_projection_gpu_tests.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'Camera projection GPU tests failed.'
    }
}
finally {
    Pop-Location
}

