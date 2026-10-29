$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force build/model-map-tests | Out-Null
    $jsonRoot = 'External\RTXNS\external\donut\thirdparty\jsoncpp'
    foreach ($test in @('model_resource_map', 'model_map_import', 'model_map_json', 'vmd_retarget')) {
        $command = 'cl /nologo /std:c++20 /EHsc /utf-8 /DNOMINMAX /I source /I assimp /I {0}\include tests\{1}_tests.cpp {0}\src\lib_json\json_reader.cpp {0}\src\lib_json\json_value.cpp {0}\src\lib_json\json_writer.cpp /Fe:build\model-map-tests\{1}.exe /Fo:build\model-map-tests\' -f $jsonRoot, $test
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
        if ($LASTEXITCODE) { throw "$test compilation failed." }
        & "./build/model-map-tests/$test.exe"
        if ($LASTEXITCODE) { throw "$test failed." }
    }
}
finally { Pop-Location }
