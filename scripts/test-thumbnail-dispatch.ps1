param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    python tests/thumbnail_material_contract_tests.py --emit-dispatch
    if ($LASTEXITCODE) { throw 'Could not extract the production scheduling branch.' }
    $flags = '/Od /RTC1'
    if ($Configuration -eq 'Release') { $flags = '/O2' }
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 {1} /I build\thumbnail-tests tests\thumbnail_dispatch_tests.cpp /Fe:build\thumbnail-tests\dispatch-{2}.exe /Fo:build\thumbnail-tests\ && build\thumbnail-tests\dispatch-{2}.exe' -f $developerCommand, $flags, $Configuration
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Thumbnail dispatch tests failed.' }
    $dxc = 'packages\Microsoft.Direct3D.DXC.1.9.2602.24\build\native'
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 {1} /I source /I {2}\include tests\thumbnail_compiler_tests.cpp /Fe:build\thumbnail-tests\compiler-{3}.exe /Fo:build\thumbnail-tests\ /link /LIBPATH:{2}\lib\x64 d3dcompiler.lib' -f $developerCommand, $flags, $dxc, $Configuration
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Thumbnail compiler test build failed.' }
    foreach ($library in @('dxcompiler.dll', 'dxil.dll')) {
        Copy-Item -LiteralPath "$dxc\bin\x64\$library" -Destination build/thumbnail-tests -Force
    }
    & "./build/thumbnail-tests/compiler-$Configuration.exe"
    if ($LASTEXITCODE) { throw 'Thumbnail compiler isolation tests failed.' }
}
finally {
    Pop-Location
}
