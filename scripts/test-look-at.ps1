$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $root
try {
    New-Item -ItemType Directory -Force bin/validation/look-at | Out-Null
    $json = 'External/RTXNS/external/donut/thirdparty/jsoncpp'
    foreach ($test in @('look_at', 'look_at_fixture')) {
        $command = 'cl.exe /nologo /utf-8 /EHsc /W4 /std:c++20 /DNOMINMAX /I source /I assimp /I {0}/include tests/{1}_tests.cpp {0}/src/lib_json/json_reader.cpp {0}/src/lib_json/json_value.cpp {0}/src/lib_json/json_writer.cpp /Fe:bin/validation/look-at/{1}.exe /Fo:bin/validation/look-at/' -f $json, $test
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
        if ($LASTEXITCODE) { throw "$test compilation failed." }
        & "./bin/validation/look-at/$test.exe"
        if ($LASTEXITCODE) { throw "$test failed." }
    }
}
finally { Pop-Location }
